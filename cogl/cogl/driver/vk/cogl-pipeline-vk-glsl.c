/*
 * Copyright (C) 2026 Ultranix
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include "cogl/driver/vk/cogl-pipeline-vk-glsl-private.h"
#include "cogl/driver/vk/cogl-pipeline-vk-private.h"
#include "cogl/driver/vk/cogl-pipeline-vk-assemble-private.h"
#include "cogl/driver/vk/cogl-driver-vk-private.h"
#include "cogl/driver/vk/cogl-vk-debug-private.h"

#include "cogl/cogl-pipeline-layer-private.h"
#include "cogl/cogl-pipeline-layer-state.h"
#include "cogl/cogl-sampler-cache-private.h"
#include "cogl/cogl-pipeline-private.h"
#include "cogl/cogl-pipeline-state-private.h"
#include "cogl/cogl-pipeline-state.h"
#include "cogl/cogl-pipeline-snippet-private.h"
#include "cogl/cogl-snippet.h"
#include "cogl/cogl-context-private.h"
#include "cogl/cogl-framebuffer.h"

#include <gio/gio.h>
#include <graphene.h>
#include <string.h>

#ifdef HAVE_SHADERC
#include <shaderc/shaderc.h>
#endif

/* Simple Cogl draws use embedded shaders. Texture layer combines and snippet
 * hooks require generated GLSL, including the color-management snippets used
 * by Mutter. Shader modules and graphics pipelines are cached by their
 * complete inputs; state changes select a new cache entry.
 */

typedef struct _CoglVkSpirvModuleEntry
{
  char *key;
  VkShaderModule module;
} CoglVkSpirvModuleEntry;

typedef struct _CoglVkSpirvPipeEntry
{
  char *key;
  VkPipeline pipeline;
  gboolean owns_pipeline; /* FALSE if aliasing hot format_setup pipe */
} CoglVkSpirvPipeEntry;

struct _CoglVkSpirvCache
{
  CoglVkDevice *device; /* unowned */
  GHashTable *modules; /* key → CoglVkSpirvModuleEntry* */
  GHashTable *pipes;   /* key → CoglVkSpirvPipeEntry* */
  char *disk_cache_dir; /* $XDG_CACHE_HOME/mutter/cogl-vk-spirv */
#ifdef HAVE_SHADERC
  shaderc_compiler_t compiler;
#endif
};

static gboolean
cogl_vk_layer_combine_is_default (CoglPipelineLayer *layer)
{
  CoglPipelineLayer *auth;
  CoglPipelineLayerBigState *bs;

  if (!layer)
    return TRUE;

  auth = _cogl_pipeline_layer_get_authority (layer,
                                             COGL_PIPELINE_LAYER_STATE_COMBINE);
  if (!auth || !auth->has_big_state || !auth->big_state)
    return TRUE;

  bs = auth->big_state;
  return bs->texture_combine_rgb_func == COGL_PIPELINE_COMBINE_FUNC_MODULATE &&
         bs->texture_combine_rgb_src[0] == COGL_PIPELINE_COMBINE_SOURCE_PREVIOUS &&
         bs->texture_combine_rgb_src[1] == COGL_PIPELINE_COMBINE_SOURCE_TEXTURE &&
         bs->texture_combine_rgb_op[0] == COGL_PIPELINE_COMBINE_OP_SRC_COLOR &&
         bs->texture_combine_rgb_op[1] == COGL_PIPELINE_COMBINE_OP_SRC_COLOR &&
         bs->texture_combine_alpha_func == COGL_PIPELINE_COMBINE_FUNC_MODULATE &&
         bs->texture_combine_alpha_src[0] == COGL_PIPELINE_COMBINE_SOURCE_PREVIOUS &&
         bs->texture_combine_alpha_src[1] == COGL_PIPELINE_COMBINE_SOURCE_TEXTURE &&
         bs->texture_combine_alpha_op[0] == COGL_PIPELINE_COMBINE_OP_SRC_ALPHA &&
         bs->texture_combine_alpha_op[1] == COGL_PIPELINE_COMBINE_OP_SRC_ALPHA;
}

/* Cold-path features that require assemble (alpha discard, point size,
 * point-sprite, snippets, multi-layer, non-default combine / matrix).
 * Shared by is_hot_path and get_pipeline_for_cogl so they cannot drift.
 */
