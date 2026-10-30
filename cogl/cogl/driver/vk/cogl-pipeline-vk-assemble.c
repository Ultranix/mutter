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

#include "cogl/driver/vk/cogl-pipeline-vk-assemble-private.h"
#include "cogl/driver/vk/cogl-pipeline-vk-private.h"
#include "cogl/driver/vk/cogl-pipeline-vk-vbo-private.h"
#include "cogl/driver/vk/cogl-vk-debug-private.h"

#include "cogl/cogl-pipeline-layer-private.h"
#include "cogl/cogl-pipeline-layer-state.h"
#include "cogl/cogl-sampler-cache-private.h"
#include "cogl/cogl-pipeline-private.h"
#include "cogl/cogl-pipeline-snippet-private.h"
#include "cogl/cogl-pipeline-state.h"
#include "cogl/cogl-pipeline-state-private.h"
#include "cogl/cogl-snippet.h"

#include <gio/gio.h>
#include <string.h>

void
cogl_vk_assemble_result_clear (CoglVkAssembleResult *result)
{
  if (!result)
    return;
  g_clear_pointer (&result->vert_glsl, g_free);
  g_clear_pointer (&result->frag_glsl, g_free);
  result->n_layers = 0;
  result->has_vertex_hooks = FALSE;
  result->needs_flip_vector = FALSE;
  result->n_texcoord_varyings = 0;
  result->needs_texture_matrix_ubo = FALSE;
  result->texcoord0_attr_location = 0;
  result->point_size_attr_location = -1;
  result->normal_attr_location = -1;
  result->custom_attr_base_location = -1;
}

/* Snippet `varying` is GLSL 1.20. SPIR-V needs `in`/`out` plus a location
 * that matches between stages. Reserved locations: texcoord array 0..n-1,
 * v_color at n. Snippet varyings start at first_loc.
 */
static gboolean
line_skips_varying_scan (const char *line)
{
  while (*line == ' ' || *line == '\t')
    line++;
  if (*line == '#' || *line == '/' || *line == '*' || *line == '\0')
    return TRUE;
  return FALSE;
}

static const char *
varying_token_at (const char *line)
{
  const char *p = line;

  while (*p)
    {
      if (g_str_has_prefix (p, "varying") &&
          (p == line || g_ascii_isspace (p[-1])) &&
          (p[7] == '\0' || g_ascii_isspace (p[7])))
        return p;
      p++;
    }
  return NULL;
}

static gboolean
parse_varying_decl (const char *line,
                    char      **name,
                    int        *slots)
{
  const char *tok;
  const char *p;
  const char *start;
  int n = 0;

  if (line_skips_varying_scan (line))
    return FALSE;
  if (strstr (line, "layout"))
    return FALSE;
  tok = varying_token_at (line);
  if (!tok)
    return FALSE;

  p = tok + 7;
  while (g_ascii_isspace (*p))
    p++;
  if (g_str_has_prefix (p, "lowp ") ||
      g_str_has_prefix (p, "mediump ") ||
      g_str_has_prefix (p, "highp "))
    {
      while (*p && !g_ascii_isspace (*p))
        p++;
      while (g_ascii_isspace (*p))
        p++;
    }
  if (!g_ascii_isalpha (*p) && *p != '_')
    return FALSE;
  while (g_ascii_isalnum (*p) || *p == '_')
    p++;
  while (g_ascii_isspace (*p))
    p++;
  start = p;
  if (!g_ascii_isalpha (*p) && *p != '_')
    return FALSE;
  while (g_ascii_isalnum (*p) || *p == '_')
    p++;

  *name = g_strndup (start, (gsize) (p - start));
  *slots = 1;
  while (g_ascii_isspace (*p))
    p++;
  if (*p == '[')
    {
      p++;
      while (g_ascii_isdigit (*p))
        {
          n = n * 10 + (*p - '0');
          p++;
        }
      if (n > 1)
        *slots = n;
    }
  return TRUE;
}

static void
collect_varying_locs (const char *src,
                      GHashTable *locs,
                      int        *next)
{
  g_auto (GStrv) lines = g_strsplit (src ? src : "", "\n", -1);
  char **iter;

  for (iter = lines; iter && *iter; iter++)
    {
      g_autofree char *name = NULL;
      int slots = 1;
      int *existing;

      if (!parse_varying_decl (*iter, &name, &slots))
        continue;
      existing = g_hash_table_lookup (locs, name);
      if (!existing)
        {
          int *slot = g_new (int, 1);

          *slot = *next;
          g_hash_table_insert (locs, g_steal_pointer (&name), slot);
          *next += slots;
        }
      else if (*existing + slots > *next)
        {
          *next = *existing + slots;
        }
    }
}

static void
rewrite_varying_lines (GString    *src,
                       GHashTable *locs,
                       const char *storage)
{
  g_auto (GStrv) lines = NULL;
  GString *out;
  char **iter;

  if (!src)
    return;

  lines = g_strsplit (src->str, "\n", -1);
  out = g_string_new (NULL);
  for (iter = lines; iter && *iter; iter++)
    {
      g_autofree char *name = NULL;
      int slots = 1;
      const char *tok;
      int *loc;

      if (!parse_varying_decl (*iter, &name, &slots) ||
          !(loc = g_hash_table_lookup (locs, name)) ||
          !(tok = varying_token_at (*iter)))
        {
          g_string_append (out, *iter);
          g_string_append_c (out, '\n');
          continue;
        }

      g_string_append_len (out, *iter, (gssize) (tok - *iter));
      g_string_append_printf (out, "layout(location = %d) %s", *loc, storage);
      g_string_append (out, tok + strlen ("varying"));
      g_string_append_c (out, '\n');
    }
  g_string_assign (src, out->str);
  g_string_free (out, TRUE);
}

static void
promote_legacy_varyings (GString *vert,
                         GString *frag,
                         int      first_loc)
{
  GHashTable *locs = g_hash_table_new_full (g_str_hash, g_str_equal,
                                            g_free, g_free);
  int next = first_loc;

  collect_varying_locs (vert ? vert->str : NULL, locs, &next);
  collect_varying_locs (frag ? frag->str : NULL, locs, &next);
  rewrite_varying_lines (vert, locs, "out");
  rewrite_varying_lines (frag, locs, "in");
  g_hash_table_unref (locs);
}

/* generate_code needs a non-NULL list; empty entries → stub chain (GL). */
static CoglPipelineSnippetList *
snippets_or_empty (CoglPipelineSnippetList *list)
{
  static CoglPipelineSnippetList empty = { .entries = NULL };
  return list ? list : &empty;
}

static const char *
glsl_vec_type (int n_components)
{
  switch (n_components)
    {
    case 1:
      return "float";
    case 2:
      return "vec2";
    case 3:
      return "vec3";
    default:
      return "vec4";
    }
}

static int
vbo_texcoord_attr_location (const CoglVkAssembleVboAttrs *attrs,
                            int                           layer)
{
  if (attrs)
    for (int i = 0; i < attrs->n_texcoords; i++)
      if (attrs->texcoord_layers[i] == layer)
        return i == 0 ? COGL_VK_VBO_LOC_TEXCOORD0 :
               COGL_VK_VBO_LOC_TEXCOORD_BASE + i - 1;
  return layer == 0 ? COGL_VK_VBO_LOC_TEXCOORD0 : -1;
}