static gboolean
cogl_vk_pipeline_needs_assemble (CoglPipeline *pipeline)
{
  g_autofree CoglPipelineLayer **layers = NULL;
  int n_layers = 0;
  int i;

  if (_cogl_pipeline_has_vertex_snippets (pipeline) ||
      _cogl_pipeline_has_non_layer_fragment_snippets (pipeline))
    return TRUE;

  if (cogl_pipeline_get_alpha_test_function (pipeline) !=
      COGL_PIPELINE_ALPHA_FUNC_ALWAYS)
    return TRUE;

  if (cogl_pipeline_get_point_size (pipeline) > 0.f ||
      cogl_pipeline_get_per_vertex_point_size (pipeline))
    return TRUE;

  cogl_vk_pipeline_collect_layers (pipeline, &layers, &n_layers);
  if (n_layers > 1)
    return TRUE;

  for (i = 0; i < n_layers; i++)
    {
      CoglPipelineLayer *layer = layers[i];
      CoglPipelineLayer *auth;
      const graphene_matrix_t *matrix;

      if (!layer)
        continue;

      if (cogl_pipeline_get_layer_point_sprite_coords_enabled (pipeline,
                                                               layer->index))
        return TRUE;

      if (!cogl_vk_layer_combine_is_default (layer))
        return TRUE;

      matrix = _cogl_pipeline_get_layer_matrix (pipeline, layer->index);
      if (matrix && !graphene_matrix_is_identity (matrix))
        return TRUE;

      auth = _cogl_pipeline_layer_get_authority
               (layer, COGL_PIPELINE_LAYER_STATE_FRAGMENT_SNIPPETS);
      if (auth && auth->has_big_state && auth->big_state &&
          auth->big_state->fragment_snippets.entries)
        return TRUE;
    }

  return FALSE;
}

gboolean
cogl_vk_pipeline_is_hot_path (CoglPipeline *pipeline)
{
  g_return_val_if_fail (pipeline != NULL, TRUE);

  return !cogl_vk_pipeline_needs_assemble (pipeline);
}

static void
free_module_entry (gpointer data)
{
  CoglVkSpirvModuleEntry *e = data;
  g_free (e->key);
  g_free (e);
}

static void
free_pipe_entry (gpointer data)
{
  CoglVkSpirvPipeEntry *e = data;
  g_free (e->key);
  g_free (e);
}

CoglVkSpirvCache *
cogl_vk_spirv_cache_new (CoglVkDevice *device)
{
  CoglVkSpirvCache *cache;

  g_return_val_if_fail (device != NULL, NULL);

  cache = g_new0 (CoglVkSpirvCache, 1);
  cache->device = device;
  cache->modules = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          NULL, free_module_entry);
  cache->pipes = g_hash_table_new_full (g_str_hash, g_str_equal,
                                        NULL, free_pipe_entry);
  cache->disk_cache_dir =
    g_build_filename (g_get_user_cache_dir (), "mutter", "cogl-vk-spirv", NULL);
  g_mkdir_with_parents (cache->disk_cache_dir, 0700);
#ifdef HAVE_SHADERC
  cache->compiler = shaderc_compiler_initialize ();
#endif
  return cache;
}

void
cogl_vk_spirv_cache_free (CoglVkSpirvCache *cache)
{
  GHashTableIter iter;
  gpointer value;

  if (!cache)
    return;

  g_hash_table_iter_init (&iter, cache->modules);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      CoglVkSpirvModuleEntry *e = value;
      if (e->module != VK_NULL_HANDLE)
        vkDestroyShaderModule (cache->device->device, e->module, NULL);
    }

  g_hash_table_iter_init (&iter, cache->pipes);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      CoglVkSpirvPipeEntry *e = value;
      if (e->owns_pipeline && e->pipeline != VK_NULL_HANDLE)
        vkDestroyPipeline (cache->device->device, e->pipeline, NULL);
    }

  g_clear_pointer (&cache->modules, g_hash_table_unref);
  g_clear_pointer (&cache->pipes, g_hash_table_unref);
  g_clear_pointer (&cache->disk_cache_dir, g_free);
#ifdef HAVE_SHADERC
  if (cache->compiler)
    shaderc_compiler_release (cache->compiler);
#endif
  g_free (cache);
}

#ifdef HAVE_SHADERC
static char *
spirv_disk_cache_path (CoglVkSpirvCache       *cache,
                       const char             *key,
                       VkShaderStageFlagBits   stage,
                       const char             *glsl_source)
{
  g_autoptr (GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);
  const char *digest;

  g_checksum_update (checksum, (const unsigned char *) key, strlen (key));
  g_checksum_update (checksum, (const unsigned char *) "\0", 1);
  g_checksum_update (checksum, (const unsigned char *) &stage, sizeof (stage));
  g_checksum_update (checksum, (const unsigned char *) "\0", 1);
  g_checksum_update (checksum, (const unsigned char *) glsl_source, strlen (glsl_source));
  digest = g_checksum_get_string (checksum);

  return g_build_filename (cache->disk_cache_dir, digest, NULL);
}

static gboolean
spirv_disk_cache_load (const char  *path,
                       GBytes     **out_bytes)
{
  g_autoptr (GError) local = NULL;
  char *contents = NULL;
  gsize length = 0;

  if (!g_file_get_contents (path, &contents, &length, &local))
    return FALSE;
  if (length < 4 || (length % 4) != 0)
    {
      g_free (contents);
      return FALSE;
    }

  *out_bytes = g_bytes_new_take (contents, length);
  return TRUE;
}

static void
spirv_disk_cache_store (const char   *path,
                        const void   *spirv,
                        gsize         length)
{
  g_autoptr (GError) local = NULL;
  g_file_set_contents (path, spirv, (gssize) length, &local);
}
#endif

VkShaderModule
cogl_vk_spirv_cache_get_module (CoglVkSpirvCache       *cache,
                                const char             *key,
                                VkShaderStageFlagBits   stage,
                                const char             *glsl_source,
                                GError                **error)
{
  CoglVkSpirvModuleEntry *entry;
#ifdef HAVE_SHADERC
  shaderc_compilation_result_t result = NULL;
  shaderc_shader_kind kind;
  shaderc_compile_options_t options;
  VkShaderModuleCreateInfo info = {
    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
  };
  VkResult res;
  g_autofree char *disk_path = NULL;
  g_autoptr (GBytes) disk_bytes = NULL;
  const uint32_t *code = NULL;
  gsize code_size = 0;
  gboolean from_disk = FALSE;
#endif

  g_return_val_if_fail (cache != NULL, VK_NULL_HANDLE);
  g_return_val_if_fail (key != NULL, VK_NULL_HANDLE);

  entry = g_hash_table_lookup (cache->modules, key);
  if (entry)
    return entry->module;

#ifndef HAVE_SHADERC
  (void) stage;
  (void) glsl_source;
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "shaderc not available at build time");
  return VK_NULL_HANDLE;