static CoglPipelineSnippetList *
get_pipeline_vertex_snippets (CoglPipeline *pipeline)
{
  CoglPipeline *authority =
    _cogl_pipeline_get_authority (pipeline, COGL_PIPELINE_STATE_VERTEX_SNIPPETS);
  if (!authority || !authority->has_big_state || !authority->big_state)
    return NULL;
  return &authority->big_state->vertex_snippets;
}

static CoglPipelineSnippetList *
get_pipeline_fragment_snippets (CoglPipeline *pipeline)
{
  CoglPipeline *authority =
    _cogl_pipeline_get_authority (pipeline,
                                  COGL_PIPELINE_STATE_FRAGMENT_SNIPPETS);
  if (!authority || !authority->has_big_state || !authority->big_state)
    return NULL;
  return &authority->big_state->fragment_snippets;
}

static CoglPipelineSnippetList *
get_layer_fragment_snippets (CoglPipelineLayer *layer)
{
  CoglPipelineLayer *auth =
    _cogl_pipeline_layer_get_authority (layer,
                                        COGL_PIPELINE_LAYER_STATE_FRAGMENT_SNIPPETS);
  if (!auth || !auth->has_big_state || !auth->big_state)
    return NULL;
  return &auth->big_state->fragment_snippets;
}

static CoglPipelineSnippetList *
get_layer_vertex_snippets (CoglPipelineLayer *layer)
{
  CoglPipelineLayer *auth =
    _cogl_pipeline_layer_get_authority (layer,
                                        COGL_PIPELINE_LAYER_STATE_VERTEX_SNIPPETS);
  if (!auth || !auth->has_big_state || !auth->big_state)
    return NULL;
  return &auth->big_state->vertex_snippets;
}

static void
append_arg (GString                  *src,
            CoglPipeline             *pipeline,
            CoglPipelineLayer        *layer,
            int                       previous_layer_index,
            CoglPipelineCombineSource combine_src,
            CoglPipelineCombineOp     operand,
            const char               *swizzle)
{
  char alpha_swizzle[5] = "aaaa";

  g_string_append_c (src, '(');
  if (operand == COGL_PIPELINE_COMBINE_OP_ONE_MINUS_SRC_COLOR ||
      operand == COGL_PIPELINE_COMBINE_OP_ONE_MINUS_SRC_ALPHA)
    g_string_append_printf (src, "vec4(1.0).%s - ", swizzle);

  if (operand == COGL_PIPELINE_COMBINE_OP_SRC_ALPHA ||
      operand == COGL_PIPELINE_COMBINE_OP_ONE_MINUS_SRC_ALPHA)
    {
      alpha_swizzle[strlen (swizzle)] = '\0';
      swizzle = alpha_swizzle;
    }

  switch (combine_src)
    {
    case COGL_PIPELINE_COMBINE_SOURCE_TEXTURE:
      g_string_append_printf (src, "cogl_texel%i.%s", layer->index, swizzle);
      break;
    case COGL_PIPELINE_COMBINE_SOURCE_CONSTANT:
      g_string_append_printf (src, "cogl_layer_const%i.%s",
                              layer->index, swizzle);
      break;
    case COGL_PIPELINE_COMBINE_SOURCE_PREVIOUS:
      if (previous_layer_index >= 0)
        {
          g_string_append_printf (src, "cogl_layer%i.%s",
                                  previous_layer_index, swizzle);
          break;
        }
      G_GNUC_FALLTHROUGH;
    case COGL_PIPELINE_COMBINE_SOURCE_PRIMARY_COLOR:
      g_string_append_printf (src, "cogl_color_in.%s", swizzle);
      break;
    default:
      {
        int layer_num = combine_src - COGL_PIPELINE_COMBINE_SOURCE_TEXTURE0;
        CoglPipelineLayer *other =
          _cogl_pipeline_get_layer_with_flags (pipeline, layer_num,
                                               COGL_PIPELINE_GET_LAYER_NO_CREATE);
        if (other)
          g_string_append_printf (src, "cogl_texel%i.%s", other->index, swizzle);
        else
          g_string_append_printf (src, "vec4(1.0).%s", swizzle);
      }
      break;
    }
  g_string_append_c (src, ')');
}

static void
append_combine (GString                         *src,
                CoglPipeline                    *pipeline,
                CoglPipelineLayer               *layer,
                int                              previous_layer_index,
                CoglPipelineCombineFunc          func,
                const CoglPipelineCombineSource *csrc,
                const CoglPipelineCombineOp     *op,
                const char                      *swizzle)
{
  switch (func)
    {
    case COGL_PIPELINE_COMBINE_FUNC_REPLACE:
      append_arg (src, pipeline, layer, previous_layer_index,
                  csrc[0], op[0], swizzle);
      break;
    case COGL_PIPELINE_COMBINE_FUNC_MODULATE:
      append_arg (src, pipeline, layer, previous_layer_index,
                  csrc[0], op[0], swizzle);
      g_string_append (src, " * ");
      append_arg (src, pipeline, layer, previous_layer_index,
                  csrc[1], op[1], swizzle);
      break;
    case COGL_PIPELINE_COMBINE_FUNC_ADD:
      append_arg (src, pipeline, layer, previous_layer_index,
                  csrc[0], op[0], swizzle);
      g_string_append (src, " + ");
      append_arg (src, pipeline, layer, previous_layer_index,
                  csrc[1], op[1], swizzle);
      break;
    case COGL_PIPELINE_COMBINE_FUNC_ADD_SIGNED:
      append_arg (src, pipeline, layer, previous_layer_index,
                  csrc[0], op[0], swizzle);
      g_string_append (src, " + ");
      append_arg (src, pipeline, layer, previous_layer_index,
                  csrc[1], op[1], swizzle);
      g_string_append_printf (src, " - vec4(0.5).%s", swizzle);
      break;
    case COGL_PIPELINE_COMBINE_FUNC_SUBTRACT:
      append_arg (src, pipeline, layer, previous_layer_index,
                  csrc[0], op[0], swizzle);
      g_string_append (src, " - ");
      append_arg (src, pipeline, layer, previous_layer_index,
                  csrc[1], op[1], swizzle);
      break;
    case COGL_PIPELINE_COMBINE_FUNC_INTERPOLATE:
      append_arg (src, pipeline, layer, previous_layer_index,
                  csrc[0], op[0], swizzle);
      g_string_append (src, " * ");
      append_arg (src, pipeline, layer, previous_layer_index,
                  csrc[2], op[2], swizzle);
      g_string_append (src, " + ");
      append_arg (src, pipeline, layer, previous_layer_index,
                  csrc[1], op[1], swizzle);
      g_string_append_printf (src, " * (vec4(1.0).%s - ", swizzle);
      append_arg (src, pipeline, layer, previous_layer_index,
                  csrc[2], op[2], swizzle);
      g_string_append_c (src, ')');
      break;
    case COGL_PIPELINE_COMBINE_FUNC_DOT3_RGB:
    case COGL_PIPELINE_COMBINE_FUNC_DOT3_RGBA:
      g_string_append (src, "vec4(4.0 * ((");
      append_arg (src, pipeline, layer, previous_layer_index, csrc[0], op[0], "r");
      g_string_append (src, " - 0.5) * (");
      append_arg (src, pipeline, layer, previous_layer_index, csrc[1], op[1], "r");
      g_string_append (src, " - 0.5) + (");
      append_arg (src, pipeline, layer, previous_layer_index, csrc[0], op[0], "g");
      g_string_append (src, " - 0.5) * (");
      append_arg (src, pipeline, layer, previous_layer_index, csrc[1], op[1], "g");
      g_string_append (src, " - 0.5) + (");
      append_arg (src, pipeline, layer, previous_layer_index, csrc[0], op[0], "b");
      g_string_append (src, " - 0.5) * (");
      append_arg (src, pipeline, layer, previous_layer_index, csrc[1], op[1], "b");
      g_string_append_printf (src, " - 0.5))).%s", swizzle);
      break;
    default:
      g_string_append_printf (src, "vec4(1.0).%s", swizzle);
      break;
    }
}