#else
  disk_path = spirv_disk_cache_path (cache, key, stage, glsl_source);
  if (spirv_disk_cache_load (disk_path, &disk_bytes))
    {
      code = g_bytes_get_data (disk_bytes, &code_size);
      from_disk = TRUE;
    }
  else
    {
      if (!cache->compiler)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "shaderc compiler init failed");
          return VK_NULL_HANDLE;
        }

      kind = (stage == VK_SHADER_STAGE_VERTEX_BIT)
        ? shaderc_glsl_vertex_shader
        : shaderc_glsl_fragment_shader;

      options = shaderc_compile_options_initialize ();
      shaderc_compile_options_set_target_env (options, shaderc_target_env_vulkan,
                                              shaderc_env_version_vulkan_1_1);
      result = shaderc_compile_into_spv (cache->compiler,
                                         glsl_source, strlen (glsl_source),
                                         kind, key, "main", options);
      shaderc_compile_options_release (options);

      if (shaderc_result_get_compilation_status (result) !=
          shaderc_compilation_status_success)
        {
          const char *err = shaderc_result_get_error_message (result);
          FILE *dump;
          const char *stage_name =
            (stage == VK_SHADER_STAGE_VERTEX_BIT) ? "vert" : "frag";

          cogl_vk_debug_log (
            "draw",
            "shaderc FAIL stage=%s key=%s err=%s",
            stage_name, key, err ? err : "?");
          /* Dump full GLSL for diagnosis (unsure which emit is wrong). */
          dump = cogl_vk_debug_enabled ("draw")
                 ? fopen ("/tmp/cogl-vk-shaderc-fail.glsl", "w") : NULL;
          if (dump)
            {
              fprintf (dump, "/* stage=%s key=%s */\n%s\n",
                       stage_name, key, glsl_source);
              fclose (dump);
              cogl_vk_debug_log (
                "draw",
                "shaderc FAIL dumped GLSL → /tmp/cogl-vk-shaderc-fail.glsl "
                "(%zu bytes)",
                strlen (glsl_source));
            }
          /* Also log first ~800 chars to the debug file. */
          {
            gsize n = strlen (glsl_source);
            g_autofree char *head = g_strndup (glsl_source, MIN (n, 800));

            cogl_vk_debug_log ("draw", "shaderc FAIL glsl_head:\n%s", head);
          }

          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "shaderc: %s",
                       err ? err : "unknown");
          shaderc_result_release (result);
          return VK_NULL_HANDLE;
        }

      code_size = shaderc_result_get_length (result);
      code = (const uint32_t *) shaderc_result_get_bytes (result);
      spirv_disk_cache_store (disk_path, code, code_size);
    }

  info.codeSize = code_size;
  info.pCode = code;
  entry = g_new0 (CoglVkSpirvModuleEntry, 1);
  entry->key = g_strdup (key);
  res = vkCreateShaderModule (cache->device->device, &info, NULL,
                              &entry->module);
  if (result)
    shaderc_result_release (result);
  if (res != VK_SUCCESS)
    {
      g_free (entry->key);
      g_free (entry);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkCreateShaderModule failed (%d)%s", (int) res,
                   from_disk ? " (disk cache)" : "");
      return VK_NULL_HANDLE;
    }

  g_hash_table_insert (cache->modules, entry->key, entry);
  return entry->module;
#endif
}

/* Inject std140 UBO at the assemble marker; always declare binding 1 so
 * the shader matches ds_layout_cold even when there are no uniforms.
 */
static char *
cogl_vk_inject_ubo_into_assembled (const char *src,
                                   const char *ubo_glsl,
                                   const char *defines)
{
  return cogl_vk_pipeline_inject_ubo (src, ubo_glsl, defines);
}

/* Strip `uniform type name;` lines so they can live in the std140 UBO. */
static void
cogl_vk_strip_uniform_decls (GString *src)
{
  g_autoptr (GString) kept = NULL;
  const char *p;

  if (!src || !src->str)
    return;

  kept = g_string_new (NULL);
  for (p = src->str; *p; )
    {
      const char *line_end = strchr (p, '\n');
      size_t line_len = line_end ? (size_t) (line_end - p) : strlen (p);
      g_autofree char *line = g_strndup (p, line_len);
      char type[32];
      char name[128];
      const char *s = line;

      while (*s == ' ' || *s == '\t')
        s++;

      if (sscanf (s, "uniform %31s %127[^;];", type, name) == 2 &&
          strcmp (type, "sampler2D") != 0 &&
          strncmp (type, "sampler", 7) != 0)
        {
          /* drop — packed into UBO */
        }
      else if (line_len > 0)
        {
          g_string_append_len (kept, p, line_len);
          g_string_append_c (kept, '\n');
        }

      if (!line_end)
        break;
      p = line_end + 1;
    }

  g_string_assign (src, kept->str);
}