static gboolean
layer_has_replace_hook (CoglPipelineLayer *layer,
                        CoglSnippetHook    hook)
{
  CoglPipelineSnippetList *list;
  GList *l;

  list = get_layer_fragment_snippets (layer);
  if (!list)
    return FALSE;
  for (l = list->entries; l; l = l->next)
    {
      CoglSnippet *s = l->data;
      if (cogl_snippet_get_hook (s) == hook &&
          cogl_snippet_get_replace (s) != NULL)
        return TRUE;
    }
  return FALSE;
}

static void
append_layer_snippet_hooks (GString                 *header,
                            CoglPipelineSnippetList *snippets,
                            CoglSnippetHook          hook,
                            const char              *chain_prefix,
                            const char              *final_prefix,
                            const char              *hook_prefix,
                            int                      layer_index,
                            const char              *ret_var,
                            gboolean                 ret_is_arg,
                            const char              *args,
                            const char              *arg_decls)
{
  CoglPipelineSnippetData data = {0};
  g_autofree char *chain = NULL;
  g_autofree char *final_name = NULL;
  g_autofree char *prefix = NULL;

  if (!snippets || !snippets->entries)
    {
      /* No snippets: still need a final symbol that chains to real_*. */
      chain = g_strdup_printf ("%s%i", chain_prefix, layer_index);
      final_name = g_strdup_printf ("%s%i", final_prefix, layer_index);
      if (ret_var)
        {
          if (arg_decls && arg_decls[0])
            {
              g_string_append_printf (header,
                                      "vec4 %s(%s) {\n"
                                      "  return %s(%s);\n"
                                      "}\n",
                                      final_name, arg_decls, chain, args);
            }
          else
            {
              g_string_append_printf (header,
                                      "vec4 %s(void) {\n"
                                      "  return %s();\n"
                                      "}\n",
                                      final_name, chain);
            }
        }
      else
        {
          g_string_append_printf (header,
                                  "void %s(void) {\n"
                                  "  %s();\n"
                                  "}\n",
                                  final_name, chain);
        }
      return;
    }

  chain = g_strdup_printf ("%s%i", chain_prefix, layer_index);
  final_name = g_strdup_printf ("%s%i", final_prefix, layer_index);
  prefix = g_strdup_printf ("%s%i", hook_prefix, layer_index);

  data.snippets = snippets;
  data.hook = hook;
  data.chain_function = chain;
  data.final_name = final_name;
  data.function_prefix = prefix;
  data.return_type = ret_var ? "vec4" : NULL;
  data.return_variable = ret_var;
  data.return_variable_is_argument = ret_is_arg;
  data.arguments = args;
  data.argument_declarations = arg_decls;
  data.source_buf = header;
  _cogl_pipeline_snippet_generate_code (&data);
}

gboolean
cogl_vk_assemble_pipeline_glsl (CoglPipeline                 *pipeline,
                                CoglVkAssembleVertMode        vert_mode,
                                const CoglVkAssembleVboAttrs *vbo_attrs,
                                CoglVkAssembleResult         *out,
                                GError                      **error)
{
  COGL_VK_PROFILE_SCOPE ("cogl_vk_assemble_pipeline_glsl");
  g_autoptr (GString) frag = NULL;
  g_autoptr (GString) vert = NULL;
  g_autoptr (GString) header = NULL;
  g_autoptr (GString) frag_source = NULL;
  g_autoptr (GString) vert_header = NULL;
  g_autoptr (GString) vert_source = NULL;
  g_autofree CoglPipelineLayer **layers = NULL;
  CoglPipelineSnippetList *vert_snippets;
  CoglPipelineSnippetList *frag_snippets;
  int n_layers;
  int i;
  int previous = -1;
  gboolean any_point_sprite = FALSE;
  gboolean has_vert = FALSE;
  gboolean per_vertex_ps;
  CoglPipelineAlphaFunc alpha_func;
  float alpha_ref;
  float point_size;
  int color_loc;
  const char *interpolation = vbo_attrs && vbo_attrs->flat_varyings ? "flat " : "";

  g_return_val_if_fail (pipeline && out, FALSE);
  memset (out, 0, sizeof (*out));

  cogl_vk_pipeline_collect_layers (pipeline, &layers, &n_layers);
  {
    GString *idx_buf = g_string_new (NULL);
    int j;

    for (j = 0; j < n_layers; j++)
      g_string_append_printf (idx_buf, "%s%d",
                              j ? "," : "",
                              layers[j] ? layers[j]->index : -1);
    cogl_vk_debug_log (
      "draw",
      "assemble collect n=%d indices=[%s] vert_mode=%d",
      n_layers, idx_buf->str, (int) vert_mode);
    g_string_free (idx_buf, TRUE);
  }
  if (n_layers > cogl_vk_pipeline_get_max_layers (pipeline))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Vulkan device supports at most %d layers",
                   cogl_vk_pipeline_get_max_layers (pipeline));
      return FALSE;
    }

  for (i = 0; i < n_layers; i++)
    {
      if (layers[i] &&
          cogl_pipeline_get_layer_point_sprite_coords_enabled (pipeline,
                                                               layers[i]->index))
        any_point_sprite = TRUE;
    }

  /* No layers: keep n_layers=0 and use the solid/no-texel frag path.
   * Inventing a NULL layers[0] previously emitted cogl_texture_lookup0 /
   * cogl_generate_layer0 calls without definitions → shaderc fail.
   */
  if (n_layers < 1)
    {
      cogl_vk_debug_log ("draw",
                         "assemble n_layers=0 — solid/no-layer frag path");
    }

  alpha_func = cogl_pipeline_get_alpha_test_function (pipeline);
  alpha_ref = cogl_pipeline_get_alpha_test_reference (pipeline);
  point_size = cogl_pipeline_get_point_size (pipeline);
  if (point_size <= 0.f)
    point_size = 1.f;
  per_vertex_ps = cogl_pipeline_get_per_vertex_point_size (pipeline);

  vert_snippets = get_pipeline_vertex_snippets (pipeline);
  frag_snippets = get_pipeline_fragment_snippets (pipeline);
  has_vert = _cogl_pipeline_has_vertex_snippets (pipeline);

  out->n_layers = n_layers;
  out->n_texcoord_varyings = MAX (n_layers, 1);
  out->needs_texture_matrix_ubo = (n_layers > 0);
  out->has_vertex_hooks = has_vert;
  out->needs_flip_vector = has_vert;
  out->texcoord0_attr_location = COGL_VK_VBO_LOC_TEXCOORD0;
  out->point_size_attr_location =
    (vert_mode == COGL_VK_ASSEMBLE_VERT_VBO) ? COGL_VK_VBO_LOC_POINT_SIZE : -1;
  out->normal_attr_location =
    (vert_mode == COGL_VK_ASSEMBLE_VERT_VBO) ? COGL_VK_VBO_LOC_NORMAL : -1;
  out->custom_attr_base_location =
    vbo_attrs ? vbo_attrs->custom_base : -1;
  /* Varying locations: tex_coord[0..n-1] then v_color. n=0 → color at 0. */
  color_loc = MAX (n_layers, 0);

  frag = g_string_new (NULL);
  vert = g_string_new (NULL);
  header = g_string_new (NULL);
  frag_source = g_string_new (NULL);
  vert_header = g_string_new (NULL);
  vert_source = g_string_new (NULL);

  /* --- Fragment boilerplate (GL fragend + per-layer texcoord_in) --- */
  g_string_append (frag,
                   "#version 450\n"
                   "#define texture2D texture\n"
                   "#define texture2DProj textureProj\n"
                   "#define texture2DLod textureLod\n"
                   "#define varying in\n"
                   "#define cogl_color_out out_color\n"
                   "#define cogl_color_in _cogl_color_in\n"
                   "#define cogl_point_coord gl_PointCoord\n");
  if (n_layers > 0)
    {
      g_string_append_printf (frag,
                              "layout(location = 0) %sin vec4 _cogl_tex_coord[%d];\n"
                              "layout(location = %d) %sin vec4 v_color;\n",
                              interpolation, n_layers, color_loc, interpolation);
    }
  else
    {
      g_string_append_printf (frag, "layout(location = 0) %sin vec4 v_color;\n",
                              interpolation);
    }
  g_string_append (frag,
                   "layout(location = 0) out vec4 out_color;\n"
                   "layout(push_constant) uniform FragPC {\n"
                   "  layout(offset = 80) vec4 color_mul;\n"
                   "} data;\n"
                   "vec4 _cogl_color_in;\n");

  for (i = 0; i < n_layers; i++)
    {
      CoglPipelineLayer *layer = layers[i];
      int idx = layer ? layer->index : i;
      int unit = layer ? _cogl_pipeline_layer_get_unit_index (layer) : i;

      /* Point-sprite layers sample gl_PointCoord; others use per-layer
       * varying.
       */
      if (any_point_sprite &&
          cogl_pipeline_get_layer_point_sprite_coords_enabled (pipeline, idx))
        {
          g_string_append_printf (frag,
                                  "#define cogl_tex_coord%i_in "
                                  "vec4(gl_PointCoord, 0.0, 1.0)\n",
                                  idx);
        }
      else
        {
          g_string_append_printf (frag,
                                  "#define cogl_tex_coord%i_in "
                                  "_cogl_tex_coord[%i]\n",
                                  idx, unit);
        }
    }

  g_string_append_printf (frag,
                          "layout(set = 0, binding = 0) "
                          "uniform sampler2D cogl_sampler[%d];\n"
                          "/* COGL_VK_UBO_SLOT */\n",
                          MAX (n_layers, 1));
  /* Public sampler names use logical layer indices; descriptor slots
   * use compact texture units even when an index fits inside the array
   * (e.g. layers 0 and 10 occupy slots 0 and 1).
   */
  for (i = 0; i < n_layers; i++)
    {
      int idx = layers[i] ? layers[i]->index : i;
      int unit = layers[i] ? _cogl_pipeline_layer_get_unit_index (layers[i]) : i;

      g_string_append_printf (frag,
                              "#define cogl_sampler%i cogl_sampler[%i]\n",
                              idx, unit);
    }

  /* GLOBALS: declarations-only via generate_declarations (GL fragend;
   * pre/post/replace on GLOBALS hooks are ignored — not flat concat of
   * those strings).
   */
  if (frag_snippets)
    {
      _cogl_pipeline_snippet_generate_declarations (header,
                                                    COGL_SNIPPET_HOOK_FRAGMENT_GLOBALS,
                                                    frag_snippets);
    }

  /* Default texture lookup + TEXTURE_LOOKUP wrap per layer */
  for (i = 0; i < n_layers; i++)
    {
      CoglPipelineLayer *layer = layers[i];
      int idx = layer ? layer->index : i;
      CoglPipelineSnippetList *layer_snips =
        layer ? get_layer_fragment_snippets (layer) : NULL;

      /* Always emit real lookup — REPLACE hooks may still chain to it, and
       * skipping left cogl_texture_lookupN calling a missing real_*.
       */
      if (cogl_pipeline_get_layer_point_sprite_coords_enabled (pipeline, idx))
        {
          g_string_append_printf (header,
                                  "vec4 cogl_real_texture_lookup%i(sampler2D tex, "
                                  "vec4 coords) {\n"
                                  "  return textureLod(tex, gl_PointCoord, 0.0);\n"
                                  "}\n",
                                  idx);
        }
      else
        {
          CoglSamplerCacheWrapMode wrap_s =
            COGL_SAMPLER_CACHE_WRAP_MODE_CLAMP_TO_EDGE;
          CoglSamplerCacheWrapMode wrap_t =
            COGL_SAMPLER_CACHE_WRAP_MODE_CLAMP_TO_EDGE;
          const char *wrap_s_expr = "clamp(coords.s, 0.0, 0.999999)";
          const char *wrap_t_expr = "clamp(coords.t, 0.0, 0.999999)";

          if (layer)
            _cogl_pipeline_layer_get_wrap_modes (layer, &wrap_s, &wrap_t);
          if ((int) wrap_s == COGL_PIPELINE_WRAP_MODE_REPEAT)
            wrap_s_expr = "coords.s - floor(coords.s)";
          else if ((int) wrap_s == COGL_PIPELINE_WRAP_MODE_MIRRORED_REPEAT)
            wrap_s_expr = "((mod(floor(coords.s), 2.0) >= 1.0) "
                          "? 1.0 - fract(coords.s) : fract(coords.s))";
          if ((int) wrap_t == COGL_PIPELINE_WRAP_MODE_REPEAT)
            wrap_t_expr = "coords.t - floor(coords.t)";
          else if ((int) wrap_t == COGL_PIPELINE_WRAP_MODE_MIRRORED_REPEAT)
            wrap_t_expr = "((mod(floor(coords.t), 2.0) >= 1.0) "
                          "? 1.0 - fract(coords.t) : fract(coords.t))";

          g_string_append_printf (header,
                                  "vec4 cogl_real_texture_lookup%i(sampler2D tex, "
                                  "vec4 coords) {\n"
                                  "  vec2 _w = vec2(%s, %s);\n"
                                  "  return textureLod(tex, _w, 0.0);\n"
                                  "}\n",
                                  idx, wrap_s_expr, wrap_t_expr);
          cogl_vk_debug_log (
            "tex",
            "assemble lookup%i wrap_s=%d wrap_t=%d s_expr=%s t_expr=%s",
            idx, (int) wrap_s, (int) wrap_t, wrap_s_expr, wrap_t_expr);
        }

      append_layer_snippet_hooks (header, layer_snips,
                                  COGL_SNIPPET_HOOK_TEXTURE_LOOKUP,
                                  "cogl_real_texture_lookup",
                                  "cogl_texture_lookup",
                                  "cogl_texture_lookup_hook",
                                  idx,
                                  "cogl_texel",
                                  FALSE,
                                  "cogl_sampler, cogl_tex_coord",
                                  "sampler2D cogl_sampler, vec4 cogl_tex_coord");
    }

  /* GL fragend: globals before generate_layer (combine refs cogl_texelN). */
  for (i = 0; i < n_layers; i++)
    {
      CoglPipelineLayer *layer = layers[i];
      int idx = layer ? layer->index : i;

      g_string_append_printf (header, "vec4 cogl_texel%i;\n", idx);
    }

  /* Layer combine generation */
  previous = -1;
  for (i = 0; i < n_layers; i++)
    {
      CoglPipelineLayer *layer = layers[i];
      CoglPipelineLayer *combine_auth;
      CoglPipelineLayerBigState *bs;
      CoglPipelineSnippetList *layer_snips;
      int idx;
      float constv[4] = { 0, 0, 0, 0 };

      if (!layer)
        continue;

      idx = layer->index;
      layer_snips = get_layer_fragment_snippets (layer);
      combine_auth = _cogl_pipeline_layer_get_authority
                       (layer, COGL_PIPELINE_LAYER_STATE_COMBINE);
      bs = combine_auth->big_state;

      _cogl_pipeline_get_layer_combine_constant (pipeline, idx, constv);
      g_string_append_printf (header,
                              "const vec4 cogl_layer_const%i = "
                              "vec4(%f, %f, %f, %f);\n",
                              idx, constv[0], constv[1], constv[2], constv[3]);

      g_string_append_printf (header, "vec4 cogl_layer%i;\n", idx);

      if (!layer_has_replace_hook (layer, COGL_SNIPPET_HOOK_LAYER_FRAGMENT))
        {
          g_string_append_printf (header,
                                  "vec4 cogl_real_generate_layer%i(void) {\n"
                                  "  vec4 cogl_layer;\n",
                                  idx);

          /* Match GL fragend: one rgba combine when not separate, or when
           * DOT3_RGBA overrides the alpha function.
           */
          if (!_cogl_pipeline_layer_needs_combine_separate (combine_auth) ||
              bs->texture_combine_rgb_func ==
              COGL_PIPELINE_COMBINE_FUNC_DOT3_RGBA)
            {
              g_string_append (header, "  cogl_layer.rgba = ");
              append_combine (header, pipeline, layer, previous,
                              bs->texture_combine_rgb_func,
                              bs->texture_combine_rgb_src,
                              bs->texture_combine_rgb_op,
                              "rgba");
              g_string_append (header, ";\n");
            }
          else
            {
              g_string_append (header, "  cogl_layer.rgb = ");
              append_combine (header, pipeline, layer, previous,
                              bs->texture_combine_rgb_func,
                              bs->texture_combine_rgb_src,
                              bs->texture_combine_rgb_op,
                              "rgb");
              g_string_append (header, ";\n  cogl_layer.a = ");
              append_combine (header, pipeline, layer, previous,
                              bs->texture_combine_alpha_func,
                              bs->texture_combine_alpha_src,
                              bs->texture_combine_alpha_op,
                              "a");
              g_string_append (header, ";\n");
            }

          g_string_append (header, "  return cogl_layer;\n}\n");
        }

      append_layer_snippet_hooks (header, layer_snips,
                                  COGL_SNIPPET_HOOK_LAYER_FRAGMENT,
                                  "cogl_real_generate_layer",
                                  "cogl_generate_layer",
                                  "cogl_generate_layer_hook",
                                  idx,
                                  "cogl_layer",
                                  FALSE,
                                  "",
                                  "");

      previous = idx;
    }

  g_string_append (frag, header->str);

  /* Generated fragment body — wrapped by FRAGMENT hooks like GL fragend. */
  g_string_append (frag_source,
                   "void\n"
                   "cogl_generated_source ()\n"
                   "{\n"
                   "  _cogl_color_in = v_color;\n");

  for (i = 0; i < n_layers; i++)
    {
      CoglPipelineLayer *layer = layers[i];
      int idx = layer ? layer->index : i;
      int unit = i;

      g_string_append_printf (frag_source,
                              "  cogl_texel%i = cogl_texture_lookup%i("
                              "cogl_sampler[%i], cogl_tex_coord%i_in);\n",
                              idx, idx, unit, idx);
    }
  previous = -1;
  for (i = 0; i < n_layers; i++)
    {
      CoglPipelineLayer *layer = layers[i];
      int idx = layer ? layer->index : i;

      g_string_append_printf (frag_source,
                              "  cogl_layer%i = cogl_generate_layer%i();\n",
                              idx, idx);
      previous = idx;
    }
  if (previous >= 0)
    {
      g_string_append_printf (frag_source,
                              "  cogl_color_out = cogl_layer%i;\n"
                              "  cogl_color_out *= data.color_mul;\n",
                              previous);
    }
  else
    {
      /* No layers (or all NULL): pipeline color via v_color, not a fake
       * texel0.
       */
      cogl_vk_debug_log ("draw",
                         "assemble frag solid path (no layer combine)");
      g_string_append (frag_source,
                       "  cogl_color_out = cogl_color_in;\n"
                       "  cogl_color_out *= data.color_mul;\n");
    }

  /* ALPHA_FUNC → discard (GL fragend parity). */
  if (alpha_func == COGL_PIPELINE_ALPHA_FUNC_NEVER)
    g_string_append (frag_source, "  discard;\n");
  else if (alpha_func != COGL_PIPELINE_ALPHA_FUNC_ALWAYS)
    {
      const char *op = ">=";
      switch (alpha_func)
        {
        case COGL_PIPELINE_ALPHA_FUNC_LESS: op = ">="; break;
        case COGL_PIPELINE_ALPHA_FUNC_EQUAL: op = "!="; break;
        case COGL_PIPELINE_ALPHA_FUNC_LEQUAL: op = ">"; break;
        case COGL_PIPELINE_ALPHA_FUNC_GREATER: op = "<="; break;
        case COGL_PIPELINE_ALPHA_FUNC_NOTEQUAL: op = "=="; break;
        case COGL_PIPELINE_ALPHA_FUNC_GEQUAL: op = "<"; break;
        default: break;
        }
      g_string_append_printf (frag_source,
                              "  if (cogl_color_out.a %s %f) discard;\n",
                              op, alpha_ref);
    }

  g_string_append (frag_source, "}\n");

  /* Always wrap via generate_code (GL fragend): stub main when no FRAGMENT
   * hooks.
   */
  {
    CoglPipelineSnippetData snippet_data = {0};

    snippet_data.snippets = snippets_or_empty (frag_snippets);
    snippet_data.hook = COGL_SNIPPET_HOOK_FRAGMENT;
    snippet_data.chain_function = "cogl_generated_source";
    snippet_data.final_name = "main";
    snippet_data.function_prefix = "cogl_fragment_hook";
    snippet_data.source_buf = frag_source;
    _cogl_pipeline_snippet_generate_code (&snippet_data);
  }

  g_string_append (frag, frag_source->str);

  /* --- Vertex (GL vertend: per-layer texcoord + matrix + hook wrap) --- */
  g_string_append (vert,
                   "#version 450\n"
                   "/* Legacy GLSL keywords → Vulkan; do not `#define attribute //`\n"
                   " * (that comments out whole decls and breaks custom attrs). */\n"
                   "#define texture2D texture\n"
                   "#define attribute in\n"
                   "#define varying out\n");

  if (vert_mode == COGL_VK_ASSEMBLE_VERT_VBO)
    {
      int nc;
      int normal_comps = 3;

      g_string_append (vert,
                       "layout(push_constant, row_major) uniform VertPC {\n"
                       "  mat4 mvp;\n"
                       "  vec2 point_size_pad;\n"
                       "  vec2 uv_size;\n"
                       "} data;\n");
      if (vbo_attrs && vbo_attrs->position_as_int)
        {
          int pc = vbo_attrs->position_components > 0
            ? vbo_attrs->position_components : 2;
          gboolean u = vbo_attrs->position_unsigned;
          const char *ity;

          if (u)
            ity = (pc >= 4) ? "uvec4" : (pc == 3) ? "uvec3"
              : (pc == 2) ? "uvec2" : "uint";
          else
            ity = (pc >= 4) ? "ivec4" : (pc == 3) ? "ivec3"
              : (pc == 2) ? "ivec2" : "int";
          cogl_vk_debug_log ("draw",
                             "assemble VERT position as %s (int VI)",
                             ity);
          /* GLSL: cannot init globals from `in` (constant-expr only) — that
           * zeroed positions → black geometry. Declare + fill in transform.
           */
          g_string_append_printf (
            vert,
            "layout(location = 0) in %s _cogl_position_i;\n"
            "vec4 cogl_position_in;\n",
            ity);
        }
      else
        {
          g_string_append (vert,
                           "layout(location = 0) in vec4 cogl_position_in;\n");
        }
      g_string_append (vert,
                       "layout(location = 1) in vec4 cogl_color_in;\n");
      /* Use the same compact mapping as the vertex bindings. */
      for (i = 0; i < n_layers; i++)
        {
          CoglPipelineLayer *layer = layers[i];
          int idx = layer ? layer->index : i;
          int loc = vbo_texcoord_attr_location (vbo_attrs, idx);

          if (loc >= 0)
            {
              g_string_append_printf (vert,
                                      "layout(location = %d) in vec4 "
                                      "cogl_tex_coord%d_in;\n",
                                      loc, idx);
            }
          else
            {
              g_string_append_printf (vert, "vec4 cogl_tex_coord%d_in;\n",
                                      idx);
            }
        }
      if (n_layers == 0)
        {
          g_string_append_printf (vert,
                                  "layout(location = %d) in vec4 "
                                  "cogl_tex_coord0_in;\n",
                                  COGL_VK_VBO_LOC_TEXCOORD0);
        }
      g_string_append_printf (vert,
                              "layout(location = %d) in float "
                              "cogl_point_size_in;\n",
                              COGL_VK_VBO_LOC_POINT_SIZE);
      if (vbo_attrs && vbo_attrs->normal_components > 0)
        normal_comps = vbo_attrs->normal_components;
      g_string_append_printf (vert,
                              "layout(location = %d) in %s cogl_normal_in;\n",
                              COGL_VK_VBO_LOC_NORMAL,
                              glsl_vec_type (normal_comps));
      if (vbo_attrs)
        {
          for (nc = 0; nc < vbo_attrs->n_custom; nc++)
            {
              const char *name = vbo_attrs->custom_names[nc];
              int comps = vbo_attrs->custom_components[nc];

              if (!name || !*name)
                continue;
              if (comps < 1)
                comps = 4;
              if (vbo_attrs->custom_as_int[nc])
                {
                  gboolean u = vbo_attrs->custom_unsigned[nc];
                  const char *ity;

                  if (u)
                    ity = (comps >= 4) ? "uvec4" : (comps == 3) ? "uvec3"
                      : (comps == 2) ? "uvec2" : "uint";
                  else
                    ity = (comps >= 4) ? "ivec4" : (comps == 3) ? "ivec3"
                      : (comps == 2) ? "ivec2" : "int";
                  cogl_vk_debug_log (
                    "draw",
                    "assemble emit CUSTOM in loc=%d name=%s as %s→vec (runtime assign)",
                    vbo_attrs->custom_base + nc, name, ity);
                  /* Declare i/uvec `in` + float var; assign in
                   * generated_source
                   * (global init from `in` is illegal / zeros).
                   */
                  g_string_append_printf (
                    vert,
                    "layout(location = %d) in %s _cogl_custom_i_%d;\n"
                    "%s %s;\n",
                    vbo_attrs->custom_base + nc, ity, nc,
                    glsl_vec_type (comps), name);
                }
              else
                {
                  cogl_vk_debug_log (
                    "draw",
                    "assemble emit CUSTOM in loc=%d name=%s comps=%d",
                    vbo_attrs->custom_base + nc, name, comps);
                  g_string_append_printf (vert,
                                          "layout(location = %d) in %s %s;\n",
                                          vbo_attrs->custom_base + nc,
                                          glsl_vec_type (comps), name);
                }
            }
        }
    }
  else
    {
      g_string_append (vert,
                       "layout(push_constant, row_major) uniform VertPC {\n"
                       "  mat4 proj;\n"
                       "  vec2 uv_offset;\n"
                       "  vec2 uv_size;\n"
                       "} data;\n"
                       "const vec2 values[4] = vec2[](vec2(0,0), vec2(1,0), "
                       "vec2(1,1), vec2(0,1));\n");
    }

  if (n_layers > 0)
    {
      g_string_append_printf (vert,
                              "layout(location = 0) %sout vec4 _cogl_tex_coord[%d];\n"
                              "layout(location = %d) %sout vec4 v_color;\n",
                              interpolation, n_layers, color_loc, interpolation);
    }
  else
    {
      g_string_append_printf (vert, "layout(location = 0) %sout vec4 v_color;\n",
                              interpolation);
    }
  g_string_append (vert,
                   "/* COGL_VK_UBO_SLOT */\n"
                   "vec4 cogl_position_out;\n"
                   "float cogl_point_size_out;\n");

  /* GL vertex boilerplate parity (cogl-glsl-shader-boilerplate.h). */
  if (vert_mode == COGL_VK_ASSEMBLE_VERT_VBO)
    {
      g_string_append (vert,
                       "#define cogl_modelview_projection_matrix data.mvp\n"
                       "#define cogl_modelview_matrix data.mvp\n"
                       "#define cogl_projection_matrix data.mvp\n");
    }
  else
    {
      g_string_append (vert,
                       "#define cogl_modelview_projection_matrix data.proj\n"
                       "#define cogl_modelview_matrix data.proj\n"
                       "#define cogl_projection_matrix data.proj\n");
    }
  g_string_append (vert,
                   "#define cogl_color_out v_color\n"
                   "#define cogl_tex_coord_out _cogl_tex_coord\n"
                   "/* Redeclares of `attribute`/`varying` are stripped below;\n"
                   " * leftover uses map via the #defines above. */\n");

  /* cogl_tex_coord_in aliases layer-0 input (GL boilerplate). */
  {
    int base_idx = 0;

    if (n_layers > 0 && layers[0])
      base_idx = layers[0]->index;
    g_string_append_printf (vert,
                            "#define cogl_tex_coord_in cogl_tex_coord%d_in\n",
                            base_idx);
  }

  for (i = 0; i < n_layers; i++)
    {
      CoglPipelineLayer *layer = layers[i];
      int idx = layer ? layer->index : i;
      int unit = layer ? _cogl_pipeline_layer_get_unit_index (layer) : i;

      g_string_append_printf (vert,
                              "#define cogl_tex_coord%i_out _cogl_tex_coord[%i]\n",
                              idx, unit);
    }

  /* Push-quad: mutable globals so VERTEX_TRANSFORM / hooks see them (GL
   * attrs).
   */
  if (vert_mode == COGL_VK_ASSEMBLE_VERT_PUSH_QUAD)
    {
      g_string_append (vert,
                       "vec4 cogl_position_in;\n"
                       "vec4 cogl_color_in;\n"
                       "float cogl_point_size_in = 0.0;\n");
      for (i = 0; i < n_layers; i++)
        {
          CoglPipelineLayer *layer = layers[i];
          int idx = layer ? layer->index : i;
          g_string_append_printf (vert, "vec4 cogl_tex_coord%i_in;\n", idx);
        }
      if (n_layers == 0)
        g_string_append (vert, "vec4 cogl_tex_coord0_in;\n");
    }

  /* VERTEX_GLOBALS often redeclares `attribute vec4 color` while VBO already
   * emitted layout(location=N) in vec4 color — strip attribute lines.
   */
  if (vert_snippets)
    {
      g_autoptr (GString) decls = g_string_new (NULL);
      g_auto (GStrv) lines = NULL;
      char **iter;

      _cogl_pipeline_snippet_generate_declarations (decls,
                                                    COGL_SNIPPET_HOOK_VERTEX_GLOBALS,
                                                    vert_snippets);
      lines = g_strsplit (decls->str, "\n", -1);
      for (iter = lines; iter && *iter; iter++)
        {
          const char *line = *iter;
          while (*line == ' ' || *line == '\t')
            line++;
          if (g_str_has_prefix (line, "attribute ") ||
              g_str_has_prefix (line, "attribute\t"))
            {
              cogl_vk_debug_log (
                "draw",
                "assemble skip VERTEX_GLOBALS attribute redecl: %s",
                line);
              continue;
            }
          g_string_append (vert_header, *iter);
          g_string_append_c (vert_header, '\n');
        }
    }

  /* Per-layer TEXTURE_COORD_TRANSFORM (GL vertend_add_layer). */
  for (i = 0; i < n_layers; i++)
    {
      CoglPipelineLayer *layer = layers[i];
      CoglPipelineSnippetList *layer_snips;
      int idx = layer ? layer->index : i;
      CoglPipelineSnippetData snippet_data = {0};
      g_autofree char *chain = NULL;
      g_autofree char *final_name = NULL;
      g_autofree char *prefix = NULL;

      g_string_append_printf (vert_header,
                              "vec4 cogl_real_transform_layer%i(mat4 matrix, "
                              "vec4 tex_coord) {\n"
                              "  return matrix * tex_coord;\n"
                              "}\n",
                              idx);

      layer_snips = layer ? get_layer_vertex_snippets (layer) : NULL;
      chain = g_strdup_printf ("cogl_real_transform_layer%i", idx);
      final_name = g_strdup_printf ("cogl_transform_layer%i", idx);
      prefix = g_strdup_printf ("cogl_transform_layer_hook%i", idx);
      snippet_data.snippets = snippets_or_empty (layer_snips);
      snippet_data.hook = COGL_SNIPPET_HOOK_TEXTURE_COORD_TRANSFORM;
      snippet_data.chain_function = chain;
      snippet_data.final_name = final_name;
      snippet_data.function_prefix = prefix;
      snippet_data.return_type = "vec4";
      snippet_data.return_variable = "cogl_tex_coord";
      snippet_data.return_variable_is_argument = TRUE;
      snippet_data.arguments = "cogl_matrix, cogl_tex_coord";
      snippet_data.argument_declarations =
        "mat4 cogl_matrix, vec4 cogl_tex_coord";
      snippet_data.source_buf = vert_header;
      _cogl_pipeline_snippet_generate_code (&snippet_data);
    }

  /* VERTEX_TRANSFORM real + wrap */
  if (vert_mode == COGL_VK_ASSEMBLE_VERT_VBO)
    {
      g_string_append (vert_header,
                       "void cogl_real_vertex_transform(void) {\n"
                       "  vec4 pos = cogl_position_in;\n"
                       "  if (pos.w == 0.0) pos.w = 1.0;\n"
                       "  cogl_position_out = data.mvp * pos;\n"
                       "}\n");
    }
  else
    {
      g_string_append (vert_header,
                       "void cogl_real_vertex_transform(void) {\n"
                       "  cogl_position_out = data.proj * cogl_position_in;\n"
                       "}\n");
    }

  {
    CoglPipelineSnippetData snippet_data = {0};
    snippet_data.snippets = snippets_or_empty (vert_snippets);
    snippet_data.hook = COGL_SNIPPET_HOOK_VERTEX_TRANSFORM;
    snippet_data.chain_function = "cogl_real_vertex_transform";
    snippet_data.final_name = "cogl_vertex_transform";
    snippet_data.function_prefix = "cogl_vertex_transform";
    snippet_data.source_buf = vert_header;
    _cogl_pipeline_snippet_generate_code (&snippet_data);
  }

  if (per_vertex_ps)
    {
      CoglPipelineSnippetData snippet_data = {0};

      g_string_append (vert_header,
                       "void cogl_real_point_size_calculation(void) {\n"
                       "  cogl_point_size_out = cogl_point_size_in;\n"
                       "}\n");
      snippet_data.snippets = snippets_or_empty (vert_snippets);
      snippet_data.hook = COGL_SNIPPET_HOOK_POINT_SIZE;
      snippet_data.chain_function = "cogl_real_point_size_calculation";
      snippet_data.final_name = "cogl_point_size_calculation";
      snippet_data.function_prefix = "cogl_point_size_calculation";
      snippet_data.source_buf = vert_header;
      _cogl_pipeline_snippet_generate_code (&snippet_data);
    }

  g_string_append (vert, vert_header->str);

  /* cogl_generated_source — GL vertend order: texcoord transform, then
   * vertex_transform, point size, color.
   */
  g_string_append (vert_source,
                   "void\n"
                   "cogl_generated_source ()\n"
                   "{\n");

  if (vert_mode == COGL_VK_ASSEMBLE_VERT_PUSH_QUAD)
    {
      g_string_append (vert_source,
                       "  vec2 pos = values[gl_VertexIndex % 4];\n"
                       "  cogl_position_in = vec4(pos, 0.0, 1.0);\n"
                       "  cogl_color_in = vec4(1.0);\n"
                       "  {\n"
                       "    vec4 _quad_texcoord = "
                       "vec4(data.uv_offset + pos * data.uv_size, 0.0, 1.0);\n");
      for (i = 0; i < n_layers; i++)
        {
          CoglPipelineLayer *layer = layers[i];
          int idx = layer ? layer->index : i;
          g_string_append_printf (vert_source,
                                  "    cogl_tex_coord%i_in = _quad_texcoord;\n",
                                  idx);
        }
      g_string_append (vert_source, "  }\n");
      g_string_append_printf (vert_source,
                              "  cogl_point_size_out = %f;\n",
                              point_size);
    }
  else
    {
      g_string_append (vert_source,
                       "  cogl_point_size_out = data.point_size_pad.x;\n");
      /* Match GL's default generic attribute value for missing inputs. */
      for (i = 0; i < n_layers; i++)
        {
          CoglPipelineLayer *layer = layers[i];
          int idx = layer ? layer->index : i;

          if (vbo_texcoord_attr_location (vbo_attrs, idx) >= 0)
            continue;
          g_string_append_printf (vert_source,
                                  "  cogl_tex_coord%i_in = "
                                  "vec4 (0.0, 0.0, 0.0, 1.0);\n",
                                  idx);
        }
    }

  for (i = 0; i < n_layers; i++)
    {
      CoglPipelineLayer *layer = layers[i];
      int idx = layer ? layer->index : i;

      g_string_append_printf (vert_source,
                              "  cogl_tex_coord%i_out = "
                              "cogl_transform_layer%i(cogl_texture_matrix%i, "
                              "cogl_tex_coord%i_in);\n",
                              idx, idx, idx, idx);
    }

  g_string_append (vert_source, "  cogl_vertex_transform();\n");
  if (per_vertex_ps)
    g_string_append (vert_source, "  cogl_point_size_calculation();\n");
  /* Write via cogl_color_out so VERTEX replace hooks that assign it win. */
  g_string_append (vert_source, "  cogl_color_out = cogl_color_in;\n}\n");

  /* VERTEX wrap + main; flip_vector when any vertex snippets (GL vertend). */
  {
    CoglPipelineSnippetData snippet_data = {0};

    snippet_data.snippets = snippets_or_empty (vert_snippets);
    snippet_data.hook = COGL_SNIPPET_HOOK_VERTEX;
    snippet_data.chain_function = "cogl_generated_source";
    snippet_data.final_name = "cogl_vertex_hook";
    snippet_data.function_prefix = "cogl_vertex_hook";
    snippet_data.source_buf = vert_source;
    _cogl_pipeline_snippet_generate_code (&snippet_data);
  }

  if (has_vert)
    {
      /* Name remapped to snip._cogl_flip_vector by pack_uniforms inject.
       * Requires cold DS UBO stageFlags include VERTEX (see pipeline-vk).
       */
      cogl_vk_debug_log (
        "draw",
        "assemble VERT uses UBO _cogl_flip_vector (needs VERTEX stage on "
        "binding1)");
      g_string_append (vert_source,
                       "/* flip via UBO; see pack_uniforms */\n");
      g_string_append_printf (vert_source,
                              "void main() {\n"
                              "  cogl_vertex_hook();\n"
                              "  cogl_position_out *= _cogl_flip_vector;\n"
                              "  /* Cogl/GL clip z [-1,1] → Vulkan [0,1] */\n"
                              "  vec4 clip = cogl_position_out;\n"
                              "  clip.z = clip.z * 0.5 + clip.w * 0.5;\n"
                              "  gl_Position = clip;\n"
                              "  gl_PointSize = max(cogl_point_size_out, %f);\n"
                              "}\n",
                              point_size);
    }
  else
    {
      g_string_append_printf (vert_source,
                              "void main() {\n"
                              "  cogl_vertex_hook();\n"
                              "  /* Cogl/GL clip z [-1,1] → Vulkan [0,1] */\n"
                              "  vec4 clip = cogl_position_out;\n"
                              "  clip.z = clip.z * 0.5 + clip.w * 0.5;\n"
                              "  gl_Position = clip;\n"
                              "  gl_PointSize = max(cogl_point_size_out, %f);\n"
                              "}\n",
                              point_size);
    }

  g_string_append (vert, vert_source->str);

  /* Strip legacy `attribute …;` from snippet declarations. VERTEX inputs
   * are already `layout(location) in`. `#define attribute in` would
   * otherwise redeclare the same name (point-size attribute snippet).
   */
  {
    g_auto (GStrv) lines = g_strsplit (vert->str, "\n", -1);
    GString *cleaned = g_string_new (NULL);
    char **iter;

    for (iter = lines; iter && *iter; iter++)
      {
        const char *line = *iter;
        while (*line == ' ' || *line == '\t')
          line++;
        if (g_str_has_prefix (line, "attribute ") ||
            g_str_has_prefix (line, "attribute\t"))
          {
            cogl_vk_debug_log ("draw",
                               "assemble strip VERTEX attribute: %s", line);
            continue;
          }
        g_string_append (cleaned, *iter);
        g_string_append_c (cleaned, '\n');
      }
    g_string_assign (vert, cleaned->str);
    g_string_free (cleaned, TRUE);
  }

  /* After both stages exist so snippet `varying name` shares one location. */
  promote_legacy_varyings (vert, frag, color_loc + 1);

  out->frag_glsl = g_string_free (g_steal_pointer (&frag), FALSE);
  out->vert_glsl = g_string_free (g_steal_pointer (&vert), FALSE);
  return TRUE;
}