char *
cogl_vk_glsl_finalize_cold_frag (CoglVkPipelineCache  *cache,
                                 CoglPipeline         *pipeline,
                                 const char           *assembled_frag,
                                 GError              **error)
{
  g_autoptr (GString) frag_src = NULL;
  g_autoptr (GString) ubo_glsl = NULL;
  g_autoptr (GString) defines = NULL;

  g_return_val_if_fail (assembled_frag != NULL, NULL);

  frag_src = g_string_new (assembled_frag);
  ubo_glsl = g_string_new (NULL);
  defines = g_string_new (NULL);

  if (!cogl_vk_pipeline_pack_uniforms (cache, pipeline,
                                       NULL, ubo_glsl, defines, error))
    return NULL;

  cogl_vk_strip_uniform_decls (frag_src);
  {
    char *injected = cogl_vk_inject_ubo_into_assembled (frag_src->str,
                                                        ubo_glsl->str,
                                                        defines->str);
    if (g_getenv ("COGL_VK_FORCE_RED") && injected)
      {
        char *at = strstr (injected, "void\nmain");
        if (!at)
          at = strstr (injected, "void main");
        if (at)
          {
            char *brace = strchr (at, '{');
            if (brace)
              {
                GString *forced = g_string_new (NULL);
                g_string_append_len (forced, injected, brace - injected + 1);
                g_string_append (forced,
                                 "\n  out_color = vec4(1.0, 0.0, 0.0, 1.0);\n  return;\n");
                g_string_append (forced, brace + 1);
                g_free (injected);
                injected = g_string_free (forced, FALSE);
              }
          }
      }
    if (g_getenv ("COGL_VK_UBO_TRACE") && injected &&
        !g_file_test ("/tmp/cogl-vk-cold-frag.glsl", G_FILE_TEST_EXISTS))
      g_file_set_contents ("/tmp/cogl-vk-cold-frag.glsl", injected, -1, NULL);
    return injected;
  }
}

char *
cogl_vk_glsl_finalize_cold_vert (CoglVkPipelineCache  *cache,
                                 CoglPipeline         *pipeline,
                                 const char           *assembled_vert,
                                 GError              **error)
{
  g_autoptr (GString) vert_src = NULL;
  g_autoptr (GString) ubo_glsl = NULL;
  g_autoptr (GString) defines = NULL;

  g_return_val_if_fail (assembled_vert != NULL, NULL);

  vert_src = g_string_new (assembled_vert);
  ubo_glsl = g_string_new (NULL);
  defines = g_string_new (NULL);

  if (!cogl_vk_pipeline_pack_uniforms (cache, pipeline,
                                       NULL, ubo_glsl, defines, error))
    return NULL;

  cogl_vk_strip_uniform_decls (vert_src);
  {
    char *injected = cogl_vk_inject_ubo_into_assembled (vert_src->str,
                                                        ubo_glsl->str,
                                                        defines->str);
    if (g_getenv ("COGL_VK_UBO_TRACE") && injected)
      g_file_set_contents ("/tmp/cogl-vk-cold-vert.glsl", injected, -1, NULL);
    return injected;
  }
}

VkPipeline
cogl_vk_spirv_cache_get_pipeline_for_cogl (CoglVkSpirvCache        *cache,
                                           CoglVkPipelineCache     *pipe_cache,
                                           CoglVkRenderFormatSetup *setup,
                                           CoglPipeline            *pipeline,
                                           gboolean                 opaque,
                                           GError                 **error)
{
  g_autofree char *assemble_fp = NULL;
  g_autofree char *pipe_key = NULL;
  g_autofree char *frag_key = NULL;
  g_autofree char *vert_key = NULL;
  g_autofree char *frag_final = NULL;
  g_autofree char *vert_final = NULL;
  g_autoptr (GString) frag_src = NULL;
  g_autoptr (GString) vert_src = NULL;
  g_autoptr (GString) ubo_glsl = NULL;
  g_autoptr (GString) defines = NULL;
  CoglVkAssembleResult assembled = {0};
  CoglVkSpirvPipeEntry *entry;
  VkShaderModule vert, frag;
  VkPipeline pipe = VK_NULL_HANDLE;

  g_return_val_if_fail (cache && pipe_cache && setup && pipeline, VK_NULL_HANDLE);

  if (!cogl_vk_pipeline_needs_assemble (pipeline))
    {
      /* Should be rare: caller normally gates on !is_hot_path. */
      return opaque ? setup->tex_pipe_opaque : setup->tex_pipe;
    }

  assemble_fp = cogl_vk_assemble_pipeline_fingerprint (pipeline);

  {
    CoglVkPipeStateKey state_key;
    CoglDriver *drv = cogl_context_get_driver (pipeline->context);
    VkFrontFace ff = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    gboolean blend_on = !opaque;
    g_autoptr (GString) key_buf = g_string_new (NULL);

    if (COGL_IS_DRIVER_VK (drv))
      ff = COGL_DRIVER_VK (drv)->pending_front_face;

    cogl_vk_pipe_state_key_from_pipeline (
      &state_key, pipeline, ff, blend_on,
      setup->samples > VK_SAMPLE_COUNT_1_BIT
        ? setup->samples : VK_SAMPLE_COUNT_1_BIT,
      setup->ds_format != VK_FORMAT_UNDEFINED,
      FALSE, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
      0 /* cold textured: empty vertex input */);

    g_string_printf (key_buf, "asm|%s|fmt=%u|ds=%u",
                     assemble_fp ? assemble_fp : "fail",
                     (unsigned) setup->format,
                     (unsigned) setup->ds_format);
    cogl_vk_pipe_state_key_append (key_buf, &state_key);
    pipe_key = g_string_free (g_steal_pointer (&key_buf), FALSE);
  }

  entry = g_hash_table_lookup (cache->pipes, pipe_key);
  if (entry)
    return entry->pipeline;

  if (!cogl_vk_assemble_pipeline_glsl (pipeline,
                                       COGL_VK_ASSEMBLE_VERT_PUSH_QUAD,
                                       NULL,
                                       &assembled, error))
    return VK_NULL_HANDLE;

  frag_src = g_string_new (assembled.frag_glsl);
  vert_src = g_string_new (assembled.vert_glsl);
  ubo_glsl = g_string_new (NULL);
  defines = g_string_new (NULL);

  /* Same snippet→UBO layout as draw-time pack (decls_inout == NULL). */
  if (!cogl_vk_pipeline_pack_uniforms (pipe_cache, pipeline,
                                       NULL, ubo_glsl, defines, error))
    {
      cogl_vk_assemble_result_clear (&assembled);
      return VK_NULL_HANDLE;
    }

  cogl_vk_strip_uniform_decls (frag_src);
  cogl_vk_strip_uniform_decls (vert_src);
  frag_final = cogl_vk_inject_ubo_into_assembled (frag_src->str,
                                                  ubo_glsl->str,
                                                  defines->str);
  vert_final = cogl_vk_inject_ubo_into_assembled (vert_src->str,
                                                  ubo_glsl->str,
                                                  defines->str);
  frag_key = g_strdup_printf ("asm-frag|%s", assemble_fp);
  vert_key = g_strdup_printf ("asm-vert|%s", assemble_fp);

  vert = cogl_vk_spirv_cache_get_module (cache, vert_key,
                                         VK_SHADER_STAGE_VERTEX_BIT,
                                         vert_final, error);
  frag = cogl_vk_spirv_cache_get_module (cache, frag_key,
                                         VK_SHADER_STAGE_FRAGMENT_BIT,
                                         frag_final, error);
  cogl_vk_assemble_result_clear (&assembled);
  if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE)
    return VK_NULL_HANDLE;

  if (pipe_cache->pipe_layout_cold == VK_NULL_HANDLE)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "cold pipeline layout unavailable");
      return VK_NULL_HANDLE;
    }

  {
    CoglVkTexPipeExtras extras;
    CoglDriver *drv = cogl_context_get_driver (pipeline->context);
    VkFrontFace ff = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    gboolean blend_on = !opaque;

    if (COGL_IS_DRIVER_VK (drv))
      ff = COGL_DRIVER_VK (drv)->pending_front_face;
    cogl_vk_tex_pipe_extras_from_pipeline (pipeline, ff, &extras);
    if (setup->ds_format != VK_FORMAT_UNDEFINED)
      extras.stencil_equal_keep = TRUE;
    if (setup->samples > VK_SAMPLE_COUNT_1_BIT)
      extras.samples = setup->samples;
    /* Always declare dynamic blend constants on cold PSOs. */
    extras.dynamic_blend_constants = TRUE;

    if (!cogl_vk_create_tex_pipeline (pipe_cache, setup->render_pass,
                                      vert, frag, pipe_cache->pipe_layout_cold,
                                      blend_on, &extras, &pipe, error))
      return VK_NULL_HANDLE;
  }

  entry = g_new0 (CoglVkSpirvPipeEntry, 1);
  entry->key = g_steal_pointer (&pipe_key);
  entry->pipeline = pipe;
  entry->owns_pipeline = TRUE;
  g_hash_table_insert (cache->pipes, entry->key, entry);
  return entry->pipeline;
}