typedef struct
{
  unsigned int age;
  char *fingerprint;
} PipelineFingerprint;

static void
pipeline_fingerprint_free (gpointer data)
{
  PipelineFingerprint *cached = data;
  g_free (cached->fingerprint);
  g_free (cached);
}

char *
cogl_vk_assemble_pipeline_fingerprint (CoglPipeline *pipeline)
{
  COGL_VK_PROFILE_SCOPE ("cogl_vk_assemble_pipeline_fingerprint");
  g_autoptr (GChecksum) sum = NULL;
  CoglVkAssembleResult assembled = {0};
  g_autoptr (GError) err = NULL;
  PipelineFingerprint *cached;
  CoglPipeline *authority;
  const char *cache_key = "cogl-vk-assemble-fingerprint";

  /* Share with an equivalent ancestor, as the GL shader caches do.
   * Pipeline color and uniform values are runtime inputs, not GLSL literals.
   * Keep every other sparse state and all layer state in the equivalence
   * test, including Vulkan's baked alpha reference and combine constants.
   * Copy-on-write preserves descendants when an authority changes.
   */
  authority = _cogl_pipeline_find_equivalent_parent (
    pipeline,
    COGL_PIPELINE_STATE_ALL_SPARSE & ~(COGL_PIPELINE_STATE_COLOR |
                                       COGL_PIPELINE_STATE_UNIFORMS |
                                       COGL_PIPELINE_STATE_LAYERS),
    COGL_PIPELINE_LAYER_STATE_ALL_SPARSE);
  cached = g_object_get_data (G_OBJECT (authority), cache_key);
  if (cached && cached->age == authority->age)
    return g_strdup (cached->fingerprint);
  sum = g_checksum_new (G_CHECKSUM_SHA1);

  if (!cogl_vk_assemble_pipeline_glsl (pipeline,
                                       COGL_VK_ASSEMBLE_VERT_PUSH_QUAD,
                                       NULL,
                                       &assembled, &err))
    return g_strdup ("assemble-fail");

  g_checksum_update (sum, (const unsigned char *) assembled.vert_glsl,
                     strlen (assembled.vert_glsl));
  g_checksum_update (sum, (const unsigned char *) assembled.frag_glsl,
                     strlen (assembled.frag_glsl));
  cogl_vk_assemble_result_clear (&assembled);
  cached = g_new0 (PipelineFingerprint, 1);
  cached->age = authority->age;
  cached->fingerprint = g_strdup (g_checksum_get_string (sum));
  g_object_set_data_full (G_OBJECT (authority), cache_key, cached,
                          pipeline_fingerprint_free);
  return g_strdup (cached->fingerprint);
}
