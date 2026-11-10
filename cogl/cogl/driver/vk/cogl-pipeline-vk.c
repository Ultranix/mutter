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

/* Vulkan render setup, descriptor management and Cogl pipeline state.
 * Simple texture and solid draws use embedded shaders. Generated shaders
 * handle texture layer combines and Cogl snippet hooks.
 */

#include "config.h"

#include "cogl/driver/vk/cogl-pipeline-vk-private.h"
#include "cogl/driver/vk/cogl-pipeline-vk-glsl-private.h"
#include "cogl/driver/vk/cogl-pipeline-vk-assemble-private.h"
#include "cogl/driver/vk/cogl-driver-vk-private.h"
#include "cogl/driver/vk/cogl-texture-driver-vk-private.h"
#include "cogl/driver/vk/cogl-image-vk-private.h"
#include "cogl/driver/vk/cogl-vk-debug-private.h"

#include "cogl/cogl-bitmask.h"
#include "cogl/cogl-boxed-value.h"
#include "cogl/cogl-color.h"
#include "cogl/cogl-context-private.h"
#include "cogl/cogl-framebuffer.h"
#include "cogl/cogl-pipeline-layer-private.h"
#include "cogl/cogl-pipeline-layer-state.h"
#include "cogl/cogl-pipeline-private.h"
#include "cogl/cogl-pipeline-state-private.h"
#include "cogl/cogl-snippet.h"
#include "cogl/cogl-types.h"

#if defined(HAVE_GL)
#include <GL/gl.h>
#elif defined(HAVE_GLES2)
#include <GLES2/gl2.h>
#endif
#ifndef GL_MIRRORED_REPEAT
#define GL_MIRRORED_REPEAT 0x8370
#endif

#include "cogl/driver/vk/shaders/common.vert.h"
#include "cogl/driver/vk/shaders/texture.frag.h"
#include "cogl/driver/vk/shaders/quad.frag.h"

#include <gio/gio.h>
#include <graphene.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int max_layers_for_limits (const VkPhysicalDeviceLimits *limits);

#ifndef GL_MIN
#define GL_MIN 0x8007
#endif
#ifndef GL_MAX
#define GL_MAX 0x8008
#endif
#ifndef GL_FUNC_ADD
#define GL_FUNC_ADD 0x8006
#endif
#ifndef GL_FUNC_SUBTRACT
#define GL_FUNC_SUBTRACT 0x800A
#endif
#ifndef GL_FUNC_REVERSE_SUBTRACT
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#endif
#ifndef GL_CONSTANT_COLOR
#define GL_CONSTANT_COLOR 0x8001
#endif
#ifndef GL_ONE_MINUS_CONSTANT_COLOR
#define GL_ONE_MINUS_CONSTANT_COLOR 0x8002
#endif
#ifndef GL_CONSTANT_ALPHA
#define GL_CONSTANT_ALPHA 0x8003
#endif
#ifndef GL_ONE_MINUS_CONSTANT_ALPHA
#define GL_ONE_MINUS_CONSTANT_ALPHA 0x8004
#endif
#ifndef GL_SRC_ALPHA_SATURATE
#define GL_SRC_ALPHA_SATURATE 0x0308
#endif

typedef struct
{
  CoglPipelineLayer **layers;
  int n;
  int capacity;
} CoglVkCollectLayersData;

static gboolean
cogl_vk_collect_layers_cb (CoglPipelineLayer *layer,
                           void              *user_data)
{
  CoglVkCollectLayersData *data = user_data;

  if (data->n >= data->capacity)
    {
      data->capacity = data->capacity ? data->capacity * 2 : 8;
      data->layers = g_realloc (data->layers,
                                sizeof (CoglPipelineLayer *) * data->capacity);
    }
  data->layers[data->n++] = layer;
  return TRUE;
}

void
cogl_vk_pipeline_collect_layers (CoglPipeline        *pipeline,
                                 CoglPipelineLayer ***layers_out,
                                 int                 *n_out)
{
  CoglVkCollectLayersData data = { 0 };
  g_autoptr (GString) indices = NULL;
  int i;

  g_return_if_fail (layers_out != NULL);
  g_return_if_fail (n_out != NULL);

  *layers_out = NULL;
  *n_out = 0;

  if (!pipeline)
    return;

  _cogl_pipeline_foreach_layer_internal (pipeline,
                                         cogl_vk_collect_layers_cb,
                                         &data);

  indices = g_string_new (NULL);
  for (i = 0; i < data.n; i++)
    {
      if (i > 0)
        g_string_append_c (indices, ',');
      g_string_append_printf (indices, "%d", data.layers[i]->index);
    }
  cogl_vk_debug_log ("draw",
                     "collect_layers n=%d indices=[%s]",
                     data.n, indices->str);

  *layers_out = data.layers;
  *n_out = data.n;
}

/* Extra fields hang off pass via this helper stored... we extend pass struct.
 */

gboolean
cogl_vk_should_bake_y_flip_projection (CoglFramebuffer *fb,
                                       CoglPipeline    *pipeline)
{
  if (!fb || !cogl_framebuffer_is_y_flipped (fb))
    return FALSE;
  /* Vertex snippets → `_cogl_flip_vector` in assemble main (R2); never bake. */
  if (pipeline && _cogl_pipeline_has_vertex_snippets (pipeline))
    return FALSE;
  return TRUE;
}

void
cogl_vk_maybe_bake_y_flip_projection (CoglFramebuffer   *fb,
                                      CoglPipeline      *pipeline,
                                      graphene_matrix_t *projection)
{
  CoglContext *ctx;
  graphene_matrix_t tmp;

  g_return_if_fail (projection != NULL);

  if (!cogl_vk_should_bake_y_flip_projection (fb, pipeline))
    return;

  ctx = cogl_framebuffer_get_context (fb);
  if (!ctx)
    return;

  tmp = *projection;
  graphene_matrix_multiply (&tmp,
                            cogl_context_get_y_flip_matrix (ctx),
                            projection);
}

float
cogl_vk_shader_wrap_mode (int wrap_enum)
{
  switch (wrap_enum)
    {
    case GL_REPEAT:
      return 1.f;
    case GL_MIRRORED_REPEAT:
      return 2.f;
    default:
      return 0.f;
    }
}

void
cogl_vk_vert_push_set_matrix (CoglVkVertPush          *out,
                              const graphene_matrix_t *matrix)
{
  float col[16];
  int r, c;

  g_return_if_fail (out != NULL);
  g_return_if_fail (matrix != NULL);

  graphene_matrix_to_float (matrix, col);
  memset (out, 0, sizeof (*out));
  /* Push const is row_major; graphene_matrix_to_float is column-major. */
  for (r = 0; r < 4; r++)
    for (c = 0; c < 4; c++)
      out->mat4[r][c] = col[c * 4 + r];
  out->uv_off[0] = 0.f;
  out->uv_off[1] = 0.f;
  out->uv_size[0] = 1.f;
  out->uv_size[1] = 1.f;
}

void
cogl_vk_fill_vert_push_mvp (CoglFramebuffer *fb,
                            CoglPipeline    *pipeline,
                            CoglVkVertPush  *out)
{
  graphene_matrix_t mv, proj, mvp;

  g_return_if_fail (out != NULL);

  graphene_matrix_init_identity (&mv);
  graphene_matrix_init_identity (&proj);
  if (fb)
    {
      CoglContext *ctx = cogl_framebuffer_get_context (fb);
      CoglMatrixEntry *mv_entry = NULL;
      CoglMatrixEntry *proj_entry = NULL;

      /* Journal flush loads identity into the context current entry after
       * software-transforming vertices. The framebuffer stack still holds
       * the user matrix — reading that applies modelview twice.
       */
      if (ctx)
        {
          mv_entry = cogl_context_get_current_modelview_entry (ctx);
          proj_entry = cogl_context_get_current_projection_entry (ctx);
        }
      if (mv_entry)
        cogl_matrix_entry_get (mv_entry, &mv);
      else
        cogl_framebuffer_get_modelview_matrix (fb, &mv);
      if (proj_entry)
        cogl_matrix_entry_get (proj_entry, &proj);
      else
        cogl_framebuffer_get_projection_matrix (fb, &proj);
      cogl_vk_maybe_bake_y_flip_projection (fb, pipeline, &proj);
    }
  /* Row-vector Graphene: MVP = MV × Proj (see fan_quad). */
  graphene_matrix_multiply (&mv, &proj, &mvp);
  cogl_vk_vert_push_set_matrix (out, &mvp);
  if (pipeline)
    {
      float ps = cogl_pipeline_get_point_size (pipeline);
      if (ps <= 0.f)
        ps = 1.f;
      out->uv_off[0] = ps;
    }
}

void
cogl_vk_vert_push_ortho_uv_unit (CoglVkVertPush  *out,
                                 CoglFramebuffer *fb,
                                 CoglPipeline    *pipeline)
{
  gboolean flip = cogl_vk_should_bake_y_flip_projection (fb, pipeline);

  /* Map unit quad [0,1]^2 to NDC [-1,1]^2 (row-major, matches common.vert).
   * When baking y-flip: map Y to [1,-1] instead (GL y_flip_matrix).
   */
  memset (out, 0, sizeof (*out));
  out->mat4[0][0] = 2.f;
  out->mat4[1][1] = flip ? -2.f : 2.f;
  out->mat4[2][2] = 1.f;
  out->mat4[3][3] = 1.f;
  out->mat4[0][3] = -1.f;
  out->mat4[1][3] = flip ? 1.f : -1.f;
  out->uv_off[0] = 0.f;
  out->uv_off[1] = 0.f;
  out->uv_size[0] = 1.f;
  out->uv_size[1] = 1.f;
}

static gboolean
create_shader_module (VkDevice         device,
                      const uint32_t  *code,
                      size_t           code_bytes,
                      VkShaderModule  *out,
                      GError         **error)
{
  VkShaderModuleCreateInfo info = {
    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    .codeSize = code_bytes,
    .pCode = code,
  };
  VkResult res = vkCreateShaderModule (device, &info, NULL, out);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkCreateShaderModule failed (%d)", (int) res);
      return FALSE;
    }
  return TRUE;
}

static VkBlendFactor
cogl_vk_map_gl_blend_factor (GLint factor)
{
  switch (factor)
    {
    case GL_ZERO:
      return VK_BLEND_FACTOR_ZERO;
    case GL_ONE:
      return VK_BLEND_FACTOR_ONE;
    case GL_SRC_COLOR:
      return VK_BLEND_FACTOR_SRC_COLOR;
    case GL_ONE_MINUS_SRC_COLOR:
      return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case GL_DST_COLOR:
      return VK_BLEND_FACTOR_DST_COLOR;
    case GL_ONE_MINUS_DST_COLOR:
      return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case GL_SRC_ALPHA:
      return VK_BLEND_FACTOR_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA:
      return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case GL_DST_ALPHA:
      return VK_BLEND_FACTOR_DST_ALPHA;
    case GL_ONE_MINUS_DST_ALPHA:
      return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case GL_CONSTANT_COLOR:
      return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case GL_ONE_MINUS_CONSTANT_COLOR:
      return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case GL_CONSTANT_ALPHA:
      return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case GL_ONE_MINUS_CONSTANT_ALPHA:
      return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case GL_SRC_ALPHA_SATURATE:
      return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default:
      return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp
cogl_vk_map_gl_blend_op (GLenum equation)
{
  switch (equation)
    {
    case GL_FUNC_ADD:
      return VK_BLEND_OP_ADD;
    case GL_FUNC_SUBTRACT:
      return VK_BLEND_OP_SUBTRACT;
    case GL_FUNC_REVERSE_SUBTRACT:
      return VK_BLEND_OP_REVERSE_SUBTRACT;
    case GL_MIN:
      return VK_BLEND_OP_MIN;
    case GL_MAX:
      return VK_BLEND_OP_MAX;
    default:
      return VK_BLEND_OP_ADD;
    }
}

static VkCullModeFlags
cogl_vk_map_cull_mode (CoglPipelineCullFaceMode mode)
{
  switch (mode)
    {
    case COGL_PIPELINE_CULL_FACE_MODE_NONE:
      return VK_CULL_MODE_NONE;
    case COGL_PIPELINE_CULL_FACE_MODE_FRONT:
      return VK_CULL_MODE_FRONT_BIT;
    case COGL_PIPELINE_CULL_FACE_MODE_BACK:
      return VK_CULL_MODE_BACK_BIT;
    case COGL_PIPELINE_CULL_FACE_MODE_BOTH:
      return VK_CULL_MODE_FRONT_AND_BACK;
    default:
      return VK_CULL_MODE_NONE;
    }
}

static VkCompareOp
cogl_vk_map_depth_func (CoglDepthTestFunction func)
{
  switch (func)
    {
    case COGL_DEPTH_TEST_FUNCTION_NEVER:
      return VK_COMPARE_OP_NEVER;
    case COGL_DEPTH_TEST_FUNCTION_LESS:
      return VK_COMPARE_OP_LESS;
    case COGL_DEPTH_TEST_FUNCTION_EQUAL:
      return VK_COMPARE_OP_EQUAL;
    case COGL_DEPTH_TEST_FUNCTION_LEQUAL:
      return VK_COMPARE_OP_LESS_OR_EQUAL;
    case COGL_DEPTH_TEST_FUNCTION_GREATER:
      return VK_COMPARE_OP_GREATER;
    case COGL_DEPTH_TEST_FUNCTION_NOTEQUAL:
      return VK_COMPARE_OP_NOT_EQUAL;
    case COGL_DEPTH_TEST_FUNCTION_GEQUAL:
      return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case COGL_DEPTH_TEST_FUNCTION_ALWAYS:
      return VK_COMPARE_OP_ALWAYS;
    default:
      return VK_COMPARE_OP_LESS;
    }
}

static gboolean
vk_blend_factor_uses_constant (VkBlendFactor factor)
{
  return (factor == VK_BLEND_FACTOR_CONSTANT_COLOR ||
          factor == VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR ||
          factor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
          factor == VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA);
}

static gboolean
extras_need_blend_constants (const CoglVkTexPipeExtras *extras)
{
  if (!extras)
    return FALSE;
  if (extras->dynamic_blend_constants)
    return TRUE;
  return (vk_blend_factor_uses_constant (extras->src_color) ||
          vk_blend_factor_uses_constant (extras->dst_color) ||
          vk_blend_factor_uses_constant (extras->src_alpha) ||
          vk_blend_factor_uses_constant (extras->dst_alpha));
}

/* Hot extras PSO kinds — match format_setup baked pipe roles. */
enum
{
  COGL_VK_HOT_KIND_TEX = 0,
  COGL_VK_HOT_KIND_TEX_OPAQUE = 1,
  COGL_VK_HOT_KIND_YCBCR = 2,
  COGL_VK_HOT_KIND_YCBCR_OPAQUE = 3,
  COGL_VK_HOT_KIND_QUAD = 4,
};

typedef struct _CoglVkHotExtrasKey
{
  VkFormat format;
  VkFormat ds_format;
  VkSampleCountFlagBits samples;
  uint8_t kind;
  uint8_t ycbcr_index;
  CoglVkPipeStateKey state;
} CoglVkHotExtrasKey;

typedef struct _CoglVkHotExtrasEntry
{
  CoglVkHotExtrasKey key;
  VkPipeline pipeline;
} CoglVkHotExtrasEntry;

static unsigned int
hot_extras_key_hash (gconstpointer key)
{
  const CoglVkHotExtrasKey *k = key;

  return cogl_vk_pipe_state_key_hash (&k->state) ^
         (unsigned int) k->format ^ ((unsigned int) k->ds_format << 3) ^
         ((unsigned int) k->samples << 6) ^ ((unsigned int) k->kind << 12) ^
         ((unsigned int) k->ycbcr_index << 20);
}

static gboolean
hot_extras_key_equal (gconstpointer a,
                      gconstpointer b)
{
  const CoglVkHotExtrasKey *ka = a;
  const CoglVkHotExtrasKey *kb = b;

  return ka->format == kb->format &&
         ka->ds_format == kb->ds_format &&
         ka->samples == kb->samples &&
         ka->kind == kb->kind &&
         ka->ycbcr_index == kb->ycbcr_index &&
         cogl_vk_pipe_state_key_equal (&ka->state, &kb->state);
}

gboolean
cogl_vk_tex_pipe_extras_need_keyed_hot (const CoglVkTexPipeExtras *extras,
                                        gboolean                   blend_enable)
{
  /* Baked format_setup pipes: src-over (or opaque), cull none, depth off,
   * front CCW. front_face only matters once cull is directional — covered by
   * cull_mode != NONE forcing a keyed pipe that then embeds front_face.
   */
  if (!extras)
    return FALSE;

  if (extras->cull_mode != VK_CULL_MODE_NONE)
    return TRUE;
  if (extras->depth_test_enable)
    return TRUE;
  if (extras_need_blend_constants (extras))
    return TRUE;

  if (blend_enable)
    {
      if (extras->src_color != VK_BLEND_FACTOR_ONE ||
          extras->dst_color != VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA ||
          extras->color_op != VK_BLEND_OP_ADD ||
          extras->src_alpha != VK_BLEND_FACTOR_ONE ||
          extras->dst_alpha != VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA ||
          extras->alpha_op != VK_BLEND_OP_ADD)
        return TRUE;
    }

  return FALSE;
}

void
cogl_vk_pipe_state_key_from_extras (CoglVkPipeStateKey        *out,
                                    const CoglVkTexPipeExtras *extras,
                                    gboolean                   blend_enable,
                                    uint32_t                   vertex_input_mask)
{
  g_return_if_fail (out != NULL);
  memset (out, 0, sizeof (*out));

  out->blend_enable = blend_enable ? 1 : 0;
  out->src_color = VK_BLEND_FACTOR_ONE;
  out->dst_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  out->color_op = VK_BLEND_OP_ADD;
  out->src_alpha = VK_BLEND_FACTOR_ONE;
  out->dst_alpha = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  out->alpha_op = VK_BLEND_OP_ADD;
  out->cull_mode = VK_CULL_MODE_NONE;
  out->front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  out->depth_compare_op = VK_COMPARE_OP_LESS;
  out->samples = VK_SAMPLE_COUNT_1_BIT;
  out->topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
  out->vertex_input_mask = vertex_input_mask;

  if (!extras)
    return;

  out->src_color = extras->src_color;
  out->dst_color = extras->dst_color;
  out->color_op = extras->color_op;
  out->src_alpha = extras->src_alpha;
  out->dst_alpha = extras->dst_alpha;
  out->alpha_op = extras->alpha_op;
  out->cull_mode = extras->cull_mode;
  out->front_face = extras->front_face;
  out->depth_test_enable = extras->depth_test_enable ? 1 : 0;
  out->depth_write_enable = extras->depth_write_enable ? 1 : 0;
  out->depth_compare_op = extras->depth_compare_op;
  out->stencil_equal_keep = extras->stencil_equal_keep ? 1 : 0;
  out->stencil_op_replace = extras->stencil_op_replace ? 1 : 0;
  out->stencil_op_incr = extras->stencil_op_incr ? 1 : 0;
  out->stencil_op_decr = extras->stencil_op_decr ? 1 : 0;
  out->samples = extras->samples > VK_SAMPLE_COUNT_1_BIT
    ? extras->samples : VK_SAMPLE_COUNT_1_BIT;
  out->has_topology = extras->has_topology ? 1 : 0;
  if (extras->has_topology)
    out->topology = extras->topology;
  out->dynamic_blend_constants =
    extras_need_blend_constants (extras) ? 1 : 0;
}

void
cogl_vk_pipe_state_key_from_pipeline (CoglVkPipeStateKey   *out,
                                      CoglPipeline         *pipeline,
                                      VkFrontFace           front_face,
                                      gboolean              blend_enable,
                                      VkSampleCountFlagBits samples,
                                      gboolean              stencil_equal_keep,
                                      gboolean              has_topology,
                                      VkPrimitiveTopology   topology,
                                      uint32_t              vertex_input_mask)
{
  CoglVkTexPipeExtras extras;

  cogl_vk_tex_pipe_extras_from_pipeline (pipeline, front_face, &extras);
  extras.samples = samples;
  extras.stencil_equal_keep = stencil_equal_keep;
  extras.has_topology = has_topology;
  extras.topology = topology;
  /* Cold/VBO: always declare dynamic blend constants for simplicity. */
  extras.dynamic_blend_constants = TRUE;
  cogl_vk_pipe_state_key_from_extras (out, &extras, blend_enable,
                                      vertex_input_mask);
}

unsigned int
cogl_vk_pipe_state_key_hash (const CoglVkPipeStateKey *key)
{
  unsigned int h;

  g_return_val_if_fail (key != NULL, 0);
  h = (unsigned int) key->blend_enable;
  h = (h * 33u) ^ (unsigned int) key->src_color;
  h = (h * 33u) ^ (unsigned int) key->dst_color;
  h = (h * 33u) ^ (unsigned int) key->color_op;
  h = (h * 33u) ^ (unsigned int) key->src_alpha;
  h = (h * 33u) ^ (unsigned int) key->dst_alpha;
  h = (h * 33u) ^ (unsigned int) key->alpha_op;
  h = (h * 33u) ^ (unsigned int) key->cull_mode;
  h = (h * 33u) ^ (unsigned int) key->front_face;
  h = (h * 33u) ^ (unsigned int) key->depth_test_enable;
  h = (h * 33u) ^ (unsigned int) key->depth_write_enable;
  h = (h * 33u) ^ (unsigned int) key->depth_compare_op;
  h = (h * 33u) ^ (unsigned int) key->stencil_equal_keep;
  h = (h * 33u) ^ (unsigned int) key->stencil_op_replace;
  h = (h * 33u) ^ (unsigned int) key->stencil_op_incr;
  h = (h * 33u) ^ (unsigned int) key->stencil_op_decr;
  h = (h * 33u) ^ (unsigned int) key->samples;
  h = (h * 33u) ^ (unsigned int) key->has_topology;
  h = (h * 33u) ^ (unsigned int) key->topology;
  h = (h * 33u) ^ key->vertex_input_mask;
  h = (h * 33u) ^ (unsigned int) key->dynamic_blend_constants;
  return h;
}

gboolean
cogl_vk_pipe_state_key_equal (const CoglVkPipeStateKey *a,
                              const CoglVkPipeStateKey *b)
{
  g_return_val_if_fail (a != NULL && b != NULL, FALSE);
  return memcmp (a, b, sizeof (*a)) == 0;
}

void
cogl_vk_pipe_state_key_append (GString                  *str,
                               const CoglVkPipeStateKey *key)
{
  g_return_if_fail (str != NULL && key != NULL);
  g_string_append_printf (
    str,
    "|pso=be%u,sc%u,dc%u,co%u,sa%u,da%u,ao%u,cu%u,ff%u,"
    "dt%u,dw%u,dcmp%u,ste%u,str%u,sti%u,std%u,sm%u,ht%u,tp%u,vim%x,dbc%u",
    (unsigned) key->blend_enable,
    (unsigned) key->src_color,
    (unsigned) key->dst_color,
    (unsigned) key->color_op,
    (unsigned) key->src_alpha,
    (unsigned) key->dst_alpha,
    (unsigned) key->alpha_op,
    (unsigned) key->cull_mode,
    (unsigned) key->front_face,
    (unsigned) key->depth_test_enable,
    (unsigned) key->depth_write_enable,
    (unsigned) key->depth_compare_op,
    (unsigned) key->stencil_equal_keep,
    (unsigned) key->stencil_op_replace,
    (unsigned) key->stencil_op_incr,
    (unsigned) key->stencil_op_decr,
    (unsigned) key->samples,
    (unsigned) key->has_topology,
    (unsigned) key->topology,
    (unsigned) key->vertex_input_mask,
    (unsigned) key->dynamic_blend_constants);
}

gboolean
cogl_vk_create_tex_pipeline (CoglVkPipelineCache       *cache,
                             VkRenderPass               rp,
                             VkShaderModule             vert,
                             VkShaderModule             frag,
                             VkPipelineLayout           layout,
                             gboolean                   blend_enable,
                             const CoglVkTexPipeExtras *extras,
                             VkPipeline                *out_pipe,
                             GError                   **error)
{
  VkDevice dev = cache->device->device;
  VkPipelineShaderStageCreateInfo stages[2] = {
    {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_VERTEX_BIT,
      .module = vert,
      .pName = "main",
    },
    {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
      .module = frag,
      .pName = "main",
    },
  };
  VkPipelineInputAssemblyStateCreateInfo assembly = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
  };
  VkPipelineRasterizationStateCreateInfo rasterization = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
    .polygonMode = VK_POLYGON_MODE_FILL,
    .cullMode = extras ? extras->cull_mode : VK_CULL_MODE_NONE,
    .frontFace = extras ? extras->front_face
                        : VK_FRONT_FACE_COUNTER_CLOCKWISE,
    .lineWidth = 1.f,
  };
  /* Hot default: GL src-over (ONE, ONE_MINUS_SRC_ALPHA). */
  VkPipelineColorBlendAttachmentState blend_attachment = {
    .blendEnable = blend_enable ? VK_TRUE : VK_FALSE,
    .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
    .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    .colorBlendOp = VK_BLEND_OP_ADD,
    .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
    .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    .alphaBlendOp = VK_BLEND_OP_ADD,
    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };
  VkPipelineColorBlendStateCreateInfo blend = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
    .attachmentCount = 1,
    .pAttachments = &blend_attachment,
  };
  VkStencilOpState stencil_keep = {
    .failOp = VK_STENCIL_OP_KEEP,
    .passOp = VK_STENCIL_OP_KEEP,
    .depthFailOp = VK_STENCIL_OP_KEEP,
    .compareOp = VK_COMPARE_OP_EQUAL,
    .compareMask = 0xff,
    .writeMask = 0xff,
    .reference = 0,
  };
  VkPipelineDepthStencilStateCreateInfo depth_stencil = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    .depthTestEnable = VK_FALSE,
    .depthWriteEnable = VK_FALSE,
    .depthCompareOp = extras ? extras->depth_compare_op : VK_COMPARE_OP_LESS,
    .depthBoundsTestEnable = VK_FALSE,
    .stencilTestEnable = VK_FALSE,
    .front = stencil_keep,
    .back = stencil_keep,
  };
  VkPipelineMultisampleStateCreateInfo multisample = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };
  VkPipelineViewportStateCreateInfo viewport = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
    .viewportCount = 1,
    .scissorCount = 1,
  };
  /* viewport, scissor, optional stencil×3, optional blend constants */
  VkDynamicState dyn_states[6];
  uint32_t n_dyn = 0;
  gboolean want_blend_const = extras_need_blend_constants (extras);
  gboolean want_stencil_dyn = FALSE;
  VkPipelineDynamicStateCreateInfo dynamic = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
  };
  VkPipelineVertexInputStateCreateInfo vertex = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
  };
  VkGraphicsPipelineCreateInfo pinfo = {
    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
    .layout = layout,
    .renderPass = rp,
    .subpass = 0,
    .stageCount = 2,
    .pStages = stages,
    .pInputAssemblyState = &assembly,
    .pRasterizationState = &rasterization,
    .pColorBlendState = &blend,
    .pDepthStencilState = &depth_stencil,
    .pMultisampleState = &multisample,
    .pViewportState = &viewport,
    .pDynamicState = &dynamic,
    .pVertexInputState = &vertex,
  };
  VkResult res;

  dyn_states[n_dyn++] = VK_DYNAMIC_STATE_VIEWPORT;
  dyn_states[n_dyn++] = VK_DYNAMIC_STATE_SCISSOR;

  if (blend_enable && extras)
    {
      blend_attachment.srcColorBlendFactor = extras->src_color;
      blend_attachment.dstColorBlendFactor = extras->dst_color;
      blend_attachment.colorBlendOp = extras->color_op;
      blend_attachment.srcAlphaBlendFactor = extras->src_alpha;
      blend_attachment.dstAlphaBlendFactor = extras->dst_alpha;
      blend_attachment.alphaBlendOp = extras->alpha_op;
    }

  if (extras)
    {
      depth_stencil.depthTestEnable =
        extras->depth_test_enable ? VK_TRUE : VK_FALSE;
      depth_stencil.depthWriteEnable =
        extras->depth_write_enable ? VK_TRUE : VK_FALSE;
      /* Compare op is set in the initializer; repeat it so a later
       * stencil rewrite of this struct cannot leave NEVER as 0-by-accident
       * if the initializer was skipped.
       */
      depth_stencil.depthCompareOp = extras->depth_compare_op;

      if (extras->disable_color_writes)
        blend_attachment.colorWriteMask = 0;

      if (extras->stencil_equal_keep ||
          extras->stencil_op_replace ||
          extras->stencil_op_incr ||
          extras->stencil_op_decr)
        {
          depth_stencil.stencilTestEnable = VK_TRUE;
          want_stencil_dyn = TRUE;

          if (extras->stencil_op_replace)
            {
              /* GL: ALWAYS + REPLACE (sfail/zfail unused when always passes).
               */
              stencil_keep.failOp = VK_STENCIL_OP_KEEP;
              stencil_keep.passOp = VK_STENCIL_OP_REPLACE;
              stencil_keep.depthFailOp = VK_STENCIL_OP_KEEP;
              stencil_keep.compareOp = VK_COMPARE_OP_ALWAYS;
            }
          else if (extras->stencil_op_incr)
            {
              /* Cover pixels: ALWAYS + INCR (works for rect + region merge). */
              stencil_keep.failOp = VK_STENCIL_OP_KEEP;
              stencil_keep.passOp = VK_STENCIL_OP_INCREMENT_AND_CLAMP;
              stencil_keep.depthFailOp = VK_STENCIL_OP_KEEP;
              stencil_keep.compareOp = VK_COMPARE_OP_ALWAYS;
            }
          else if (extras->stencil_op_decr)
            {
              stencil_keep.failOp = VK_STENCIL_OP_KEEP;
              stencil_keep.passOp = VK_STENCIL_OP_DECREMENT_AND_CLAMP;
              stencil_keep.depthFailOp = VK_STENCIL_OP_KEEP;
              stencil_keep.compareOp = VK_COMPARE_OP_ALWAYS;
            }
          else
            {
              /* Color draws under clip: EQUAL + KEEP; masks set dynamically. */
              stencil_keep.failOp = VK_STENCIL_OP_KEEP;
              stencil_keep.passOp = VK_STENCIL_OP_KEEP;
              stencil_keep.depthFailOp = VK_STENCIL_OP_KEEP;
              stencil_keep.compareOp = VK_COMPARE_OP_EQUAL;
            }

          depth_stencil.front = stencil_keep;
          depth_stencil.back = stencil_keep;
        }

      if (extras->samples > VK_SAMPLE_COUNT_1_BIT)
        multisample.rasterizationSamples = extras->samples;
      if (extras->has_topology)
        assembly.topology = extras->topology;
      if (extras->vertex_input)
        pinfo.pVertexInputState = extras->vertex_input;
    }

  if (want_stencil_dyn)
    {
      dyn_states[n_dyn++] = VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK;
      dyn_states[n_dyn++] = VK_DYNAMIC_STATE_STENCIL_WRITE_MASK;
      dyn_states[n_dyn++] = VK_DYNAMIC_STATE_STENCIL_REFERENCE;
    }
  if (want_blend_const)
    dyn_states[n_dyn++] = VK_DYNAMIC_STATE_BLEND_CONSTANTS;

  dynamic.dynamicStateCount = n_dyn;
  dynamic.pDynamicStates = dyn_states;

  res = vkCreateGraphicsPipelines (dev, cache->device->pipeline_cache, 1, &pinfo,
                                   NULL, out_pipe);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkCreateGraphicsPipelines failed (%d)", (int) res);
      return FALSE;
    }
  return TRUE;
}

void
cogl_vk_tex_pipe_extras_from_pipeline (CoglPipeline        *pipeline,
                                       VkFrontFace          front_face,
                                       CoglVkTexPipeExtras *out)
{
  CoglPipeline *blend_auth;
  CoglPipeline *cull_auth;
  CoglPipeline *depth_auth;

  g_return_if_fail (out != NULL);
  memset (out, 0, sizeof (*out));
  out->src_color = VK_BLEND_FACTOR_ONE;
  out->dst_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  out->color_op = VK_BLEND_OP_ADD;
  out->src_alpha = VK_BLEND_FACTOR_ONE;
  out->dst_alpha = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  out->alpha_op = VK_BLEND_OP_ADD;
  out->cull_mode = VK_CULL_MODE_NONE;
  out->front_face = front_face;
  out->depth_compare_op = VK_COMPARE_OP_LESS;

  if (!pipeline)
    return;

  blend_auth = _cogl_pipeline_get_authority (pipeline, COGL_PIPELINE_STATE_BLEND);
  if (blend_auth && blend_auth->has_big_state && blend_auth->big_state)
    {
      const CoglPipelineBlendState *bs = &blend_auth->big_state->blend_state;

      out->src_color = cogl_vk_map_gl_blend_factor (bs->blend_src_factor_rgb);
      out->dst_color = cogl_vk_map_gl_blend_factor (bs->blend_dst_factor_rgb);
      out->color_op = cogl_vk_map_gl_blend_op (bs->blend_equation_rgb);
      out->src_alpha = cogl_vk_map_gl_blend_factor (bs->blend_src_factor_alpha);
      out->dst_alpha = cogl_vk_map_gl_blend_factor (bs->blend_dst_factor_alpha);
      out->alpha_op = cogl_vk_map_gl_blend_op (bs->blend_equation_alpha);
      if (vk_blend_factor_uses_constant (out->src_color) ||
          vk_blend_factor_uses_constant (out->dst_color) ||
          vk_blend_factor_uses_constant (out->src_alpha) ||
          vk_blend_factor_uses_constant (out->dst_alpha))
        out->dynamic_blend_constants = TRUE;
    }

  cull_auth = _cogl_pipeline_get_authority (pipeline,
                                            COGL_PIPELINE_STATE_CULL_FACE);
  if (cull_auth && cull_auth->has_big_state && cull_auth->big_state)
    {
      const CoglPipelineCullFaceState *cull =
        &cull_auth->big_state->cull_face_state;
      gboolean y_flip = FALSE;
      CoglWinding winding = cull->front_winding;

      out->cull_mode = cogl_vk_map_cull_mode (cull->mode);

      /* Vulkan framebuffer y grows downward, so a positive viewport
       * height is the opposite of GL's bottom-left window. Do not copy
       * GL's y-flip glFrontFace invert; that culls the front rectangle
       * in backface-culling (draw 4, expected red, got black).
       */
      if (pipeline->context)
        {
          CoglFramebuffer *fb =
            cogl_context_get_current_draw_buffer (pipeline->context);

          if (fb)
            y_flip = cogl_framebuffer_is_y_flipped (fb);
        }

      if (winding == COGL_WINDING_CLOCKWISE)
        out->front_face = y_flip ? VK_FRONT_FACE_CLOCKWISE
                                 : VK_FRONT_FACE_COUNTER_CLOCKWISE;
      else
        out->front_face = y_flip ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                 : VK_FRONT_FACE_CLOCKWISE;

      cogl_vk_debug_log (
        "draw",
        "extras cull_mode=0x%x cull->mode=%d winding=%s y_flip=%d "
        "front_face=%s",
        (unsigned) out->cull_mode, (int) cull->mode,
        winding == COGL_WINDING_CLOCKWISE ? "CW" : "CCW",
        y_flip,
        out->front_face == VK_FRONT_FACE_CLOCKWISE ? "CW" : "CCW");
    }
  else
    {
      cogl_vk_debug_log (
        "draw",
        "extras NO cull_face authority (auth=%p big=%d) → cull=NONE",
        (void *) cull_auth,
        cull_auth ? cull_auth->has_big_state : 0);
    }

  depth_auth = _cogl_pipeline_get_authority (pipeline, COGL_PIPELINE_STATE_DEPTH);
  if (depth_auth && depth_auth->has_big_state && depth_auth->big_state)
    {
      const CoglDepthState *ds = &depth_auth->big_state->depth_state;

      out->depth_test_enable = ds->test_enabled;
      out->depth_write_enable = ds->write_enabled;
      out->depth_compare_op = cogl_vk_map_depth_func (ds->test_function);
    }

  /* GL updates the depth buffer when the test is disabled. Vulkan only
   * writes depth while the test is enabled, so "test off, write on,
   * function != default LESS" becomes ALWAYS. Default pipelines stay
   * test-off: leaving write=1 with test=0 produced empty draws on ANV.
   */
  if (!out->depth_test_enable &&
      out->depth_write_enable &&
      out->depth_compare_op != VK_COMPARE_OP_LESS)
    {
      out->depth_test_enable = TRUE;
      out->depth_compare_op = VK_COMPARE_OP_ALWAYS;
    }
  else if (!out->depth_test_enable)
    out->depth_write_enable = FALSE;

  /* GL flush_depth_state: depth write &= framebuffer depth write enable. */
  if (pipeline->context)
    {
      CoglDriver *drv = cogl_context_get_driver (pipeline->context);

      if (COGL_IS_DRIVER_VK (drv))
        {
          out->depth_write_enable =
            out->depth_write_enable &&
            COGL_DRIVER_VK (drv)->pending_depth_write;
        }
    }
}

VkPipeline
cogl_vk_pipeline_cache_get_hot_extras_pipe (CoglVkPipelineCache     *cache,
                                            CoglVkRenderFormatSetup *setup,
                                            CoglPipeline            *pipeline,
                                            uint8_t                  kind,
                                            unsigned int             ycbcr_index,
                                            GError                 **error)
{
  CoglVkHotExtrasKey key;
  CoglVkHotExtrasEntry *entry;
  CoglVkTexPipeExtras extras;
  CoglDriver *drv;
  VkFrontFace ff = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  gboolean blend_enable;
  gboolean opaque;
  VkShaderModule frag;
  VkPipelineLayout layout;
  VkPipeline pipe = VK_NULL_HANDLE;

  g_return_val_if_fail (cache != NULL && setup != NULL, VK_NULL_HANDLE);
  g_return_val_if_fail (pipeline != NULL, VK_NULL_HANDLE);

  opaque = (kind == COGL_VK_HOT_KIND_TEX_OPAQUE ||
            kind == COGL_VK_HOT_KIND_YCBCR_OPAQUE);
  blend_enable = !opaque;

  drv = cogl_context_get_driver (pipeline->context);
  if (drv && COGL_IS_DRIVER_VK (drv))
    ff = COGL_DRIVER_VK (drv)->pending_front_face;

  cogl_vk_tex_pipe_extras_from_pipeline (pipeline, ff, &extras);
  if (setup->ds_format != VK_FORMAT_UNDEFINED)
    extras.stencil_equal_keep = TRUE;
  if (setup->samples > VK_SAMPLE_COUNT_1_BIT)
    extras.samples = setup->samples;

  memset (&key, 0, sizeof (key));
  key.format = setup->format;
  key.ds_format = setup->ds_format;
  key.samples = setup->samples ? setup->samples : VK_SAMPLE_COUNT_1_BIT;
  key.kind = kind;
  key.ycbcr_index = ycbcr_index;
  cogl_vk_pipe_state_key_from_extras (&key.state, &extras, blend_enable, 0);

  if (!cache->hot_extras_pipes)
    {
      cache->hot_extras_pipes =
        g_hash_table_new_full (hot_extras_key_hash, hot_extras_key_equal,
                               NULL, g_free);
    }

  entry = g_hash_table_lookup (cache->hot_extras_pipes, &key);
  if (entry)
    return entry->pipeline;

  switch (kind)
    {
    case COGL_VK_HOT_KIND_QUAD:
      frag = cache->quad_frag_module;
      layout = cache->pipe_layout;
      break;
    case COGL_VK_HOT_KIND_YCBCR:
    case COGL_VK_HOT_KIND_YCBCR_OPAQUE:
      frag = cache->tex_frag_module;
      layout = cache->pipe_layout_ycbcr[ycbcr_index];
      break;
    case COGL_VK_HOT_KIND_TEX:
    case COGL_VK_HOT_KIND_TEX_OPAQUE:
    default:
      frag = cache->tex_frag_module;
      layout = cache->pipe_layout;
      break;
    }

  if (layout == VK_NULL_HANDLE || frag == VK_NULL_HANDLE ||
      cache->vert_module == VK_NULL_HANDLE)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "hot extras pipeline modules unavailable");
      return VK_NULL_HANDLE;
    }

  cogl_vk_debug_log (
    "pipe",
    "hot_extras kind=%u cull=0x%x front_face=%s depth_test=%d "
    "(extras, not pending-only)",
    (unsigned) kind,
    (unsigned) extras.cull_mode,
    extras.front_face == VK_FRONT_FACE_CLOCKWISE ? "CW" : "CCW",
    extras.depth_test_enable ? 1 : 0);

  if (!cogl_vk_create_tex_pipeline (cache, setup->render_pass,
                                    cache->vert_module, frag, layout,
                                    blend_enable, &extras, &pipe, error))
    return VK_NULL_HANDLE;

  entry = g_new0 (CoglVkHotExtrasEntry, 1);
  entry->key = key;
  entry->pipeline = pipe;
  g_hash_table_insert (cache->hot_extras_pipes, &entry->key, entry);
  return pipe;
}

void
cogl_vk_pass_apply_blend_constants (CoglVkPass   *pass,
                                    CoglPipeline *pipeline)
{
  float constants[4] = { 0.f, 0.f, 0.f, 0.f };

  g_return_if_fail (pass != NULL && pass->command_buffer != VK_NULL_HANDLE);

  if (pipeline)
    {
      CoglPipeline *blend_auth =
        _cogl_pipeline_get_authority (pipeline, COGL_PIPELINE_STATE_BLEND);

      if (blend_auth && blend_auth->has_big_state && blend_auth->big_state)
        {
          const CoglColor *bc =
            &blend_auth->big_state->blend_state.blend_constant;

          constants[0] = cogl_color_get_red (bc);
          constants[1] = cogl_color_get_green (bc);
          constants[2] = cogl_color_get_blue (bc);
          constants[3] = cogl_color_get_alpha (bc);
        }
    }

  vkCmdSetBlendConstants (pass->command_buffer, constants);
}

void
cogl_vk_pass_apply_depth_range (CoglVkPass   *pass,
                                CoglPipeline *pipeline)
{
  CoglPipeline *depth_auth;
  const CoglDepthState *ds;

  if (!pass || !pass->command_buffer || !pass->in_render_pass || !pipeline)
    return;

  depth_auth = _cogl_pipeline_get_authority (pipeline, COGL_PIPELINE_STATE_DEPTH);
  if (!depth_auth || !depth_auth->has_big_state || !depth_auth->big_state)
    return;

  ds = &depth_auth->big_state->depth_state;

  if (!pass->viewport_valid)
    return;

  pass->viewport.minDepth = ds->range_near;
  pass->viewport.maxDepth = ds->range_far;
  vkCmdSetViewport (pass->command_buffer, 0, 1, &pass->viewport);

  cogl_vk_debug_log (
    "draw",
    "depth_range test=%d near=%g far=%g viewport min=%g max=%g",
    ds->test_enabled ? 1 : 0,
    ds->range_near, ds->range_far,
    pass->viewport.minDepth, pass->viewport.maxDepth);
}

typedef struct
{
  VkDescriptorPool pool;
  VkSemaphore timeline;
  uint64_t busy_point;
  gboolean open;
  gboolean spare;
  gboolean pending_recording;
  unsigned int texture_owners;
  gboolean persistent;
} CoglVkDsPoolSlot;

typedef struct
{
  VkDescriptorPool pool;
  VkDescriptorSet set;
  VkSemaphore timeline;
  uint64_t point;
  gboolean pending_recording;
} CoglVkRetiredDescriptor;

static void
cogl_vk_pipeline_reclaim_texture_sets (CoglVkPipelineCache *cache)
{
  if (!cache->ds_retired || !cache->device->vkGetSemaphoreCounterValue)
    return;

  for (unsigned int i = 0; i < cache->ds_retired->len;)
    {
      CoglVkRetiredDescriptor *retired =
        &g_array_index (cache->ds_retired, CoglVkRetiredDescriptor, i);
      uint64_t completed = 0;
      VkResult res;

      if (retired->pending_recording ||
          (retired->timeline != VK_NULL_HANDLE &&
           (cache->device->vkGetSemaphoreCounterValue (cache->device->device,
                                                       retired->timeline,
                                                       &completed) != VK_SUCCESS ||
            completed < retired->point)))
        {
          i++;
          continue;
        }
      res = vkFreeDescriptorSets (cache->device->device,
                                  retired->pool, 1, &retired->set);
      if (res != VK_SUCCESS)
        {
          cogl_vk_device_note_vk_result (cache->device, res);
          i++;
          continue;
        }
      g_array_remove_index_fast (cache->ds_retired, i);
    }
}

static gboolean
cogl_vk_create_descriptor_pool (CoglVkDevice     *device,
                                VkDescriptorPool *out_pool)
{
  VkDescriptorPoolSize pool_sizes[2] = {
    {
      .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 256 + (max_layers_for_limits (&device->limits) * 2048),
    },
    {
      .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .descriptorCount = 2048,
    },
  };
  VkDescriptorPoolCreateInfo dpool_info = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
    .maxSets = 2304,
    .poolSizeCount = 2,
    .pPoolSizes = pool_sizes,
  };

  return vkCreateDescriptorPool (device->device, &dpool_info, NULL,
                                 out_pool) == VK_SUCCESS;
}

static void
cogl_vk_pipeline_cache_forget_recycled_sets (CoglVkPipelineCache *cache)
{
  if (cache->ds_free)
    g_array_set_size (cache->ds_free, 0);
  if (cache->ds_free_ycbcr)
    g_array_set_size (cache->ds_free_ycbcr, 0);
  if (cache->ds_free_cold)
    g_array_set_size (cache->ds_free_cold, 0);
  /* Persistent sets (including dummy_white_ds) pin their pool. */
}

static void
cogl_vk_pipeline_cache_reclaim_pools (CoglVkPipelineCache *cache)
{
  unsigned int i;
  gboolean reset_any = FALSE;

  if (!cache->ds_pool_slots || !cache->device ||
      !cache->device->vkGetSemaphoreCounterValue)
    return;

  cogl_vk_pipeline_reclaim_texture_sets (cache);

  for (i = 0; i < cache->ds_pool_slots->len; i++)
    {
      CoglVkDsPoolSlot *slot = g_ptr_array_index (cache->ds_pool_slots, i);
      uint64_t value = 0;
      VkResult res;

      if (slot->persistent || slot->open || slot->spare || slot->pending_recording ||
          slot->texture_owners > 0 || slot->timeline == VK_NULL_HANDLE)
        continue;

      res = cache->device->vkGetSemaphoreCounterValue (cache->device->device,
                                                       slot->timeline,
                                                       &value);
      if (res != VK_SUCCESS || value < slot->busy_point)
        continue;

      res = vkResetDescriptorPool (cache->device->device, slot->pool, 0);
      if (res != VK_SUCCESS)
        {
          cogl_vk_device_note_vk_result (cache->device, res);
          continue;
        }
      slot->timeline = VK_NULL_HANDLE;
      slot->busy_point = 0;
      slot->spare = TRUE;
      reset_any = TRUE;
    }

  if (reset_any)
    cogl_vk_pipeline_cache_forget_recycled_sets (cache);
}

static gboolean
cogl_vk_pipeline_cache_grow_pool (CoglVkPipelineCache *cache,
                                  gboolean             persistent)
{
  CoglVkDsPoolSlot *slot;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  unsigned int i;

  if (cache->ds_pool_slots)
    {
      for (i = 0; i < cache->ds_pool_slots->len; i++)
        {
          CoglVkDsPoolSlot *old = g_ptr_array_index (cache->ds_pool_slots, i);

          if (old->open && old->persistent == persistent)
            old->open = FALSE;
        }
    }

  cogl_vk_pipeline_cache_reclaim_pools (cache);

  if (cache->ds_pool_slots)
    {
      for (i = 0; i < cache->ds_pool_slots->len; i++)
        {
          CoglVkDsPoolSlot *old = g_ptr_array_index (cache->ds_pool_slots, i);

          if (!old->spare || old->persistent != persistent)
            continue;
          old->spare = FALSE;
          old->open = TRUE;
          if (persistent)
            cache->texture_descriptor_pool = old->pool;
          else
            cache->descriptor_pool = old->pool;
          cogl_vk_debug_log ("draw",
                             "descriptor pool reuse slot=%u", i);
          return TRUE;
        }
    }

  /* Completed transient pools are reused. A single recording may require
   * more than eight pools; live textures must not pin those pools.
   */
  if (!cogl_vk_create_descriptor_pool (cache->device, &pool))
    return FALSE;

  slot = g_new0 (CoglVkDsPoolSlot, 1);
  slot->pool = pool;
  slot->open = TRUE;
  slot->persistent = persistent;
  if (!cache->ds_pool_slots)
    cache->ds_pool_slots = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (cache->ds_pool_slots, slot);
  if (persistent)
    cache->texture_descriptor_pool = pool;
  else
    cache->descriptor_pool = pool;
  cogl_vk_debug_log ("draw",
                     "descriptor pool grow n=%u",
                     cache->ds_pool_slots->len);
  return TRUE;
}

static gboolean
cogl_vk_pipeline_alloc_from_pool (CoglVkPipelineCache         *cache,
                                  VkDescriptorSetAllocateInfo *info,
                                  VkDescriptorSet             *out_ds,
                                  gboolean                     persistent,
                                  const char                  *what,
                                  GError                     **error)
{
  VkResult res;

  if (persistent)
    cogl_vk_pipeline_reclaim_texture_sets (cache);
  if (persistent && cache->texture_descriptor_pool == VK_NULL_HANDLE &&
      !cogl_vk_pipeline_cache_grow_pool (cache, TRUE))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Failed to create texture descriptor pool");
      return FALSE;
    }
  info->descriptorPool = persistent ? cache->texture_descriptor_pool
                                    : cache->descriptor_pool;
  res = vkAllocateDescriptorSets (cache->device->device, info, out_ds);
  if (persistent && (res == VK_ERROR_OUT_OF_POOL_MEMORY ||
                     res == VK_ERROR_FRAGMENTED_POOL))
    {
      /* Older texture pools can have individually retired sets available. */
      for (unsigned int i = 0; i < cache->ds_pool_slots->len; i++)
        {
          CoglVkDsPoolSlot *slot = g_ptr_array_index (cache->ds_pool_slots, i);

          if (!slot->persistent || slot->pool == cache->texture_descriptor_pool)
            continue;
          info->descriptorPool = slot->pool;
          res = vkAllocateDescriptorSets (cache->device->device, info, out_ds);
          if (res != VK_ERROR_OUT_OF_POOL_MEMORY && res != VK_ERROR_FRAGMENTED_POOL)
            break;
        }
    }
  if (res == VK_ERROR_OUT_OF_POOL_MEMORY ||
      res == VK_ERROR_FRAGMENTED_POOL)
    {
      if (!cogl_vk_pipeline_cache_grow_pool (cache, persistent))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "%s (%d)", what, (int) res);
          return FALSE;
        }
      info->descriptorPool = persistent ? cache->texture_descriptor_pool
                                        : cache->descriptor_pool;
      res = vkAllocateDescriptorSets (cache->device->device, info, out_ds);
    }

  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "%s (%d)", what, (int) res);
      return FALSE;
    }
  for (unsigned int i = 0; i < cache->ds_pool_slots->len; i++)
    {
      CoglVkDsPoolSlot *slot = g_ptr_array_index (cache->ds_pool_slots, i);

      if (slot->pool == info->descriptorPool)
        {
          slot->pending_recording = TRUE;
          break;
        }
    }
  return TRUE;
}

void
cogl_vk_pipeline_cache_note_submit (CoglVkPipelineCache *cache,
                                    VkSemaphore          timeline,
                                    uint64_t             point)
{
  unsigned int i;

  if (!cache || !cache->ds_pool_slots ||
      timeline == VK_NULL_HANDLE || point == 0)
    return;

  for (i = 0; i < cache->ds_pool_slots->len; i++)
    {
      CoglVkDsPoolSlot *slot = g_ptr_array_index (cache->ds_pool_slots, i);

      /* Still referenced by the command buffer just submitted. A later
       * point on this queue implies every earlier submit finished.
       */
      if (!slot->spare && (slot->persistent || slot->pending_recording))
        {
          slot->timeline = timeline;
          slot->busy_point = point;
          slot->pending_recording = FALSE;
        }
    }
  if (cache->ds_retired)
    for (i = 0; i < cache->ds_retired->len; i++)
      {
        CoglVkRetiredDescriptor *retired =
          &g_array_index (cache->ds_retired, CoglVkRetiredDescriptor, i);
        if (retired->pending_recording)
          {
            retired->timeline = timeline;
            retired->point = point;
            retired->pending_recording = FALSE;
          }
      }
}

void
cogl_vk_pipeline_cache_discard_recording (CoglVkPipelineCache *cache)
{
  if (!cache)
    return;
  if (cache->ds_pool_slots)
    for (unsigned int i = 0; i < cache->ds_pool_slots->len; i++)
      {
        CoglVkDsPoolSlot *slot = g_ptr_array_index (cache->ds_pool_slots, i);
        if (slot->persistent)
          {
            slot->pending_recording = FALSE;
            continue;
          }
        if (vkResetDescriptorPool (cache->device->device, slot->pool, 0) != VK_SUCCESS)
          continue;
        slot->pending_recording = FALSE;
        slot->timeline = VK_NULL_HANDLE;
        slot->busy_point = 0;
        slot->spare = !slot->open;
      }
  cogl_vk_pipeline_cache_forget_recycled_sets (cache);
  if (cache->ds_retired)
    for (unsigned int i = 0; i < cache->ds_retired->len; i++)
      {
        CoglVkRetiredDescriptor *retired =
          &g_array_index (cache->ds_retired, CoglVkRetiredDescriptor, i);
        retired->pending_recording = FALSE;
        retired->timeline = VK_NULL_HANDLE;
        retired->point = 0;
      }
  cogl_vk_pipeline_reclaim_texture_sets (cache);
}

static int
max_layers_for_limits (const VkPhysicalDeviceLimits *limits)
{
  uint32_t n = MIN (limits->maxPerStageDescriptorSamplers,
                    limits->maxPerStageDescriptorSampledImages);

  if (limits->maxPerStageResources < 2 ||
      limits->maxVertexOutputComponents < 8 ||
      limits->maxFragmentInputComponents < 8 ||
      limits->maxUniformBufferRange < 80)
    return 0;
  n = MIN (n, limits->maxDescriptorSetSamplers);
  n = MIN (n, limits->maxDescriptorSetSampledImages);
  n = MIN (n, limits->maxPerStageResources - 1); /* snippet UBO */
  /* One vec4 varying per layer, plus the vertex color. */
  n = MIN (n, (MIN (limits->maxVertexOutputComponents,
                    limits->maxFragmentInputComponents) - 4) / 4);
  n = MIN (n, (MIN (limits->maxUniformBufferRange,
                    COGL_VK_UBO_SLICE) - 16) / 64);
  return n;
}

int
cogl_vk_pipeline_get_max_layers (CoglPipeline *pipeline)
{
  CoglDriver *driver = cogl_context_get_driver (pipeline->context);
  CoglVkDevice *device = cogl_driver_vk_peek_device (COGL_DRIVER_VK (driver));

  return max_layers_for_limits (&device->limits);
}

CoglVkPipelineCache *
cogl_vk_pipeline_cache_new (CoglVkDevice *device,
                            GError      **error)
{
  CoglVkPipelineCache *cache;
  VkSamplerCreateInfo sampler_info = {
    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
    .magFilter = VK_FILTER_LINEAR,
    .minFilter = VK_FILTER_LINEAR,
    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .maxAnisotropy = 1.f,
    .maxLod = 0.25f,
  };
  VkDescriptorSetLayoutBinding ds_binding = {
    .binding = 0,
    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    .descriptorCount = 1,
    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
  };
  VkDescriptorSetLayoutBinding cold_bindings[2];
  VkDescriptorSetLayoutCreateInfo ds_info = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .bindingCount = 1,
    .pBindings = &ds_binding,
  };
  VkDescriptorSetLayoutCreateInfo ds_cold_info = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .bindingCount = 2,
    .pBindings = cold_bindings,
  };
  VkPushConstantRange pc_ranges[2] = {
    {
      .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
      .offset = 0,
      .size = sizeof (CoglVkVertPush),
    },
    {
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      .offset = sizeof (CoglVkVertPush),
      .size = sizeof (float) * 8,
    },
  };
  VkPipelineLayoutCreateInfo pl_info = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .setLayoutCount = 1,
    .pushConstantRangeCount = 2,
    .pPushConstantRanges = pc_ranges,
  };
  /* Hot DS: 1 sampler each; cold DS: N samplers + 1 UBO each. */
  VkDescriptorPoolSize pool_sizes[2] = {
    {
      .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 256 + (max_layers_for_limits (&device->limits) * 2048),
    },
    {
      .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .descriptorCount = 2048,
    },
  };
  VkDescriptorPoolCreateInfo dpool_info = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
    .maxSets = 2304,
    .poolSizeCount = 2,
    .pPoolSizes = pool_sizes,
  };
  VkBufferCreateInfo ubo_info = {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size = 4 * 1024 * 1024,
    .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  VkMemoryRequirements ubo_reqs;
  VkMemoryAllocateInfo ubo_alloc = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
  };
  VkPhysicalDeviceMemoryProperties mem_props;
  int ubo_mem_type = -1;
  uint32_t mi;
  VkResult res;

  g_return_val_if_fail (device != NULL, NULL);

  cache = g_new0 (CoglVkPipelineCache, 1);
  cache->device = device;
  cache->ds_free = g_array_new (FALSE, FALSE, sizeof (VkDescriptorSet));
  cache->ds_free_ycbcr = g_array_new (FALSE, FALSE, sizeof (VkDescriptorSet));
  cache->ds_free_cold = g_array_new (FALSE, FALSE, sizeof (VkDescriptorSet));
  cache->uniform_size = 4 * 1024 * 1024;

  res = vkCreateSampler (device->device, &sampler_info, NULL, &cache->sampler);
  if (res != VK_SUCCESS)
    goto fail_sampler;

  /* Non-immutable combined image sampler so update_texture_ds can
   * swap per-layer wrap/filter. YCbCr keeps immutable below.
   */
  ds_binding.pImmutableSamplers = NULL;
  /* Cached texture sets need an allocatable layout even when hot draws use
   * push descriptors. Vulkan forbids allocating sets from a push layout.
   */
  res = vkCreateDescriptorSetLayout (device->device, &ds_info, NULL,
                                     &cache->ds_layout);
  if (res != VK_SUCCESS)
    goto fail_ds;
  if (device->has_push_descriptor)
    {
      ds_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
      res = vkCreateDescriptorSetLayout (device->device, &ds_info, NULL,
                                         &cache->ds_layout_push);
      if (res != VK_SUCCESS)
        goto fail_ds;
    }

  /* Array of N combined-image-samplers at binding 0; UBO at binding 1.
   * UBO MUST be VERTEX|FRAGMENT: VERT_VBO assemble with vertex snippets
   * multiplies gl_Position by `_cogl_flip_vector` from this UBO. FRAGMENT-
   * only stageFlags → vertex UBO access is invalid → device lost / exit 245
   * inside vkQueueWaitIdle (custom-attributes log: WaitIdle never "done").
   */
  cold_bindings[0] = (VkDescriptorSetLayoutBinding) {
    .binding = 0,
    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    .descriptorCount = max_layers_for_limits (&device->limits),
    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    .pImmutableSamplers = NULL,
  };
  cold_bindings[1] = (VkDescriptorSetLayoutBinding) {
    .binding = 1,
    .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
    .descriptorCount = 1,
    .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
  };
  res = vkCreateDescriptorSetLayout (device->device, &ds_cold_info, NULL,
                                     &cache->ds_layout_cold);
  if (res != VK_SUCCESS)
    goto fail_ds_cold;

  pl_info.pSetLayouts = device->has_push_descriptor
    ? &cache->ds_layout_push : &cache->ds_layout;
  res = vkCreatePipelineLayout (device->device, &pl_info, NULL,
                                &cache->pipe_layout);
  if (res != VK_SUCCESS)
    goto fail_pl;

  pl_info.pSetLayouts = &cache->ds_layout_cold;
  res = vkCreatePipelineLayout (device->device, &pl_info, NULL,
                                &cache->pipe_layout_cold);
  if (res != VK_SUCCESS)
    goto fail_pl_cold;

  if (!create_shader_module (device->device, common_vert_data,
                             sizeof (common_vert_data),
                             &cache->vert_module, error))
    goto fail_pl_cold;
  if (!create_shader_module (device->device, texture_frag_data,
                             sizeof (texture_frag_data),
                             &cache->tex_frag_module, error))
    goto fail_vert;
  if (!create_shader_module (device->device, quad_frag_data,
                             sizeof (quad_frag_data),
                             &cache->quad_frag_module, error))
    goto fail_tex;

  res = vkCreateDescriptorPool (device->device, &dpool_info, NULL,
                                &cache->descriptor_pool);
  if (res != VK_SUCCESS)
    goto fail_quad;

  /* Host-visible UBO for cold-path snippet uniforms (color-mgmt matrices). */
  res = vkCreateBuffer (device->device, &ubo_info, NULL, &cache->uniform_buffer);
  if (res != VK_SUCCESS)
    goto fail_pool;
  vkGetBufferMemoryRequirements (device->device, cache->uniform_buffer, &ubo_reqs);
  vkGetPhysicalDeviceMemoryProperties (device->physical_device, &mem_props);
  for (mi = 0; mi < mem_props.memoryTypeCount; mi++)
    {
      if ((ubo_reqs.memoryTypeBits & (1u << mi)) &&
          (mem_props.memoryTypes[mi].propertyFlags &
           (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
          (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        {
          ubo_mem_type = (int) mi;
          break;
        }
    }
  if (ubo_mem_type < 0)
    goto fail_ubo_buf;
  ubo_alloc.allocationSize = ubo_reqs.size;
  ubo_alloc.memoryTypeIndex = (uint32_t) ubo_mem_type;
  res = vkAllocateMemory (device->device, &ubo_alloc, NULL, &cache->uniform_memory);
  if (res != VK_SUCCESS)
    goto fail_ubo_buf;
  res = vkBindBufferMemory (device->device, cache->uniform_buffer,
                            cache->uniform_memory, 0);
  if (res != VK_SUCCESS)
    goto fail_ubo_mem;
  res = vkMapMemory (device->device, cache->uniform_memory, 0, cache->uniform_size,
                     0, &cache->uniform_mapped);
  if (res != VK_SUCCESS)
    goto fail_ubo_mem;
  memset (cache->uniform_mapped, 0, cache->uniform_size);

  /* Immutable conversions are keyed by matrix and range, never shared
   * between textures with different color interpretations.
   */
  for (unsigned int i = 0; device->has_sampler_ycbcr && i < COGL_VK_YCBCR_VARIANTS; i++)
    {
      static const VkSamplerYcbcrModelConversion models[] = {
        VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
        VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601,
        VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020,
      };
      VkFormatProperties nv12_props;
      gboolean cosited;
      VkSamplerYcbcrConversionCreateInfo ycbcr_ci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
        .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        .ycbcrModel = models[i / 2],
        .ycbcrRange = i % 2 ? VK_SAMPLER_YCBCR_RANGE_ITU_FULL :
                      VK_SAMPLER_YCBCR_RANGE_ITU_NARROW,
        .components = {
          .r = VK_COMPONENT_SWIZZLE_IDENTITY,
          .g = VK_COMPONENT_SWIZZLE_IDENTITY,
          .b = VK_COMPONENT_SWIZZLE_IDENTITY,
          .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .chromaFilter = VK_FILTER_LINEAR,
        .forceExplicitReconstruction = VK_FALSE,
      };
      VkSamplerYcbcrConversionInfo ycbcr_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
      };
      VkSamplerCreateInfo ycbcr_sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
      };
      VkDescriptorSetLayoutBinding ycbcr_binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      };
      VkDescriptorSetLayoutCreateInfo ycbcr_ds_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &ycbcr_binding,
      };
      VkPipelineLayoutCreateInfo ycbcr_pl_info = pl_info;

      vkGetPhysicalDeviceFormatProperties (device->physical_device,
                                           VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
                                           &nv12_props);
      if (!(nv12_props.optimalTilingFeatures &
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT))
        {
          ycbcr_ci.chromaFilter = VK_FILTER_NEAREST;
          ycbcr_sampler_info.magFilter = VK_FILTER_NEAREST;
          ycbcr_sampler_info.minFilter = VK_FILTER_NEAREST;
        }
      cosited = !!(nv12_props.optimalTilingFeatures &
                   VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT);
      ycbcr_ci.xChromaOffset = cosited
        ? VK_CHROMA_LOCATION_COSITED_EVEN
        : VK_CHROMA_LOCATION_MIDPOINT;
      ycbcr_ci.yChromaOffset = ycbcr_ci.xChromaOffset;

      res = vkCreateSamplerYcbcrConversion (device->device, &ycbcr_ci, NULL,
                                            &cache->ycbcr_conversion[i]);
      if (res != VK_SUCCESS)
        goto fail_ycbcr_conv;

      ycbcr_info.conversion = cache->ycbcr_conversion[i];
      ycbcr_sampler_info.pNext = &ycbcr_info;
      res = vkCreateSampler (device->device, &ycbcr_sampler_info, NULL,
                             &cache->ycbcr_sampler[i]);
      if (res != VK_SUCCESS)
        goto fail_ycbcr_conv;

      ycbcr_binding.pImmutableSamplers = &cache->ycbcr_sampler[i];
      res = vkCreateDescriptorSetLayout (device->device, &ycbcr_ds_info, NULL,
                                         &cache->ds_layout_ycbcr[i]);
      if (res != VK_SUCCESS)
        goto fail_ycbcr_sampler;

      ycbcr_pl_info.pSetLayouts = &cache->ds_layout_ycbcr[i];
      res = vkCreatePipelineLayout (device->device, &ycbcr_pl_info, NULL,
                                    &cache->pipe_layout_ycbcr[i]);
      if (res != VK_SUCCESS)
        goto fail_ycbcr_ds;
    }

  cache->spirv_cache = cogl_vk_spirv_cache_new (device);
  if (!cache->ds_pool_slots)
    cache->ds_pool_slots = g_ptr_array_new_with_free_func (g_free);
  if (cache->descriptor_pool != VK_NULL_HANDLE)
    {
      CoglVkDsPoolSlot *slot = g_new0 (CoglVkDsPoolSlot, 1);

      slot->pool = cache->descriptor_pool;
      slot->open = TRUE;
      g_ptr_array_add (cache->ds_pool_slots, slot);
    }

  return cache;

fail_ycbcr_ds:
fail_ycbcr_sampler:
fail_ycbcr_conv:
  for (unsigned int i = 0; i < COGL_VK_YCBCR_VARIANTS; i++)
    {
      if (cache->pipe_layout_ycbcr[i])
        vkDestroyPipelineLayout (device->device, cache->pipe_layout_ycbcr[i], NULL);
      if (cache->ds_layout_ycbcr[i])
        vkDestroyDescriptorSetLayout (device->device, cache->ds_layout_ycbcr[i], NULL);
      if (cache->ycbcr_sampler[i])
        vkDestroySampler (device->device, cache->ycbcr_sampler[i], NULL);
      if (cache->ycbcr_conversion[i])
        vkDestroySamplerYcbcrConversion (device->device,
                                         cache->ycbcr_conversion[i], NULL);
    }
  if (cache->uniform_mapped)
    {
      vkUnmapMemory (device->device, cache->uniform_memory);
      cache->uniform_mapped = NULL;
    }
fail_ubo_mem:
  if (cache->uniform_memory)
    vkFreeMemory (device->device, cache->uniform_memory, NULL);
fail_ubo_buf:
  if (cache->uniform_buffer)
    vkDestroyBuffer (device->device, cache->uniform_buffer, NULL);
fail_pool:
  if (cache->descriptor_pool)
    vkDestroyDescriptorPool (device->device, cache->descriptor_pool, NULL);
fail_quad:
  if (cache->quad_frag_module)
    vkDestroyShaderModule (device->device, cache->quad_frag_module, NULL);
fail_tex:
  if (cache->tex_frag_module)
    vkDestroyShaderModule (device->device, cache->tex_frag_module, NULL);
fail_vert:
  if (cache->vert_module)
    vkDestroyShaderModule (device->device, cache->vert_module, NULL);
fail_pl_cold:
  if (cache->pipe_layout_cold)
    vkDestroyPipelineLayout (device->device, cache->pipe_layout_cold, NULL);
fail_pl:
  if (cache->pipe_layout)
    vkDestroyPipelineLayout (device->device, cache->pipe_layout, NULL);
  if (cache->ds_layout_cold)
    vkDestroyDescriptorSetLayout (device->device, cache->ds_layout_cold, NULL);
fail_ds_cold:
fail_ds:
  if (cache->ds_layout_push)
    vkDestroyDescriptorSetLayout (device->device, cache->ds_layout_push, NULL);
  if (cache->ds_layout)
    vkDestroyDescriptorSetLayout (device->device, cache->ds_layout, NULL);
fail_sampler:
  if (cache->sampler)
    vkDestroySampler (device->device, cache->sampler, NULL);
  g_clear_pointer (&cache->ds_free, g_array_unref);
  g_clear_pointer (&cache->ds_free_ycbcr, g_array_unref);
  g_clear_pointer (&cache->ds_free_cold, g_array_unref);
  g_clear_pointer (&cache->ds_texture_owners, g_hash_table_unref);
  g_clear_pointer (&cache->ds_retired, g_array_unref);
  if (!error || !*error)
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "Cogl Vulkan pipeline cache init failed");
  g_free (cache);
  return NULL;
}

void
cogl_vk_pipeline_cache_free (CoglVkPipelineCache *cache)
{
  CoglVkRenderFormatSetup *setup, *next;
  VkDevice dev;

  if (!cache)
    return;

  dev = cache->device->device;
  for (setup = cache->format_setups; setup; setup = next)
    {
      next = setup->next;
      if (setup->tex_pipe)
        vkDestroyPipeline (dev, setup->tex_pipe, NULL);
      if (setup->tex_pipe_opaque)
        vkDestroyPipeline (dev, setup->tex_pipe_opaque, NULL);
      for (unsigned int i = 0; i < COGL_VK_YCBCR_VARIANTS; i++)
        {
          if (setup->tex_pipe_ycbcr[i])
            vkDestroyPipeline (dev, setup->tex_pipe_ycbcr[i], NULL);
          if (setup->tex_pipe_ycbcr_opaque[i])
            vkDestroyPipeline (dev, setup->tex_pipe_ycbcr_opaque[i], NULL);
        }
      if (setup->quad_pipe)
        vkDestroyPipeline (dev, setup->quad_pipe, NULL);
      if (setup->stencil_replace_pipe)
        vkDestroyPipeline (dev, setup->stencil_replace_pipe, NULL);
      if (setup->stencil_incr_pipe)
        vkDestroyPipeline (dev, setup->stencil_incr_pipe, NULL);
      if (setup->stencil_decr_pipe)
        vkDestroyPipeline (dev, setup->stencil_decr_pipe, NULL);
      if (setup->render_pass)
        vkDestroyRenderPass (dev, setup->render_pass, NULL);
      if (setup->render_pass_clear)
        vkDestroyRenderPass (dev, setup->render_pass_clear, NULL);
      if (setup->render_pass_clear_ds_only)
        vkDestroyRenderPass (dev, setup->render_pass_clear_ds_only, NULL);
      g_free (setup);
    }

  if (cache->dummy_white_ds != VK_NULL_HANDLE)
    {
      cogl_vk_pipeline_free_texture_ds (cache, FALSE, cache->dummy_white_ds);
      cache->dummy_white_ds = VK_NULL_HANDLE;
    }
  g_clear_pointer (&cache->ds_free, g_array_unref);
  g_clear_pointer (&cache->ds_free_ycbcr, g_array_unref);
  g_clear_pointer (&cache->ds_free_cold, g_array_unref);
  g_clear_pointer (&cache->ds_texture_owners, g_hash_table_unref);
  g_clear_pointer (&cache->ds_retired, g_array_unref);
  g_clear_pointer (&cache->spirv_cache, cogl_vk_spirv_cache_free);
  g_clear_pointer (&cache->dummy_white, cogl_vk_image_free);

  if (cache->hot_extras_pipes)
    {
      GHashTableIter iter;
      gpointer value;

      g_hash_table_iter_init (&iter, cache->hot_extras_pipes);
      while (g_hash_table_iter_next (&iter, NULL, &value))
        {
          CoglVkHotExtrasEntry *e = value;
          if (e->pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline (dev, e->pipeline, NULL);
        }
      g_hash_table_destroy (cache->hot_extras_pipes);
      cache->hot_extras_pipes = NULL;
    }

  if (cache->uniform_mapped)
    {
      vkUnmapMemory (dev, cache->uniform_memory);
      cache->uniform_mapped = NULL;
    }
  if (cache->uniform_buffer)
    vkDestroyBuffer (dev, cache->uniform_buffer, NULL);
  if (cache->uniform_memory)
    vkFreeMemory (dev, cache->uniform_memory, NULL);

  if (cache->ds_pool_slots)
    {
      unsigned int i;

      for (i = 0; i < cache->ds_pool_slots->len; i++)
        {
          CoglVkDsPoolSlot *slot = g_ptr_array_index (cache->ds_pool_slots, i);

          if (slot->pool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool (dev, slot->pool, NULL);
        }
      g_ptr_array_unref (cache->ds_pool_slots);
      cache->ds_pool_slots = NULL;
      cache->descriptor_pool = VK_NULL_HANDLE;
    }
  else if (cache->descriptor_pool)
    vkDestroyDescriptorPool (dev, cache->descriptor_pool, NULL);
  if (cache->quad_frag_module)
    vkDestroyShaderModule (dev, cache->quad_frag_module, NULL);
  if (cache->tex_frag_module)
    vkDestroyShaderModule (dev, cache->tex_frag_module, NULL);
  if (cache->vert_module)
    vkDestroyShaderModule (dev, cache->vert_module, NULL);
  for (unsigned int i = 0; i < COGL_VK_YCBCR_VARIANTS; i++)
    {
      if (cache->pipe_layout_ycbcr[i])
        vkDestroyPipelineLayout (dev, cache->pipe_layout_ycbcr[i], NULL);
      if (cache->ds_layout_ycbcr[i])
        vkDestroyDescriptorSetLayout (dev, cache->ds_layout_ycbcr[i], NULL);
      if (cache->ycbcr_sampler[i])
        vkDestroySampler (dev, cache->ycbcr_sampler[i], NULL);
      if (cache->ycbcr_conversion[i])
        vkDestroySamplerYcbcrConversion (dev, cache->ycbcr_conversion[i], NULL);
    }
  if (cache->pipe_layout_cold)
    vkDestroyPipelineLayout (dev, cache->pipe_layout_cold, NULL);
  if (cache->ds_layout_cold)
    vkDestroyDescriptorSetLayout (dev, cache->ds_layout_cold, NULL);
  if (cache->pipe_layout)
    vkDestroyPipelineLayout (dev, cache->pipe_layout, NULL);
  if (cache->ds_layout_push)
    vkDestroyDescriptorSetLayout (dev, cache->ds_layout_push, NULL);
  if (cache->ds_layout)
    vkDestroyDescriptorSetLayout (dev, cache->ds_layout, NULL);
  if (cache->sampler)
    vkDestroySampler (dev, cache->sampler, NULL);
  g_free (cache);
}

VkSamplerYcbcrConversion
cogl_vk_pipeline_cache_peek_ycbcr_conversion (CoglVkPipelineCache *cache,
                                              unsigned int         index)
{
  g_return_val_if_fail (cache != NULL, VK_NULL_HANDLE);
  g_return_val_if_fail (index < COGL_VK_YCBCR_VARIANTS, VK_NULL_HANDLE);
  return cache->ycbcr_conversion[index];
}

CoglVkRenderFormatSetup *
cogl_vk_pipeline_cache_get_format_setup (CoglVkPipelineCache  *cache,
                                         VkFormat              format,
                                         VkFormat              ds_format,
                                         VkSampleCountFlagBits samples,
                                         GError              **error)
{
  CoglVkRenderFormatSetup *setup;
  VkAttachmentDescription attachments[2];
  VkAttachmentReference color_ref = {0};
  VkAttachmentReference ds_ref = {0};
  VkSubpassDescription subpass = {0};
  VkSubpassDependency deps[2] = {0};
  VkRenderPassCreateInfo rp_info = {0};
  CoglVkTexPipeExtras ds_color = {0};
  CoglVkTexPipeExtras st_replace = {0};
  CoglVkTexPipeExtras st_incr = {0};
  CoglVkTexPipeExtras st_decr = {0};
  uint32_t n_attachments = 1;
  VkSampleCountFlagBits sample_count =
    (samples > VK_SAMPLE_COUNT_1_BIT) ? samples : VK_SAMPLE_COUNT_1_BIT;
  VkResult res;

  g_return_val_if_fail (cache != NULL, NULL);

  for (setup = cache->format_setups; setup; setup = setup->next)
    {
      if (setup->format == format &&
          setup->ds_format == ds_format &&
          setup->samples == sample_count)
        return setup;
    }

  setup = g_new0 (CoglVkRenderFormatSetup, 1);
  setup->format = format;
  setup->ds_format = ds_format;
  setup->samples = sample_count;

  memset (attachments, 0, sizeof (attachments));
  attachments[0].format = format;
  attachments[0].samples = sample_count;
  attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  color_ref.attachment = 0;
  color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &color_ref;

  deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  deps[0].dstSubpass = 0;
  deps[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT |
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  /* Include TRANSFER_READ: readback leaves color as TRANSFER_SRC before we
   * barrier back to COLOR_ATTACHMENT and LOAD prior contents.
   * SHADER_READ: a texture sampled in this pass may have been cleared
   * in the same command buffer (paint-fp16).
   */
  deps[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT |
                          VK_ACCESS_TRANSFER_READ_BIT |
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_SHADER_READ_BIT;

  deps[1].srcSubpass = 0;
  deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                         VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT |
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
  deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT |
                          VK_ACCESS_MEMORY_READ_BIT;

  if (ds_format != VK_FORMAT_UNDEFINED)
    {
      attachments[1].format = ds_format;
      attachments[1].samples = sample_count;
      attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachments[1].initialLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      attachments[1].finalLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

      ds_ref.attachment = 1;
      ds_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      subpass.pDepthStencilAttachment = &ds_ref;
      n_attachments = 2;

      ds_color.stencil_equal_keep = TRUE;
      ds_color.samples = sample_count;
      st_replace.disable_color_writes = TRUE;
      st_replace.stencil_op_replace = TRUE;
      st_replace.samples = sample_count;
      st_incr.disable_color_writes = TRUE;
      st_incr.stencil_op_incr = TRUE;
      st_incr.samples = sample_count;
      st_decr.disable_color_writes = TRUE;
      st_decr.stencil_op_decr = TRUE;
      st_decr.samples = sample_count;
    }

  if (sample_count > VK_SAMPLE_COUNT_1_BIT &&
      ds_format == VK_FORMAT_UNDEFINED)
    {
      ds_color.samples = sample_count;
    }

  rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rp_info.attachmentCount = n_attachments;
  rp_info.pAttachments = attachments;
  rp_info.subpassCount = 1;
  rp_info.pSubpasses = &subpass;
  rp_info.dependencyCount = 2;
  rp_info.pDependencies = deps;

  res = vkCreateRenderPass (cache->device->device, &rp_info, NULL,
                            &setup->render_pass);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkCreateRenderPass failed (%d)", (int) res);
      g_free (setup);
      return NULL;
    }

  /* Clear-from-UNDEFINED variant (compatible pipelines). */
  attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (ds_format != VK_FORMAT_UNDEFINED)
    {
      attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
  res = vkCreateRenderPass (cache->device->device, &rp_info, NULL,
                            &setup->render_pass_clear);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkCreateRenderPass (clear) failed (%d)", (int) res);
      vkDestroyRenderPass (cache->device->device, setup->render_pass, NULL);
      g_free (setup);
      return NULL;
    }

  /* Mixed: color already defined (LOAD) + DS still UNDEFINED (CLEAR).
   * Using full clear RP here is illegal — color initialLayout would be
   * UNDEFINED while the image is already COLOR_ATTACHMENT → ANV DEVICE_LOST.
   */
  setup->render_pass_clear_ds_only = VK_NULL_HANDLE;
  if (ds_format != VK_FORMAT_UNDEFINED)
    {
      attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      res = vkCreateRenderPass (cache->device->device, &rp_info, NULL,
                                &setup->render_pass_clear_ds_only);
      if (res != VK_SUCCESS)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "vkCreateRenderPass (clear-ds) failed (%d)", (int) res);
          vkDestroyRenderPass (cache->device->device, setup->render_pass_clear, NULL);
          vkDestroyRenderPass (cache->device->device, setup->render_pass, NULL);
          g_free (setup);
          return NULL;
        }
    }

  /* Restore LOAD desc in local attachments (unused after this). */
  attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  if (ds_format != VK_FORMAT_UNDEFINED)
    {
      attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      attachments[1].initialLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }

  if (ds_format != VK_FORMAT_UNDEFINED)
    {
      if (!cogl_vk_create_tex_pipeline (cache, setup->render_pass,
                                        cache->vert_module,
                                        cache->tex_frag_module,
                                        cache->pipe_layout, TRUE, &ds_color,
                                        &setup->tex_pipe, error) ||
          !cogl_vk_create_tex_pipeline (cache, setup->render_pass,
                                        cache->vert_module,
                                        cache->tex_frag_module,
                                        cache->pipe_layout, FALSE, &ds_color,
                                        &setup->tex_pipe_opaque, error) ||
          !cogl_vk_create_tex_pipeline (cache, setup->render_pass,
                                        cache->vert_module,
                                        cache->quad_frag_module,
                                        cache->pipe_layout, TRUE, &ds_color,
                                        &setup->quad_pipe, error) ||
          !cogl_vk_create_tex_pipeline (cache, setup->render_pass,
                                        cache->vert_module,
                                        cache->quad_frag_module,
                                        cache->pipe_layout, FALSE, &st_replace,
                                        &setup->stencil_replace_pipe, error) ||
          !cogl_vk_create_tex_pipeline (cache, setup->render_pass,
                                        cache->vert_module,
                                        cache->quad_frag_module,
                                        cache->pipe_layout, FALSE, &st_incr,
                                        &setup->stencil_incr_pipe, error) ||
          !cogl_vk_create_tex_pipeline (cache, setup->render_pass,
                                        cache->vert_module,
                                        cache->quad_frag_module,
                                        cache->pipe_layout, FALSE, &st_decr,
                                        &setup->stencil_decr_pipe, error))
        goto fail_setup;
    }
  else
    {
      CoglVkTexPipeExtras ms_ex = {0};
      const CoglVkTexPipeExtras *ex =
        sample_count > VK_SAMPLE_COUNT_1_BIT ? &ms_ex : NULL;

      ms_ex.samples = sample_count;
      if (!cogl_vk_create_tex_pipeline (cache, setup->render_pass,
                                        cache->vert_module,
                                        cache->tex_frag_module,
                                        cache->pipe_layout, TRUE, ex,
                                        &setup->tex_pipe, error) ||
          !cogl_vk_create_tex_pipeline (cache, setup->render_pass,
                                        cache->vert_module,
                                        cache->tex_frag_module,
                                        cache->pipe_layout, FALSE, ex,
                                        &setup->tex_pipe_opaque, error) ||
          !cogl_vk_create_tex_pipeline (cache, setup->render_pass,
                                        cache->vert_module,
                                        cache->quad_frag_module,
                                        cache->pipe_layout, TRUE, ex,
                                        &setup->quad_pipe, error))
        goto fail_setup;
    }

  for (unsigned int i = 0; i < COGL_VK_YCBCR_VARIANTS; i++)
    if (cache->pipe_layout_ycbcr[i] != VK_NULL_HANDLE)
      {
        CoglVkTexPipeExtras ycbcr_ex = { 0 };

        if (ds_format != VK_FORMAT_UNDEFINED)
          ycbcr_ex = ds_color;
        ycbcr_ex.samples = sample_count;

        if (!cogl_vk_create_tex_pipeline (cache, setup->render_pass,
                                          cache->vert_module,
                                          cache->tex_frag_module,
                                          cache->pipe_layout_ycbcr[i], TRUE,
                                          &ycbcr_ex, &setup->tex_pipe_ycbcr[i],
                                          error) ||
            !cogl_vk_create_tex_pipeline (cache, setup->render_pass,
                                          cache->vert_module,
                                          cache->tex_frag_module,
                                          cache->pipe_layout_ycbcr[i], FALSE,
                                          &ycbcr_ex, &setup->tex_pipe_ycbcr_opaque[i],
                                          error))
          goto fail_setup;
      }

  setup->next = cache->format_setups;
  cache->format_setups = setup;
  return setup;

fail_setup:
  if (setup->tex_pipe)
    vkDestroyPipeline (cache->device->device, setup->tex_pipe, NULL);
  if (setup->tex_pipe_opaque)
    vkDestroyPipeline (cache->device->device, setup->tex_pipe_opaque, NULL);
  for (unsigned int i = 0; i < COGL_VK_YCBCR_VARIANTS; i++)
    {
      if (setup->tex_pipe_ycbcr[i])
        vkDestroyPipeline (cache->device->device, setup->tex_pipe_ycbcr[i], NULL);
      if (setup->tex_pipe_ycbcr_opaque[i])
        vkDestroyPipeline (cache->device->device, setup->tex_pipe_ycbcr_opaque[i], NULL);
    }
  if (setup->quad_pipe)
    vkDestroyPipeline (cache->device->device, setup->quad_pipe, NULL);
  if (setup->stencil_replace_pipe)
    vkDestroyPipeline (cache->device->device, setup->stencil_replace_pipe, NULL);
  if (setup->stencil_incr_pipe)
    vkDestroyPipeline (cache->device->device, setup->stencil_incr_pipe, NULL);
  if (setup->stencil_decr_pipe)
    vkDestroyPipeline (cache->device->device, setup->stencil_decr_pipe, NULL);
  if (setup->render_pass)
    vkDestroyRenderPass (cache->device->device, setup->render_pass, NULL);
  if (setup->render_pass_clear)
    vkDestroyRenderPass (cache->device->device, setup->render_pass_clear, NULL);
  if (setup->render_pass_clear_ds_only)
    vkDestroyRenderPass (cache->device->device, setup->render_pass_clear_ds_only, NULL);
  g_free (setup);
  return NULL;
}

gboolean
cogl_vk_pipeline_alloc_texture_ds (CoglVkPipelineCache *cache,
                                   unsigned int         ycbcr,
                                   VkDescriptorSet     *out_ds,
                                   GError             **error)
{
  VkDescriptorSetLayout layout = ycbcr ? cache->ds_layout_ycbcr[ycbcr - 1]
                                       : cache->ds_layout;
  VkDescriptorSetAllocateInfo info = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .descriptorPool = cache->descriptor_pool,
    .descriptorSetCount = 1,
    .pSetLayouts = &layout,
  };

  if (ycbcr && layout == VK_NULL_HANDLE)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "YCbCr descriptor layout unavailable");
      return FALSE;
    }

  if (!cogl_vk_pipeline_alloc_from_pool (cache, &info, out_ds, TRUE,
                                         "vkAllocateDescriptorSets failed",
                                         error))
    return FALSE;
  if (!cache->ds_texture_owners)
    cache->ds_texture_owners =
      g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);
  for (unsigned int i = 0; i < cache->ds_pool_slots->len; i++)
    {
      CoglVkDsPoolSlot *slot = g_ptr_array_index (cache->ds_pool_slots, i);

      if (slot->pool == info.descriptorPool)
        {
          uint64_t *key = g_new (uint64_t, 1);

          *key = (uint64_t) (uintptr_t) *out_ds;
          g_hash_table_insert (cache->ds_texture_owners, key, slot);
          slot->texture_owners++;
          break;
        }
    }
  return TRUE;
}

void
cogl_vk_pipeline_free_texture_ds (CoglVkPipelineCache *cache,
                                  unsigned int         ycbcr,
                                  VkDescriptorSet      ds)
{
  uint64_t key = (uint64_t) (uintptr_t) ds;
  CoglVkDsPoolSlot *slot;

  if (!cache || ds == VK_NULL_HANDLE)
    return;

  if (!cache->ds_texture_owners)
    return;
  slot = g_hash_table_lookup (cache->ds_texture_owners, &key);
  if (!slot)
    return;
  g_assert (slot->texture_owners > 0);
  slot->texture_owners--;
  if (cache->device->owner && cache->device->owner->current_pass)
    slot->pending_recording = TRUE;
  g_hash_table_remove (cache->ds_texture_owners, &key);
  if (slot->persistent)
    {
      CoglVkRetiredDescriptor retired = {
        .pool = slot->pool,
        .set = ds,
        .timeline = slot->timeline,
        .point = slot->busy_point,
        .pending_recording = slot->pending_recording,
      };
      if (!cache->ds_retired)
        cache->ds_retired =
          g_array_new (FALSE, FALSE, sizeof (CoglVkRetiredDescriptor));
      g_array_append_val (cache->ds_retired, retired);
    }
}

void
cogl_vk_pipeline_update_texture_ds (CoglVkPipelineCache *cache,
                                    VkDescriptorSet      ds,
                                    VkImageView          view,
                                    unsigned int         ycbcr,
                                    VkSampler            sampler)
{
  VkDescriptorImageInfo image_info = {
    /* YCbCr layout still uses immutable sampler (field ignored). */
    .sampler = ycbcr ? cache->ycbcr_sampler[ycbcr - 1]
                     : (sampler != VK_NULL_HANDLE ? sampler : cache->sampler),
    .imageView = view,
    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };
  VkWriteDescriptorSet write = {
    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
    .dstSet = ds,
    .dstBinding = 0,
    .descriptorCount = 1,
    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    .pImageInfo = &image_info,
  };
  vkUpdateDescriptorSets (cache->device->device, 1, &write, 0, NULL);
}

void
cogl_vk_pipeline_push_texture_ds (CoglVkPipelineCache *cache,
                                  VkCommandBuffer      cmd,
                                  VkImageView          view,
                                  VkSampler            sampler)
{
  VkDescriptorImageInfo image_info = {
    .sampler = sampler != VK_NULL_HANDLE ? sampler : cache->sampler,
    .imageView = view,
    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };
  VkWriteDescriptorSet write = {
    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
    .dstBinding = 0,
    .descriptorCount = 1,
    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    .pImageInfo = &image_info,
  };

  /* dstSet is ignored for push descriptors; the set is embedded in the CB. */
  cache->device->vkCmdPushDescriptorSetKHR (cmd,
                                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            cache->pipe_layout, 0,
                                            1, &write);
}

gboolean
cogl_vk_pass_begin_render (CoglVkPass              *pass,
                           CoglVkPipelineCache     *cache,
                           CoglVkImage             *color,
                           CoglVkRenderFormatSetup *setup,
                           uint32_t                 width,
                           uint32_t                 height,
                           GError                 **error)
{
  CoglVkImage *ds = NULL;
  VkImageView attachments[2];
  uint32_t n_attachments = 1;
  VkFramebufferCreateInfo fb_info = {
    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
    .renderPass = setup->render_pass,
    .width = width,
    .height = height,
    .layers = 1,
  };
  VkRenderPassBeginInfo rp_begin = {
    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
    .renderPass = setup->render_pass,
    .renderArea = { .extent = { width, height } },
  };
  VkViewport viewport = {
    .x = 0.f,
    .y = 0.f,
    .width = (float) width,
    .height = (float) height,
    .minDepth = 0.f,
    .maxDepth = 1.f,
  };
  VkRect2D scissor = { .extent = { width, height } };
  VkResult res;
  gboolean need_new_fb;
  VkPipelineStageFlags src_stage =
    VK_PIPELINE_STAGE_TRANSFER_BIT |
    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
  VkAccessFlags src_access =
    VK_ACCESS_TRANSFER_WRITE_BIT |
    VK_ACCESS_TRANSFER_READ_BIT |
    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
    VK_ACCESS_MEMORY_WRITE_BIT;

  g_return_val_if_fail (pass != NULL && cache != NULL && color != NULL, FALSE);

  if (setup->ds_format != VK_FORMAT_UNDEFINED)
    {
      ds = color->attached_depth_stencil;
      if (!ds || ds->format != setup->ds_format)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "DS format setup requires attached depth/stencil");
          return FALSE;
        }
    }

  /* Proven journal path: oneshot-define UNDEFINED attachments on the
   * oneshot_command_pool, then always LOAD RP. Clear-RP variants with
   * mismatched initialLayout caused ANV DEVICE_LOST on get_data.
   */
  if (color->layout == VK_IMAGE_LAYOUT_UNDEFINED)
    {
      cogl_vk_debug_log ("draw", "begin_render oneshot-clear UNDEFINED color=%p",
                         (void *) color);
      if (!cogl_vk_image_oneshot_clear_to_color_attachment (color, error))
        return FALSE;
    }
  if (ds && ds->layout == VK_IMAGE_LAYOUT_UNDEFINED)
    {
      cogl_vk_debug_log ("draw", "begin_render oneshot-clear UNDEFINED ds=%p",
                         (void *) ds);
      if (!cogl_vk_image_oneshot_clear_to_depth_stencil_attachment (ds, error))
        return FALSE;
    }

  if (pass->viewport_valid)
    viewport = pass->viewport;

  if (pass->in_render_pass &&
      pass->render_color == color &&
      pass->render_depth_stencil == ds &&
      pass->render_setup == setup)
    {
      cogl_vk_debug_log ("draw",
                         "begin_render REUSE pass=%p color=%p layout=%d",
                         (void *) pass, (void *) color, (int) color->layout);
      vkCmdSetViewport (pass->command_buffer, 0, 1, &viewport);
      pass->viewport = viewport;
      pass->viewport_valid = TRUE;
      /* Dynamic state outlives draws: a removed clip must reset to full. */
      vkCmdSetScissor (pass->command_buffer, 0, 1,
                       pass->scissor_valid ? &pass->scissor : &scissor);
      if (setup->ds_format != VK_FORMAT_UNDEFINED)
        cogl_vk_pass_apply_stencil_dynamic (pass, 0);
      return TRUE;
    }

  cogl_vk_pass_end_render (pass);

  cogl_vk_debug_log (
    "draw",
    "begin_render NEW pass=%p color=%p layout_in=%d ds=%p "
    "was_in_rp=%d vp=%g,%g %gx%g scissor_valid=%d",
    (void *) pass, (void *) color, (int) color->layout,
    (void *) ds, pass->in_render_pass,
    viewport.x, viewport.y, viewport.width, viewport.height,
    pass->scissor_valid);

  cogl_vk_image_cmd_barrier (color,
                             pass->command_buffer,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             src_stage,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             src_access,
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
  if (color->is_dmabuf)
    cogl_vk_pass_note_foreign_image (pass, color);
  if (ds)
    {
      cogl_vk_image_cmd_barrier (
        ds,
        pass->command_buffer,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    }

  cogl_vk_debug_log (
    "draw",
    "begin_render after_barrier layout=%d src_access=0x%x",
    (int) color->layout, (unsigned) src_access);

  if (!cogl_vk_image_ensure_attachment_view (color, error))
    return FALSE;
  attachments[0] = color->attachment_view;
  if (ds)
    {
      if (!cogl_vk_image_ensure_attachment_view (ds, error))
        return FALSE;
      attachments[1] = ds->attachment_view;
      n_attachments = 2;
    }
  fb_info.attachmentCount = n_attachments;
  fb_info.pAttachments = attachments;

  need_new_fb = (color->framebuffer == VK_NULL_HANDLE ||
                 color->framebuffer_render_pass != setup->render_pass ||
                 color->framebuffer_ds != ds);
  if (need_new_fb && color->framebuffer != VK_NULL_HANDLE)
    {
      if (cache->device->graphics_queue != VK_NULL_HANDLE)
        vkQueueWaitIdle (cache->device->graphics_queue);
      vkDestroyFramebuffer (cache->device->device, color->framebuffer, NULL);
      color->framebuffer = VK_NULL_HANDLE;
    }

  if (color->framebuffer == VK_NULL_HANDLE)
    {
      res = vkCreateFramebuffer (cache->device->device, &fb_info, NULL,
                                 &color->framebuffer);
      if (res != VK_SUCCESS)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "vkCreateFramebuffer failed (%d)", (int) res);
          return FALSE;
        }
      color->framebuffer_render_pass = setup->render_pass;
      color->framebuffer_ds = ds;
    }

  if (pass->scissor_valid)
    scissor = pass->scissor;

  cogl_vk_debug_log (
    "draw",
    "BeginRenderPass pass=%p color=%p layout=%d need_new_fb=%d "
    "fb=%p rp=%p load=LOAD scissor=%d,%d %dx%d",
    (void *) pass, (void *) color, (int) color->layout, need_new_fb,
    (void *) (uintptr_t) color->framebuffer,
    (void *) (uintptr_t) setup->render_pass,
    scissor.offset.x, scissor.offset.y,
    (int) scissor.extent.width, (int) scissor.extent.height);

  rp_begin.framebuffer = color->framebuffer;
  vkCmdBeginRenderPass (pass->command_buffer, &rp_begin,
                        VK_SUBPASS_CONTENTS_INLINE);
  pass->in_render_pass = TRUE;
  pass->render_color = color;
  pass->render_depth_stencil = ds;
  pass->render_setup = setup;
  vkCmdSetViewport (pass->command_buffer, 0, 1, &viewport);
  pass->viewport = viewport;
  pass->viewport_valid = TRUE;
  vkCmdSetScissor (pass->command_buffer, 0, 1, &scissor);
  if (setup->ds_format != VK_FORMAT_UNDEFINED)
    cogl_vk_pass_apply_stencil_dynamic (pass, 0);

  if (g_strcmp0 (g_getenv ("COGL_VK_FORCE_CLEAR"), "attach") == 0 &&
      color->is_dmabuf && width >= 1920)
    {
      VkClearAttachment att = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .colorAttachment = 0,
        .clearValue.color.float32 = { 1.f, 0.f, 0.f, 1.f },
      };
      VkClearRect rect = {
        .rect.offset = { 0, 0 },
        .rect.extent = { width, height },
        .baseArrayLayer = 0,
        .layerCount = 1,
      };
      static int n_att;

      vkCmdClearAttachments (pass->command_buffer, 1, &att, 1, &rect);
      if (n_att < 4)
        {
          n_att++;
          g_printerr ("VKATTACHCLEAR %ux%u sc=%d,%d %ux%u vp=%.1f,%.1f %.1fx%.1f\n",
                      width, height,
                      scissor.offset.x, scissor.offset.y,
                      scissor.extent.width, scissor.extent.height,
                      viewport.x, viewport.y, viewport.width, viewport.height);
        }
    }
  return TRUE;
}


void
cogl_vk_pass_apply_stencil_dynamic (CoglVkPass *pass,
                                    uint32_t    write_mask)
{
  uint32_t compare_mask;
  uint32_t reference;

  if (!pass || !pass->in_render_pass || !pass->command_buffer)
    return;

  if (pass->stencil_clip_active)
    {
      compare_mask = 0x1;
      reference = 0x1;
    }
  else
    {
      /* compareMask=0 → EQUAL always passes regardless of stencil value. */
      compare_mask = 0;
      reference = 0;
    }

  vkCmdSetStencilCompareMask (pass->command_buffer,
                              VK_STENCIL_FACE_FRONT_AND_BACK, compare_mask);
  vkCmdSetStencilWriteMask (pass->command_buffer,
                            VK_STENCIL_FACE_FRONT_AND_BACK, write_mask);
  vkCmdSetStencilReference (pass->command_buffer,
                            VK_STENCIL_FACE_FRONT_AND_BACK, reference);
}

void
cogl_vk_pass_end_render (CoglVkPass *pass)
{
  if (!pass || !pass->in_render_pass)
    return;

  cogl_vk_debug_log (
    "draw",
    "end_render pass=%p color=%p layout=%d (finalLayout=COLOR_ATTACHMENT)",
    (void *) pass,
    (void *) pass->render_color,
    pass->render_color ? (int) pass->render_color->layout : -1);

  vkCmdEndRenderPass (pass->command_buffer);
  pass->in_render_pass = FALSE;
  pass->render_color = NULL;
  pass->render_depth_stencil = NULL;
  pass->render_setup = NULL;
}

gboolean
cogl_vk_cmd_draw_stencil_quad (CoglVkPass              *pass,
                               CoglVkPipelineCache     *cache,
                               CoglVkRenderFormatSetup *setup,
                               CoglVkImage             *dst,
                               const CoglVkVertPush    *vert_push,
                               CoglVkStencilClipOp      op,
                               GError                 **error)
{
  CoglVkVertPush vert;
  float color[4] = { 0.f, 0.f, 0.f, 0.f };
  VkPipeline pipe = VK_NULL_HANDLE;
  uint32_t write_mask = 0x3;

  g_return_val_if_fail (pass && cache && setup && dst && vert_push, FALSE);

  if (setup->ds_format == VK_FORMAT_UNDEFINED)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "stencil clip requires DS render format setup");
      return FALSE;
    }

  switch (op)
    {
    case COGL_VK_STENCIL_CLIP_OP_REPLACE:
      pipe = setup->stencil_replace_pipe;
      write_mask = 0x1;
      break;
    case COGL_VK_STENCIL_CLIP_OP_INCR:
      pipe = setup->stencil_incr_pipe;
      break;
    case COGL_VK_STENCIL_CLIP_OP_DECR:
      pipe = setup->stencil_decr_pipe;
      break;
    }

  if (pipe == VK_NULL_HANDLE)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "stencil clip pipeline unavailable");
      return FALSE;
    }

  if (!cogl_vk_pass_begin_render (pass, cache, dst, setup,
                                  dst->width, dst->height, error))
    return FALSE;

  cogl_vk_pass_apply_stencil_dynamic (pass, write_mask);

  vert = *vert_push;
  vkCmdBindPipeline (pass->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                     pipe);
  vkCmdPushConstants (pass->command_buffer, cache->pipe_layout,
                      VK_SHADER_STAGE_VERTEX_BIT, 0,
                      sizeof (vert), &vert);
  vkCmdPushConstants (pass->command_buffer, cache->pipe_layout,
                      VK_SHADER_STAGE_FRAGMENT_BIT, sizeof (vert),
                      sizeof (color), color);
  vkCmdDraw (pass->command_buffer, 4, 1, 0, 0);

  /* Restore write mask 0 for subsequent color draws. */
  cogl_vk_pass_apply_stencil_dynamic (pass, 0);
  return TRUE;
}


gboolean
cogl_vk_cmd_draw_texture (CoglVkPass              *pass,
                          CoglVkPipelineCache     *cache,
                          CoglVkRenderFormatSetup *setup,
                          CoglVkImage             *dst,
                          CoglVkImage             *src,
                          VkDescriptorSet          src_ds,
                          float                    alpha,
                          gboolean                 opaque,
                          const CoglVkVertPush    *vert_push,
                          VkPipeline               pipe_override,
                          CoglPipeline            *cogl_pipeline,
                          GError                 **error)
{
  CoglVkVertPush vert;
  /* wrap_s/wrap_t default clamp (0), matching GL AUTOMATIC. */
  float frag_pc[4] = { alpha, 0.f, 0.f, 0.f };
  VkPipeline pipe;
  VkPipelineLayout layout;
  VkDescriptorSet bind_ds = src_ds;
  VkDescriptorSet cold_ds = VK_NULL_HANDLE;
  VkDescriptorSet transient_ds = VK_NULL_HANDLE;
  gboolean pushed = FALSE;
  gboolean use_cold;
  gboolean apply_blend_constants = FALSE;

  /* Cold pipe uses pipe_layout_cold (sampler[N]+UBO). Alias to hot tex_pipe
   * keeps the hot single-binding layout. */
  use_cold = (pipe_override != VK_NULL_HANDLE &&
              cogl_pipeline != NULL &&
              pipe_override != setup->tex_pipe &&
              pipe_override != setup->tex_pipe_opaque &&
              cache->pipe_layout_cold != VK_NULL_HANDLE);


  if (use_cold &&
      !cogl_vk_pipeline_prepare_cold_images (pass, cache, cogl_pipeline,
                                             &src, error))
    return FALSE;

  /* Acquire foreign textures and transition sampled layers before
   * beginning the render pass. */
  if (cogl_vk_pipeline_layer_textures_need_barrier (pass, cogl_pipeline, src,
                                                    use_cold))
    {
      cogl_vk_pass_end_render (pass);
      cogl_vk_pipeline_barrier_layer_textures (pass, cogl_pipeline, src,
                                               use_cold);
    }

  if (!cogl_vk_pass_begin_render (pass, cache, dst, setup,
                                  dst->width, dst->height, error))
    return FALSE;

  if (cogl_pipeline)
    cogl_vk_pass_apply_depth_range (pass, cogl_pipeline);


  if (pipe_override != VK_NULL_HANDLE && !src->is_ycbcr)
    {
      pipe = pipe_override;
      layout = use_cold ? cache->pipe_layout_cold : cache->pipe_layout;
      apply_blend_constants = use_cold;
    }
  else if (src->is_ycbcr)
    {
      uint8_t kind = opaque ? COGL_VK_HOT_KIND_YCBCR_OPAQUE
                           : COGL_VK_HOT_KIND_YCBCR;
      pipe = opaque ? setup->tex_pipe_ycbcr_opaque[src->ycbcr_index]
                    : setup->tex_pipe_ycbcr[src->ycbcr_index];
      layout = cache->pipe_layout_ycbcr[src->ycbcr_index];
      if (cogl_pipeline)
        {
          CoglVkTexPipeExtras ex;
          CoglDriver *drv = cogl_context_get_driver (cogl_pipeline->context);
          VkFrontFace ff = VK_FRONT_FACE_COUNTER_CLOCKWISE;

          if (COGL_IS_DRIVER_VK (drv))
            ff = COGL_DRIVER_VK (drv)->pending_front_face;
          cogl_vk_tex_pipe_extras_from_pipeline (cogl_pipeline, ff, &ex);
          if (setup->ds_format != VK_FORMAT_UNDEFINED)
            ex.stencil_equal_keep = TRUE;
          if (setup->samples > VK_SAMPLE_COUNT_1_BIT)
            ex.samples = setup->samples;
          if (cogl_vk_tex_pipe_extras_need_keyed_hot (&ex, !opaque))
            {
              g_autoptr (GError) hot_err = NULL;
              VkPipeline keyed =
                cogl_vk_pipeline_cache_get_hot_extras_pipe (cache, setup,
                                                            cogl_pipeline,
                                                            kind, src->ycbcr_index, &hot_err);
              if (keyed != VK_NULL_HANDLE)
                {
                  pipe = keyed;
                  apply_blend_constants = extras_need_blend_constants (&ex);
                }
              else if (hot_err)
                g_debug ("Cogl Vulkan hot extras: %s", hot_err->message);
            }
        }
      if (pipe == VK_NULL_HANDLE || layout == VK_NULL_HANDLE)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                               "YCbCr pipeline unavailable");
          return FALSE;
        }
    }
  else
    {
      uint8_t kind = opaque ? COGL_VK_HOT_KIND_TEX_OPAQUE : COGL_VK_HOT_KIND_TEX;
      pipe = opaque ? setup->tex_pipe_opaque : setup->tex_pipe;
      layout = cache->pipe_layout;
      if (cogl_pipeline)
        {
          CoglVkTexPipeExtras ex;
          CoglDriver *drv = cogl_context_get_driver (cogl_pipeline->context);
          VkFrontFace ff = VK_FRONT_FACE_COUNTER_CLOCKWISE;

          if (COGL_IS_DRIVER_VK (drv))
            ff = COGL_DRIVER_VK (drv)->pending_front_face;
          cogl_vk_tex_pipe_extras_from_pipeline (cogl_pipeline, ff, &ex);
          if (setup->ds_format != VK_FORMAT_UNDEFINED)
            ex.stencil_equal_keep = TRUE;
          if (setup->samples > VK_SAMPLE_COUNT_1_BIT)
            ex.samples = setup->samples;
          if (cogl_vk_tex_pipe_extras_need_keyed_hot (&ex, !opaque))
            {
              g_autoptr (GError) hot_err = NULL;
              VkPipeline keyed =
                cogl_vk_pipeline_cache_get_hot_extras_pipe (cache, setup,
                                                            cogl_pipeline,
                                                            kind, 0, &hot_err);
              if (keyed != VK_NULL_HANDLE)
                {
                  pipe = keyed;
                  apply_blend_constants = extras_need_blend_constants (&ex);
                }
              else if (hot_err)
                g_debug ("Cogl Vulkan hot extras: %s", hot_err->message);
            }
        }
    }

  if (use_cold)
    {
      if (!cogl_vk_pipeline_prepare_cold_ds (cache, cogl_pipeline, src,
                                             &cold_ds, error))
        return FALSE;
      bind_ds = cold_ds;
    }
  else if (cogl_pipeline && !src->is_ycbcr && bind_ds != VK_NULL_HANDLE)
    {
      /* Refresh non-immutable sampler from first unit layer (not index 0). */
      CoglDriver *drv = cogl_context_get_driver (cogl_pipeline->context);
      g_autofree CoglPipelineLayer **layers = NULL;
      int n_layers = 0;
      CoglPipelineLayer *layer = NULL;

      cogl_vk_pipeline_collect_layers (cogl_pipeline, &layers, &n_layers);
      if (n_layers > 0)
        layer = layers[0];
      if (COGL_IS_DRIVER_VK (drv) && layer)
        {
          const CoglSamplerCacheEntry *entry =
            _cogl_pipeline_layer_get_sampler_state (layer);
          CoglSamplerCacheWrapMode wrap_s =
            COGL_SAMPLER_CACHE_WRAP_MODE_AUTOMATIC;
          CoglSamplerCacheWrapMode wrap_t =
            COGL_SAMPLER_CACHE_WRAP_MODE_AUTOMATIC;
          VkSampler samp =
            cogl_driver_vk_peek_sampler (COGL_DRIVER_VK (drv), entry);

          _cogl_pipeline_layer_get_wrap_modes (layer, &wrap_s, &wrap_t);
          frag_pc[1] = cogl_vk_shader_wrap_mode ((int) wrap_s);
          frag_pc[2] = cogl_vk_shader_wrap_mode ((int) wrap_t);
          cogl_vk_debug_log (
            "tex",
            "cmd_draw_texture wrap_pc=(%.0f,%.0f) enum=(%d,%d) samp=%p",
            frag_pc[1], frag_pc[2], (int) wrap_s, (int) wrap_t,
            (void *) (uintptr_t) samp);
          /* Push descriptor writes the sampler directly into the CB, so no
           * per-draw set is allocated, updated, or freed — and there is no
           * cross-draw aliasing to worry about since nothing is shared. When
           * the extension is absent, fall back to a fresh per-draw set (a
           * cached set must not be mutated while earlier draws in the same CB
           * still reference it).
           */
          if (cache->device->has_push_descriptor)
            {
              cogl_vk_pipeline_push_texture_ds (cache, pass->command_buffer,
                                                src->view, samp);
              pushed = TRUE;
            }
          else
            {
              if (!cogl_vk_pipeline_alloc_texture_ds (cache, FALSE,
                                                      &transient_ds, error))
                return FALSE;
              cogl_vk_pipeline_update_texture_ds (cache, transient_ds,
                                                  src->view, FALSE, samp);
              bind_ds = transient_ds;
            }
        }
    }

  if (vert_push)
    vert = *vert_push;
  else
    {
      CoglFramebuffer *fb = NULL;

      if (cogl_pipeline && cogl_pipeline->context)
        fb = cogl_context_get_current_draw_buffer (cogl_pipeline->context);
      cogl_vk_vert_push_ortho_uv_unit (&vert, fb, cogl_pipeline);
    }
  cogl_vk_debug_log (
    "draw",
    "cmd_draw_texture uv_off=(%.3f,%.3f) uv_size=(%.3f,%.3f) "
    "vert_push=%d dst=%ux%u src=%ux%u",
    vert.uv_off[0], vert.uv_off[1], vert.uv_size[0], vert.uv_size[1],
    vert_push != NULL,
    dst ? dst->width : 0, dst ? dst->height : 0,
    src ? src->width : 0, src ? src->height : 0);
  vkCmdBindPipeline (pass->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                     pipe);
  /* Only when bound PSO declares VK_DYNAMIC_STATE_BLEND_CONSTANTS. */
  if (apply_blend_constants && cogl_pipeline)
    cogl_vk_pass_apply_blend_constants (pass, cogl_pipeline);
  if (!pushed && !use_cold && !src->is_ycbcr &&
      cache->device->has_push_descriptor)
    {
      cogl_vk_pipeline_push_texture_ds (cache, pass->command_buffer,
                                        src->view, VK_NULL_HANDLE);
      pushed = TRUE;
    }

  /* Push descriptors already embedded the sampler in the CB above; only the
   * alloc/update fallback and cold/ycbcr paths bind a set object here.
   */
  if (!pushed)
    vkCmdBindDescriptorSets (pass->command_buffer,
                             VK_PIPELINE_BIND_POINT_GRAPHICS,
                             layout, 0, 1, &bind_ds, 0, NULL);
  vkCmdPushConstants (pass->command_buffer, layout,
                      VK_SHADER_STAGE_VERTEX_BIT, 0,
                      sizeof (vert), &vert);
  vkCmdPushConstants (pass->command_buffer, layout,
                      VK_SHADER_STAGE_FRAGMENT_BIT, sizeof (vert),
                      sizeof (frag_pc), frag_pc);
  vkCmdDraw (pass->command_buffer, 4, 1, 0, 0);

  if (transient_ds != VK_NULL_HANDLE)
    cogl_vk_pipeline_free_texture_ds (cache, FALSE, transient_ds);
  if (cold_ds != VK_NULL_HANDLE)
    cogl_vk_pipeline_free_cold_ds (cache, cold_ds);

  /* Close the RP before returning to Cogl — atlas/texture allocate between
   * draw batches must not see in_rp=1 (wrap-modes mid-frame DEVICE_LOST).
   */
  cogl_vk_pass_end_render (pass);

  return TRUE;
}

gboolean
cogl_vk_cmd_draw_quad (CoglVkPass              *pass,
                       CoglVkPipelineCache     *cache,
                       CoglVkRenderFormatSetup *setup,
                       CoglVkImage             *dst,
                       float                    r,
                       float                    g,
                       float                    b,
                       float                    a,
                       const CoglVkVertPush    *vert_push,
                       CoglFramebuffer         *fb,
                       CoglPipeline            *pipeline,
                       GError                 **error)
{
  CoglVkVertPush vert;
  float color[4] = { r, g, b, a };
  VkPipeline pipe = setup->quad_pipe;
  gboolean apply_blend_constants = FALSE;
  CoglVkTexPipeExtras extras;

  memset (&extras, 0, sizeof (extras));

  if (pipeline)
    {
      CoglDriver *drv = cogl_context_get_driver (pipeline->context);
      VkFrontFace ff = VK_FRONT_FACE_COUNTER_CLOCKWISE;

      if (drv && COGL_IS_DRIVER_VK (drv))
        ff = COGL_DRIVER_VK (drv)->pending_front_face;
      cogl_vk_tex_pipe_extras_from_pipeline (pipeline, ff, &extras);
      if (setup->ds_format != VK_FORMAT_UNDEFINED)
        extras.stencil_equal_keep = TRUE;
      if (setup->samples > VK_SAMPLE_COUNT_1_BIT)
        extras.samples = setup->samples;

      if (cogl_vk_tex_pipe_extras_need_keyed_hot (&extras, TRUE))
        {
          pipe = cogl_vk_pipeline_cache_get_hot_extras_pipe (
            cache, setup, pipeline, COGL_VK_HOT_KIND_QUAD, 0, error);
          if (pipe == VK_NULL_HANDLE)
            return FALSE;
          apply_blend_constants = extras_need_blend_constants (&extras);
        }
    }

  if (pipe == VK_NULL_HANDLE)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "solid quad pipeline unavailable");
      return FALSE;
    }

  if (!cogl_vk_pass_begin_render (pass, cache, dst, setup,
                                  dst->width, dst->height, error))
    return FALSE;

  if (pipeline)
    cogl_vk_pass_apply_depth_range (pass, pipeline);

  if (vert_push)
    vert = *vert_push;
  else
    cogl_vk_vert_push_ortho_uv_unit (&vert, fb, pipeline);

  {
    static int n_quad;

    if (n_quad < 6 && dst->is_dmabuf && dst->width >= 1920)
      {
        n_quad++;
        g_printerr ("VKQUAD ino=%" G_GUINT64_FORMAT
                    " sc_valid=%d sc=%d,%d %ux%u vp=%.1f,%.1f %.1fx%.1f "
                    "m00=%g m11=%g m03=%g m13=%g cull=%u dtest=%u sten=%u "
                    "rgba=%.2f,%.2f,%.2f,%.2f\n",
                    dst->debug_dmabuf_ino,
                    pass->scissor_valid,
                    pass->scissor.offset.x, pass->scissor.offset.y,
                    pass->scissor.extent.width, pass->scissor.extent.height,
                    pass->viewport.x, pass->viewport.y,
                    pass->viewport.width, pass->viewport.height,
                    vert.mat4[0][0], vert.mat4[1][1],
                    vert.mat4[0][3], vert.mat4[1][3],
                    (unsigned) extras.cull_mode,
                    extras.depth_test_enable,
                    extras.stencil_equal_keep,
                    r, g, b, a);
      }
  }

  if (g_strcmp0 (g_getenv ("COGL_VK_FORCE_CLEAR"), "attach") == 0 &&
      dst->is_dmabuf && dst->width >= 1920)
    {
      VkClearAttachment att = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .colorAttachment = 0,
        .clearValue.color.float32 = { 1.f, 0.f, 0.f, 1.f },
      };
      VkClearRect rect = {
        .rect.extent = { dst->width, dst->height },
        .layerCount = 1,
      };

      vkCmdClearAttachments (pass->command_buffer, 1, &att, 1, &rect);
    }

  vkCmdBindPipeline (pass->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                     pipe);
  if (apply_blend_constants)
    cogl_vk_pass_apply_blend_constants (pass, pipeline);
  vkCmdPushConstants (pass->command_buffer, cache->pipe_layout,
                      VK_SHADER_STAGE_VERTEX_BIT, 0,
                      sizeof (vert), &vert);
  vkCmdPushConstants (pass->command_buffer, cache->pipe_layout,
                      VK_SHADER_STAGE_FRAGMENT_BIT, sizeof (vert),
                      sizeof (color), color);
  vkCmdDraw (pass->command_buffer, 4, 1, 0, 0);
  cogl_vk_pass_end_render (pass);
  return TRUE;
}

gboolean
cogl_vk_cmd_blit_images (CoglVkPass  *pass,
                         CoglVkImage *src,
                         CoglVkImage *dst,
                         int          src_x1,
                         int          src_y1,
                         int          src_x2,
                         int          src_y2,
                         int          dst_x1,
                         int          dst_y1,
                         int          dst_x2,
                         int          dst_y2,
                         uint32_t     src_mip,
                         uint32_t     dst_mip,
                         GError     **error)
{
  VkImageBlit region = {0};
  int src_w = src_x2 - src_x1;
  int src_h = src_y2 - src_y1;
  int dst_w = dst_x2 - dst_x1;
  int dst_h = dst_y2 - dst_y1;
  gboolean same_size;

  g_return_val_if_fail (pass && src && dst, FALSE);

  if (src_mip >= MAX (1u, src->mip_levels) ||
      dst_mip >= MAX (1u, dst->mip_levels))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "blit mip level out of range");
      return FALSE;
    }

  if (src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0)
    return TRUE;

  cogl_vk_pass_end_render (pass);

  cogl_vk_image_cmd_barrier (src,
                             pass->command_buffer,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT);
  cogl_vk_image_cmd_barrier (dst,
                             pass->command_buffer,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_ACCESS_TRANSFER_WRITE_BIT);

  same_size = (src_w == dst_w && src_h == dst_h && src->format == dst->format);
  if (same_size)
    {
      VkImageCopy copy = {0};

      copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      copy.srcSubresource.mipLevel = src_mip;
      copy.srcSubresource.layerCount = 1;
      copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      copy.dstSubresource.mipLevel = dst_mip;
      copy.dstSubresource.layerCount = 1;
      copy.srcOffset.x = MIN (src_x1, src_x2);
      copy.srcOffset.y = MIN (src_y1, src_y2);
      copy.dstOffset.x = MIN (dst_x1, dst_x2);
      copy.dstOffset.y = MIN (dst_y1, dst_y2);
      copy.extent.width = (uint32_t) ABS (src_w);
      copy.extent.height = (uint32_t) ABS (src_h);
      copy.extent.depth = 1;
      vkCmdCopyImage (pass->command_buffer,
                      src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      1, &copy);
    }
  else
    {
      region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      region.srcSubresource.mipLevel = src_mip;
      region.srcSubresource.layerCount = 1;
      region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      region.dstSubresource.mipLevel = dst_mip;
      region.dstSubresource.layerCount = 1;
      region.srcOffsets[0].x = src_x1;
      region.srcOffsets[0].y = src_y1;
      region.srcOffsets[1].x = src_x2;
      region.srcOffsets[1].y = src_y2;
      region.dstOffsets[0].x = dst_x1;
      region.dstOffsets[0].y = dst_y1;
      region.dstOffsets[1].x = dst_x2;
      region.dstOffsets[1].y = dst_y2;
      vkCmdBlitImage (pass->command_buffer,
                      src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      1, &region, VK_FILTER_NEAREST);
    }

  return TRUE;
}

gboolean
cogl_vk_cmd_resolve_image (CoglVkPass  *pass,
                           CoglVkImage *src_ms,
                           CoglVkImage *dst_1x,
                           GError     **error)
{
  VkImageResolve region = {0};

  g_return_val_if_fail (pass && src_ms && dst_1x, FALSE);

  if (src_ms->samples <= VK_SAMPLE_COUNT_1_BIT)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "resolve src is not multisampled");
      return FALSE;
    }
  if (dst_1x->samples > VK_SAMPLE_COUNT_1_BIT &&
      dst_1x->samples != 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "resolve dst must be single-sample");
      return FALSE;
    }

  cogl_vk_pass_end_render (pass);

  cogl_vk_image_cmd_barrier (src_ms,
                             pass->command_buffer,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT);
  cogl_vk_image_cmd_barrier (dst_1x,
                             pass->command_buffer,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_ACCESS_TRANSFER_WRITE_BIT);

  region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.srcSubresource.layerCount = 1;
  region.dstSubresource = region.srcSubresource;
  region.extent.width = MIN (src_ms->width, dst_1x->width);
  region.extent.height = MIN (src_ms->height, dst_1x->height);
  region.extent.depth = 1;

  vkCmdResolveImage (pass->command_buffer,
                     src_ms->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     dst_1x->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     1, &region);
  return TRUE;
}

/* --- Cold-path UBO (snippet uniforms) --- */

typedef struct
{
  char *name;
  char *glsl_type; /* float, int, vec2..4, mat2..4 */
  size_t offset;
  size_t size;
  int array_len; /* 0: scalar; positive: std140 array, name has no [N]. */
} CoglVkUboMember;

typedef struct
{
  const CoglBoxedValue **dst_values;
  const CoglBoxedValue *src_values;
  int override_count;
} CoglVkGetUniformsClosure;

static gboolean
cogl_vk_get_uniforms_cb (int   uniform_num,
                         void *user_data)
{
  CoglVkGetUniformsClosure *data = user_data;

  if (data->dst_values[uniform_num] == NULL)
    data->dst_values[uniform_num] = data->src_values + data->override_count;

  data->override_count++;
  return TRUE;
}

static void
cogl_vk_get_all_uniform_values (CoglPipeline          *pipeline,
                                const CoglBoxedValue **values)
{
  CoglVkGetUniformsClosure data;
  CoglContext *ctx = pipeline->context;
  int n_uniform_names = cogl_context_get_n_uniform_names (ctx);

  memset (values, 0, sizeof (const CoglBoxedValue *) * n_uniform_names);
  data.dst_values = values;

  do
    {
      if (pipeline->differences & COGL_PIPELINE_STATE_UNIFORMS)
        {
          const CoglPipelineUniformsState *uniforms_state =
            &pipeline->big_state->uniforms_state;

          data.override_count = 0;
          data.src_values = uniforms_state->override_values;
          _cogl_bitmask_foreach (&uniforms_state->override_mask,
                                 cogl_vk_get_uniforms_cb,
                                 &data);
        }
      pipeline = _cogl_pipeline_get_parent (pipeline);
    }
  while (pipeline);
}

static size_t
std140_align_up (size_t offset,
                 size_t align)
{
  return (offset + align - 1) & ~(align - 1);
}

static gboolean
std140_type_info (const char *glsl_type,
                  size_t     *out_align,
                  size_t     *out_size)
{
  if (strcmp (glsl_type, "float") == 0 ||
      strcmp (glsl_type, "int") == 0 ||
      strcmp (glsl_type, "bool") == 0)
    {
      *out_align = 4;
      *out_size = 4;
      return TRUE;
    }
  if (strcmp (glsl_type, "vec2") == 0 || strcmp (glsl_type, "ivec2") == 0)
    {
      *out_align = 8;
      *out_size = 8;
      return TRUE;
    }
  if (strcmp (glsl_type, "vec3") == 0 || strcmp (glsl_type, "ivec3") == 0)
    {
      *out_align = 16;
      *out_size = 12;
      return TRUE;
    }
  if (strcmp (glsl_type, "vec4") == 0 || strcmp (glsl_type, "ivec4") == 0)
    {
      *out_align = 16;
      *out_size = 16;
      return TRUE;
    }
  if (strcmp (glsl_type, "mat2") == 0)
    {
      *out_align = 16;
      *out_size = 32; /* 2 × vec4 columns in std140 */
      return TRUE;
    }
  if (strcmp (glsl_type, "mat3") == 0)
    {
      *out_align = 16;
      *out_size = 48; /* 3 × vec4 */
      return TRUE;
    }
  if (strcmp (glsl_type, "mat4") == 0)
    {
      *out_align = 16;
      *out_size = 64;
      return TRUE;
    }
  return FALSE;
}

static void
ubo_member_clear (gpointer data)
{
  CoglVkUboMember *m = data;
  g_free (m->name);
  g_free (m->glsl_type);
}

static gboolean
ubo_member_exists (GArray     *members,
                   const char *name)
{
  unsigned int i;

  if (!members || !name)
    return FALSE;
  for (i = 0; i < members->len; i++)
    {
      CoglVkUboMember *m = &g_array_index (members, CoglVkUboMember, i);
      if (m->name && strcmp (m->name, name) == 0)
        return TRUE;
    }
  return FALSE;
}

static void
append_uniform_decls_from_text (const char *text,
                                GString    *kept_decls,
                                GArray     *members,
                                size_t     *offset_inout)
{
  const char *p;

  if (!text || !*text)
    return;

  for (p = text; *p; )
    {
      const char *line_end = strchr (p, '\n');
      size_t line_len = line_end ? (size_t) (line_end - p) : strlen (p);
      g_autofree char *line = g_strndup (p, line_len);
      char type[32];
      char name[128];
      const char *s = line;

      while (*s == ' ' || *s == '\t')
        s++;

      if (sscanf (s, "uniform %31s %127[^;];", type, name) == 2)
        {
          size_t align = 0, size = 0;
          if (std140_type_info (type, &align, &size))
            {
              g_auto (GStrv) parts = g_strsplit (name, ",", -1);

              for (int pi = 0; parts[pi]; pi++)
                {
                  char *ident = g_strstrip (parts[pi]);
                  char *bracket = strchr (ident, '[');
                  CoglVkUboMember m = { 0 };
                  size_t member_align = align;

                  if (bracket)
                    {
                      if (sscanf (bracket, "[%d]", &m.array_len) != 1 ||
                          m.array_len <= 0)
                        continue;
                      *bracket = '\0';
                      g_strstrip (ident);
                      member_align = MAX (align, 16);
                    }
                  if (!*ident || ubo_member_exists (members, ident))
                    continue;

                  /* Arrays, including length one, have vec4 alignment.
                   * Repeated declarations must not advance the layout.
                   */
                  *offset_inout = std140_align_up (*offset_inout, member_align);
                  m.name = g_strdup (ident);
                  m.glsl_type = g_strdup (type);
                  m.offset = *offset_inout;
                  m.size = m.array_len > 0
                    ? std140_align_up (size, 16) * (size_t) m.array_len
                    : size;
                  g_array_append_val (members, m);
                  *offset_inout += m.size;
                }
            }
          else if (kept_decls)
            {
              g_string_append_len (kept_decls, p, line_len);
              g_string_append_c (kept_decls, '\n');
            }
        }
      else if (kept_decls && line_len > 0)
        {
          g_string_append_len (kept_decls, p, line_len);
          g_string_append_c (kept_decls, '\n');
        }

      if (!line_end)
        break;
      p = line_end + 1;
    }
}

static void
collect_snippet_uniform_decls (CoglPipeline *pipeline,
                               GString      *kept_decls,
                               GArray       *members,
                               size_t       *offset_inout)
{
  CoglPipeline *authority;
  GList *l;
  g_autofree CoglPipelineLayer **layers = NULL;
  int n_layers = 0;
  int i;

  authority = _cogl_pipeline_get_authority (pipeline,
                                            COGL_PIPELINE_STATE_FRAGMENT_SNIPPETS);
  if (authority && authority->has_big_state && authority->big_state)
    {
      for (l = authority->big_state->fragment_snippets.entries; l; l = l->next)
        {
          CoglSnippet *snippet = l->data;
          CoglSnippetHook hook = cogl_snippet_get_hook (snippet);
          const char *d;

          if (hook != COGL_SNIPPET_HOOK_FRAGMENT &&
              hook != COGL_SNIPPET_HOOK_FRAGMENT_GLOBALS)
            continue;

          d = cogl_snippet_get_declarations (snippet);
          append_uniform_decls_from_text (d, kept_decls, members, offset_inout);
        }
    }

  authority = _cogl_pipeline_get_authority (pipeline,
                                            COGL_PIPELINE_STATE_VERTEX_SNIPPETS);
  if (authority && authority->has_big_state && authority->big_state)
    {
      for (l = authority->big_state->vertex_snippets.entries; l; l = l->next)
        {
          CoglSnippet *snippet = l->data;
          CoglSnippetHook hook = cogl_snippet_get_hook (snippet);
          const char *d;

          if (hook != COGL_SNIPPET_HOOK_VERTEX &&
              hook != COGL_SNIPPET_HOOK_VERTEX_TRANSFORM &&
              hook != COGL_SNIPPET_HOOK_VERTEX_GLOBALS &&
              hook != COGL_SNIPPET_HOOK_POINT_SIZE)
            continue;

          d = cogl_snippet_get_declarations (snippet);
          append_uniform_decls_from_text (d, kept_decls, members, offset_inout);
        }
    }

  cogl_vk_pipeline_collect_layers (pipeline, &layers, &n_layers);
  for (i = 0; i < n_layers; i++)
    {
      CoglPipelineLayer *layer = layers[i];
      CoglPipelineLayer *layer_auth;
      GList *ll;

      if (!layer)
        continue;
      layer_auth = _cogl_pipeline_layer_get_authority
                     (layer, COGL_PIPELINE_LAYER_STATE_FRAGMENT_SNIPPETS);
      if (layer_auth && layer_auth->has_big_state && layer_auth->big_state)
        {
          for (ll = layer_auth->big_state->fragment_snippets.entries; ll;
               ll = ll->next)
            {
              CoglSnippet *snippet = ll->data;
              CoglSnippetHook hook = cogl_snippet_get_hook (snippet);
              const char *d;

              if (hook != COGL_SNIPPET_HOOK_LAYER_FRAGMENT &&
                  hook != COGL_SNIPPET_HOOK_TEXTURE_LOOKUP)
                continue;

              d = cogl_snippet_get_declarations (snippet);
              append_uniform_decls_from_text (d, kept_decls, members,
                                              offset_inout);
            }
        }

      layer_auth = _cogl_pipeline_layer_get_authority
                     (layer, COGL_PIPELINE_LAYER_STATE_VERTEX_SNIPPETS);
      if (layer_auth && layer_auth->has_big_state && layer_auth->big_state)
        {
          for (ll = layer_auth->big_state->vertex_snippets.entries; ll;
               ll = ll->next)
            {
              CoglSnippet *snippet = ll->data;
              const char *d;

              if (cogl_snippet_get_hook (snippet) !=
                  COGL_SNIPPET_HOOK_TEXTURE_COORD_TRANSFORM)
                continue;

              d = cogl_snippet_get_declarations (snippet);
              append_uniform_decls_from_text (d, kept_decls, members,
                                              offset_inout);
            }
        }
    }
}

/* Matrices occupy dense texture units, regardless of logical layer indices. */
static int
texture_matrix_count (CoglPipeline *pipeline)
{
  return MAX (cogl_pipeline_get_n_layers (pipeline), 1);
}

#define COGL_VK_UBO_FLIP_VECTOR_BYTES 16

static void
append_texture_matrix_ubo_prefix (GString      *ubo_glsl_out,
                                  GString      *defines_out,
                                  CoglPipeline *pipeline)
{
  int i;
  g_autofree CoglPipelineLayer **layers = NULL;
  int n_layers = 0;

  if (ubo_glsl_out)
    {
      g_string_append_printf (ubo_glsl_out,
                              "layout(set = 0, binding = 1, std140) "
                              "uniform CoglSnippetUBO {\n"
                              "  mat4 cogl_texture_matrix[%d];\n"
                              "  vec4 _cogl_flip_vector;\n",
                              texture_matrix_count (pipeline));
    }
  if (defines_out)
    {
      /* Emit each cogl_texture_matrixN exactly once. A prior default loop
       * (i→i) plus layer remap (idx→unit) made shaderc reject
       * "Macro redefined; different substitutions" (pipeline-shader-state).
       */
      g_autofree gboolean *defined =
        g_new0 (gboolean, texture_matrix_count (pipeline));
      cogl_vk_pipeline_collect_layers (pipeline, &layers, &n_layers);
      for (i = 0; i < n_layers; i++)
        {
          CoglPipelineLayer *layer = layers[i];
          int idx = layer ? layer->index : i;
          int unit = layer ? _cogl_pipeline_layer_get_unit_index (layer) : i;

          if (idx < 0)
            continue;
          /* Sparse logical indices are not UBO slots. Map them to dense
           * units without redefining any of the aliases below.
           */
          if (idx >= texture_matrix_count (pipeline))
            {
              if (unit < 0 || unit >= texture_matrix_count (pipeline))
                continue;
              g_string_append_printf (defines_out,
                                      "#define cogl_texture_matrix%i "
                                      "snip.cogl_texture_matrix[%i]\n",
                                      idx, unit);
              cogl_vk_debug_log (
                "glsl",
                "texmatrix #define cogl_texture_matrix%d → snip[%d] "
                "(sparse layer_i=%d)",
                idx, unit, i);
              continue;
            }
          if (defined[idx])
            {
              cogl_vk_debug_log (
                "glsl",
                "texmatrix skip duplicate #define matrix%d "
                "(already set; want unit=%d)",
                idx, unit);
              continue;
            }
          g_string_append_printf (defines_out,
                                  "#define cogl_texture_matrix%i "
                                  "snip.cogl_texture_matrix[%i]\n",
                                  idx, unit);
          defined[idx] = TRUE;
          cogl_vk_debug_log (
            "glsl",
            "texmatrix #define cogl_texture_matrix%d → snip[%d] "
            "(layer_i=%d)",
            idx, unit, i);
        }
      for (i = 0; i < texture_matrix_count (pipeline); i++)
        {
          if (defined[i])
            continue;
          g_string_append_printf (defines_out,
                                  "#define cogl_texture_matrix%i "
                                  "snip.cogl_texture_matrix[%i]\n",
                                  i, i);
        }
      g_string_append (defines_out,
                       "#define _cogl_flip_vector snip._cogl_flip_vector\n");
    }
}

static void
write_texture_matrices_to_ubo (uint8_t      *base,
                               CoglPipeline *pipeline)
{
  g_autofree CoglPipelineLayer **layers = NULL;
  int n_layers = 0;
  int i;
  graphene_matrix_t identity;

  graphene_matrix_init_identity (&identity);

  /* Default all slots to identity, then overwrite used units. */
  for (i = 0; i < texture_matrix_count (pipeline); i++)
    graphene_matrix_to_float (&identity, (float *) (base + (size_t) i * 64));

  cogl_vk_pipeline_collect_layers (pipeline, &layers, &n_layers);
  for (i = 0; i < n_layers; i++)
    {
      CoglPipelineLayer *layer = layers[i];
      int idx = layer ? layer->index : i;
      int unit = layer ? _cogl_pipeline_layer_get_unit_index (layer) : i;
      const graphene_matrix_t *matrix;

      if (unit < 0 || unit >= texture_matrix_count (pipeline))
        continue;
      matrix = _cogl_pipeline_get_layer_matrix (pipeline, idx);
      if (!matrix)
        matrix = &identity;
      graphene_matrix_to_float (matrix,
                                (float *) (base + (size_t) unit * 64));
    }
}

static void
write_flip_vector_to_ubo (uint8_t      *base,
                          CoglPipeline *pipeline)
{
  CoglContext *ctx;
  CoglFramebuffer *fb;
  gboolean needs_flip = FALSE;
  static const float do_flip[4] = { 1.0f, -1.0f, 1.0f, 1.0f };
  static const float dont_flip[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
  const float *v = dont_flip;

  if (!pipeline || !base)
    return;

  /* Only when assemble injects `*= _cogl_flip_vector`. No-snippet paths bake
   * y_flip into MVP/projection instead — keep identity here to avoid double.
   */
  if (_cogl_pipeline_has_vertex_snippets (pipeline))
    {
      ctx = pipeline->context;
      fb = ctx ? cogl_context_get_current_draw_buffer (ctx) : NULL;
      if (fb)
        needs_flip = cogl_framebuffer_is_y_flipped (fb);
    }
  v = needs_flip ? do_flip : dont_flip;
  memcpy (base + ((size_t) texture_matrix_count (pipeline) * 64), v, COGL_VK_UBO_FLIP_VECTOR_BYTES);
  cogl_vk_debug_log (
    "vbo",
    "flip_vector UBO needs_flip=%d vec=(%.1f,%.1f,%.1f,%.1f)",
    needs_flip, v[0], v[1], v[2], v[3]);
}

static void
write_boxed_to_ubo (uint8_t               *base,
                    const CoglVkUboMember *member,
                    const CoglBoxedValue  *bv)
{
  size_t align, element_size;
  size_t stride;
  int count;

  if (!bv || bv->type == COGL_BOXED_NONE)
    return;

  if (!std140_type_info (member->glsl_type, &align, &element_size))
    return;

  stride = member->array_len > 0 ? std140_align_up (element_size, 16)
                                 : element_size;
  count = MIN (bv->count, member->array_len > 0 ? member->array_len : 1);

  /* An indexed/partial upload only changes the elements it supplies. */
  for (int i = 0; i < count; i++)
    {
      uint8_t *dst = base + member->offset + i * stride;

      if (bv->type == COGL_BOXED_FLOAT || bv->type == COGL_BOXED_INT)
        {
          const void *src;
          size_t source_size = bv->size * sizeof (float);

          if (bv->type == COGL_BOXED_FLOAT)
            src = bv->count > 1 ? bv->v.float_array : bv->v.float_value;
          else
            src = bv->count > 1 ? bv->v.int_array : bv->v.int_value;
          if (src)
            memcpy (dst, (const uint8_t *) src + i * source_size,
                    MIN (source_size, element_size));
        }
      else if (bv->type == COGL_BOXED_MATRIX)
        {
          const float *src = bv->count > 1 ? bv->v.float_array : bv->v.matrix;

          if (src && bv->size >= 2 && bv->size <= 4 &&
              element_size == (size_t) bv->size * 16)
            {
              src += i * bv->size * bv->size;
              for (int column = 0; column < bv->size; column++)
                memcpy (dst + column * 16, src + column * bv->size,
                        bv->size * sizeof (float));
            }
        }
    }
}

#define LAYER_CONSTANT_PREFIX "cogl_layer_const"

static void
add_layer_constant_members (CoglPipeline *pipeline,
                            GArray       *members,
                            size_t       *offset_inout)
{
  g_autofree CoglPipelineLayer **layers = NULL;
  int n_layers = 0;

  cogl_vk_pipeline_collect_layers (pipeline, &layers, &n_layers);
  for (int i = 0; i < n_layers; i++)
    {
      CoglVkUboMember m = { 0 };

      if (!layers[i])
        continue;
      *offset_inout = std140_align_up (*offset_inout, 16);
      m.name = g_strdup_printf (LAYER_CONSTANT_PREFIX "%d", layers[i]->index);
      m.glsl_type = g_strdup ("vec4");
      m.offset = *offset_inout;
      m.size = 16;
      g_array_append_val (members, m);
      *offset_inout += 16;
    }
}

gboolean
cogl_vk_pipeline_pack_uniforms (CoglVkPipelineCache  *cache,
                                CoglPipeline         *pipeline,
                                GString              *decls_inout,
                                GString              *ubo_glsl_out,
                                GString              *defines_out,
                                GError              **error)
{
  COGL_VK_PROFILE_SCOPE ("cogl_vk_pipeline_pack_uniforms");
  g_autoptr (GArray) members = NULL;
  g_autoptr (GString) kept = NULL;
  size_t offset = (size_t) texture_matrix_count (pipeline) * 64 + 16;
  unsigned int i;

  g_return_val_if_fail (pipeline != NULL, FALSE);

  if (cogl_pipeline_get_n_layers (pipeline) > cogl_vk_pipeline_get_max_layers (pipeline))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Vulkan pipeline needs %d layers; device supports %d",
                   cogl_pipeline_get_n_layers (pipeline),
                   cogl_vk_pipeline_get_max_layers (pipeline));
      return FALSE;
    }

  members = g_array_new (FALSE, TRUE, sizeof (CoglVkUboMember));
  g_array_set_clear_func (members, ubo_member_clear);
  add_layer_constant_members (pipeline, members, &offset);

  if (decls_inout)
    {
      kept = g_string_new (NULL);
      append_uniform_decls_from_text (decls_inout->str, kept, members, &offset);
      g_string_assign (decls_inout, kept->str);
    }
  else
    {
      collect_snippet_uniform_decls (pipeline, NULL, members, &offset);
    }

  if (ubo_glsl_out || defines_out)
    {
      if (ubo_glsl_out)
        g_string_truncate (ubo_glsl_out, 0);
      if (defines_out)
        g_string_truncate (defines_out, 0);

      append_texture_matrix_ubo_prefix (ubo_glsl_out, defines_out, pipeline);

      if (ubo_glsl_out)
        {
          for (i = 0; i < members->len; i++)
            {
              CoglVkUboMember *m =
                &g_array_index (members, CoglVkUboMember, i);
              if (m->array_len > 0)
                g_string_append_printf (ubo_glsl_out, "  %s %s[%d];\n",
                                        m->glsl_type, m->name, m->array_len);
              else
                g_string_append_printf (ubo_glsl_out, "  %s %s;\n",
                                        m->glsl_type, m->name);
            }
          g_string_append (ubo_glsl_out, "} snip;\n");
        }
      if (defines_out)
        {
          for (i = 0; i < members->len; i++)
            {
              CoglVkUboMember *m =
                &g_array_index (members, CoglVkUboMember, i);
              g_string_append_printf (defines_out,
                                      "#define %s snip.%s\n",
                                      m->name, m->name);
            }
        }
    }

  if (cache && cache->uniform_mapped)
    {
      CoglContext *ctx = pipeline->context;
      int n_names = cogl_context_get_n_uniform_names (ctx);
      GPtrArray *names = cogl_context_get_uniform_names (ctx);
      const CoglBoxedValue **values;
      uint8_t *base = cache->uniform_mapped;

      if (offset > MIN (cache->uniform_size, COGL_VK_UBO_SLICE))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "snippet UBO needs %zu bytes (limit %zu)",
                       offset, MIN (cache->uniform_size, COGL_VK_UBO_SLICE));
          return FALSE;
        }

      values = g_alloca (sizeof (const CoglBoxedValue *) * n_names);
      cogl_vk_get_all_uniform_values (pipeline, values);

      /* Overlay driver set_uniform pending over pipeline overrides. */
      {
        CoglDriver *drv = cogl_context_get_driver (ctx);
        if (COGL_IS_DRIVER_VK (drv))
          {
            CoglDriverVk *drv_vk = COGL_DRIVER_VK (drv);
            int u;
            for (u = 0; u < n_names; u++)
              {
                const CoglBoxedValue *pending =
                  cogl_driver_vk_peek_pending_uniform (drv_vk, u);
                if (pending)
                  values[u] = pending;
              }
          }
      }

      memset (base, 0, MIN (offset, cache->uniform_size));
      write_texture_matrices_to_ubo (base, pipeline);
      write_flip_vector_to_ubo (base, pipeline);

      for (i = 0; i < members->len; i++)
        {
          CoglVkUboMember *m = &g_array_index (members, CoglVkUboMember, i);
          int u;

          if (g_str_has_prefix (m->name, LAYER_CONSTANT_PREFIX))
            {
              float constant[4];

              _cogl_pipeline_get_layer_combine_constant (
                pipeline, atoi (m->name + strlen (LAYER_CONSTANT_PREFIX)),
                constant);
              memcpy (base + m->offset, constant, sizeof (constant));
              continue;
            }

          /* Whole-array upload first so a later name[i] is not wiped. */
          for (u = 0; u < n_names; u++)
            {
              const char *uname = g_ptr_array_index (names, u);

              if (uname && strcmp (uname, m->name) == 0)
                write_boxed_to_ubo (base, m, values[u]);
            }
          if (g_getenv ("COGL_VK_UBO_TRACE") && m->name)
            {
              float fv = 0.f;
              if (m->offset + 4 <= cache->uniform_size)
                memcpy (&fv, base + m->offset, sizeof (fv));
              g_printerr ("UBO member %s type=%s off=%zu size=%zu f0=%g\n",
                          m->name, m->glsl_type ? m->glsl_type : "?",
                          m->offset, m->size, fv);
            }
          if (m->array_len > 0)
            {
              for (u = 0; u < n_names; u++)
                {
                  const char *uname = g_ptr_array_index (names, u);
                  int elem = -1;
                  size_t elem_size = 0, align = 0;
                  size_t stride;
                  CoglVkUboMember one;

                  if (!uname || !values[u])
                    continue;
                  if (!g_str_has_prefix (uname, m->name) ||
                      uname[strlen (m->name)] != '[' ||
                      sscanf (uname + strlen (m->name), "[%d]", &elem) != 1 ||
                      elem < 0 || elem >= m->array_len)
                    continue;
                  if (!std140_type_info (m->glsl_type, &align, &elem_size))
                    continue;
                  stride = std140_align_up (elem_size, 16);
                  one = *m;
                  one.offset = m->offset + (size_t) elem * stride;
                  one.array_len = m->array_len - elem;
                  one.size = stride * one.array_len;
                  write_boxed_to_ubo (base, &one, values[u]);
                }
            }
        }
    }

  if (cache)
    cache->uniform_packed = offset;

  return TRUE;
}

char *
cogl_vk_pipeline_inject_ubo (const char *glsl,
                             const char *ubo_glsl,
                             const char *defines)
{
  static const char marker[] = "/* COGL_VK_UBO_SLOT */\n";
  GString *out;
  const char *at;

  g_return_val_if_fail (glsl != NULL, NULL);

  at = strstr (glsl, marker);
  out = g_string_new (NULL);
  if (at)
    {
      g_string_append_len (out, glsl, at - glsl);
      g_assert (ubo_glsl && *ubo_glsl);
      g_string_append (out, ubo_glsl);
      if (defines && *defines)
        g_string_append (out, defines);
      g_string_append (out, at + strlen (marker));
    }
  else
    {
      g_string_append (out, glsl);
      if (ubo_glsl && *ubo_glsl)
        g_string_append (out, ubo_glsl);
      if (defines && *defines)
        g_string_append (out, defines);
    }
  return g_string_free (out, FALSE);
}

gboolean
cogl_vk_pipeline_alloc_cold_ds (CoglVkPipelineCache *cache,
                                VkDescriptorSet     *out_ds,
                                GError             **error)
{
  VkDescriptorSetAllocateInfo info = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .descriptorPool = cache->descriptor_pool,
    .descriptorSetCount = 1,
    .pSetLayouts = &cache->ds_layout_cold,
  };

  g_return_val_if_fail (cache && out_ds, FALSE);

  if (cache->ds_layout_cold == VK_NULL_HANDLE)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "cold descriptor layout unavailable");
      return FALSE;
    }

  if (cache->ds_free_cold && cache->ds_free_cold->len > 0)
    {
      *out_ds = g_array_index (cache->ds_free_cold, VkDescriptorSet,
                               cache->ds_free_cold->len - 1);
      g_array_set_size (cache->ds_free_cold, cache->ds_free_cold->len - 1);
      return TRUE;
    }

  if (!cogl_vk_pipeline_alloc_from_pool (cache, &info, out_ds, FALSE,
                                         "vkAllocateDescriptorSets (cold) failed",
                                         error))
    return FALSE;
  return TRUE;
}

void
cogl_vk_pipeline_free_cold_ds (CoglVkPipelineCache *cache,
                               VkDescriptorSet      ds)
{
  /* Do not recycle. vkUpdateDescriptorSets is not captured in the command
   * buffer — a reused set would make every earlier draw in this pass see
   * the latest UBO slice. One set per cold draw until the pool is reset.
   */
  (void) cache;
  (void) ds;
}

/* Copy the pack at byte 0 into a 256-aligned slice. vkCmdBindDescriptorSets
 * records the dynamic offset, so later packs cannot overwrite this draw.
 */
static gboolean
rotate_uniform_buffer (CoglVkPipelineCache *cache,
                       GError             **error)
{
  CoglVkDevice *device = cache->device;
  CoglVkPass *pass = device->owner ? device->owner->current_pass : NULL;
  CoglVkTransientBuffer old = { cache->uniform_buffer, cache->uniform_memory };
  CoglVkTransientBuffer next = { 0 };
  VkBufferCreateInfo info = {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size = cache->uniform_size,
    .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
  };
  VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
  VkMemoryRequirements requirements;
  VkPhysicalDeviceMemoryProperties properties;
  const VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VkResult res;
  void *mapped;
  uint32_t i;

  if (!pass || !pass->fence)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Uniform buffer rotation requires a recording pass");
      return FALSE;
    }
  res = vkCreateBuffer (device->device, &info, NULL, &next.buffer);
  if (res != VK_SUCCESS)
    goto fail;
  vkGetBufferMemoryRequirements (device->device, next.buffer, &requirements);
  vkGetPhysicalDeviceMemoryProperties (device->physical_device, &properties);
  for (i = 0; i < properties.memoryTypeCount; i++)
    if ((requirements.memoryTypeBits & (1u << i)) &&
        (properties.memoryTypes[i].propertyFlags & flags) == flags)
      break;
  if (i == properties.memoryTypeCount)
    {
      res = VK_ERROR_FEATURE_NOT_PRESENT;
      goto fail;
    }
  alloc.memoryTypeIndex = i;
  alloc.allocationSize = requirements.size;
  res = vkAllocateMemory (device->device, &alloc, NULL, &next.memory);
  if (res != VK_SUCCESS)
    goto fail;
  res = vkBindBufferMemory (device->device, next.buffer, next.memory, 0);
  if (res != VK_SUCCESS)
    goto fail;
  res = vkMapMemory (device->device, next.memory, 0, cache->uniform_size, 0, &mapped);
  if (res != VK_SUCCESS)
    goto fail;

  memset (mapped, 0, cache->uniform_size);
  memcpy (mapped, cache->uniform_mapped, cache->uniform_packed);
  vkUnmapMemory (device->device, old.memory);
  if (!pass->fence->transient_buffers)
    pass->fence->transient_buffers =
      g_array_new (FALSE, FALSE, sizeof (CoglVkTransientBuffer));
  g_array_append_val (pass->fence->transient_buffers, old);
  cache->uniform_buffer = next.buffer;
  cache->uniform_memory = next.memory;
  cache->uniform_mapped = mapped;
  cache->uniform_bump = COGL_VK_UBO_SLICE;
  return TRUE;

fail:
  if (next.buffer != VK_NULL_HANDLE)
    vkDestroyBuffer (device->device, next.buffer, NULL);
  if (next.memory != VK_NULL_HANDLE)
    vkFreeMemory (device->device, next.memory, NULL);
  cogl_vk_device_note_vk_result (device, res);
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "Uniform buffer rotation failed (%d)", (int) res);
  return FALSE;
}

static gboolean
cogl_vk_pipeline_snapshot_uniforms (CoglVkPipelineCache *cache,
                                    GError             **error)
{
  size_t used;
  size_t dst;
  const size_t align = 256;

  if (!cache || !cache->uniform_mapped)
    return FALSE;

  if (cache->uniform_packed > COGL_VK_UBO_SLICE ||
      cache->uniform_size < 2 * COGL_VK_UBO_SLICE)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Uniform snapshot exceeds buffer slice capacity");
      return FALSE;
    }

  used = cache->uniform_packed;
  if (used < 16)
    used = 16;

  /* Byte 0 is the staging pack. Every recorded draw reads a later slice
   * so the next memset at 0 cannot clobber it.
   */
  if (cache->uniform_bump < COGL_VK_UBO_SLICE)
    cache->uniform_bump = COGL_VK_UBO_SLICE;

  dst = (cache->uniform_bump + (align - 1)) & ~(size_t) (align - 1);
  if (dst + COGL_VK_UBO_SLICE > cache->uniform_size)
    {
      if (!rotate_uniform_buffer (cache, error))
        return FALSE;
      dst = COGL_VK_UBO_SLICE;
    }

  memcpy ((char *) cache->uniform_mapped + dst,
          cache->uniform_mapped, used);

  cache->uniform_dyn_offset = (uint32_t) dst;
  cache->uniform_bump = dst + ((used + (align - 1)) & ~(size_t) (align - 1));
  return TRUE;
}

void
cogl_vk_pipeline_update_cold_ds (CoglVkPipelineCache *cache,
                                 VkDescriptorSet      ds,
                                 int                  n_layers,
                                 const VkImageView   *views,
                                 const VkSampler     *samplers)
{
  int max_layers = max_layers_for_limits (&cache->device->limits);
  g_autofree VkDescriptorImageInfo *image_infos =
    g_new0 (VkDescriptorImageInfo, max_layers);
  VkDescriptorBufferInfo buffer_info = {
    .buffer = cache->uniform_buffer,
    .offset = cache->uniform_dyn_offset,
    .range = COGL_VK_UBO_SLICE,
  };
  VkWriteDescriptorSet writes[2];
  int i;
  int n;

  g_return_if_fail (cache && views && samplers);
  n = n_layers > 0 ? n_layers : 1;
  g_return_if_fail (n <= max_layers);

  for (i = 0; i < max_layers; i++)
    {
      int src_i = (i < n) ? i : 0;
      image_infos[i].sampler =
        samplers[src_i] != VK_NULL_HANDLE ? samplers[src_i] : cache->sampler;
      image_infos[i].imageView = views[src_i];
      image_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

  writes[0] = (VkWriteDescriptorSet) {
    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
    .dstSet = ds,
    .dstBinding = 0,
    .descriptorCount = max_layers,
    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    .pImageInfo = image_infos,
  };
  writes[1] = (VkWriteDescriptorSet) {
    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
    .dstSet = ds,
    .dstBinding = 1,
    .descriptorCount = 1,
    .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
    .pBufferInfo = &buffer_info,
  };

  vkUpdateDescriptorSets (cache->device->device, 2, writes, 0, NULL);
}

gboolean
cogl_vk_pipeline_prepare_cold_ds (CoglVkPipelineCache  *cache,
                                  CoglPipeline         *pipeline,
                                  CoglVkImage          *src_layer0,
                                  VkDescriptorSet      *out_ds,
                                  GError              **error)
{
  COGL_VK_PROFILE_SCOPE ("cogl_vk_pipeline_prepare_cold_ds");
  VkDescriptorSet cold_ds = VK_NULL_HANDLE;
  int max_layers = max_layers_for_limits (&cache->device->limits);
  g_autofree VkImageView *views = g_new0 (VkImageView, max_layers);
  g_autofree VkSampler *samplers = g_new0 (VkSampler, max_layers);
  VkSampler layer0_samp;
  int n_layers = 1;
  int li;
  g_autofree CoglPipelineLayer **layers = NULL;
  int n_collected = 0;

  g_return_val_if_fail (cache && src_layer0 && out_ds, FALSE);

  layer0_samp = cache->sampler;
  if (pipeline)
    {
      CoglDriver *drv = cogl_context_get_driver (pipeline->context);

      cogl_vk_pipeline_collect_layers (pipeline, &layers, &n_collected);
      if (COGL_IS_DRIVER_VK (drv) && n_collected > 0 && layers[0])
        {
          const CoglSamplerCacheEntry *entry =
            _cogl_pipeline_layer_get_sampler_state (layers[0]);
          layer0_samp =
            cogl_driver_vk_peek_sampler (COGL_DRIVER_VK (drv), entry);
        }
    }

  if (!cogl_vk_pipeline_pack_uniforms (cache, pipeline, NULL, NULL, NULL,
                                       error))
    return FALSE;
  if (!cogl_vk_pipeline_snapshot_uniforms (cache, error))
    return FALSE;
  if (!cogl_vk_pipeline_alloc_cold_ds (cache, &cold_ds, error))
    return FALSE;

  views[0] = src_layer0->view;
  samplers[0] = layer0_samp;
  if (pipeline)
    {
      int want = n_collected;
      CoglDriver *drv = cogl_context_get_driver (pipeline->context);

      for (li = 0; li < want; li++)
        {
          CoglPipelineLayer *layer = layers[li];
          CoglTexture *tex = layer
            ? _cogl_pipeline_layer_get_texture (layer) : NULL;

          views[li] = src_layer0->view;
          samplers[li] = layer0_samp;
          if (tex)
            {
              CoglTexture2DVk *tex_vk = cogl_texture_2d_vk_resolve (tex);

              if (tex_vk)
                {
                  CoglVkImage *img = cogl_texture_2d_vk_peek_image (tex_vk);
                  if (img && img->is_ycbcr)
                    img = img->cold_rgba;
                  if (img && img->view != VK_NULL_HANDLE)
                    views[li] = img->view;
                }
            }
          else if (li > 0)
            {
              /* GL samples the opaque-white default texture when a layer
               * has no texture (the shaped-texture mask slot). Reusing
               * layer 0 here multiplies the window by a second sample.
               */
              if (!cogl_vk_pipeline_ensure_dummy_white (cache, error))
                return FALSE;
              if (cache->dummy_white &&
                  cache->dummy_white->view != VK_NULL_HANDLE)
                views[li] = cache->dummy_white->view;
            }
          if (layer && COGL_IS_DRIVER_VK (drv))
            {
              const CoglSamplerCacheEntry *entry =
                _cogl_pipeline_layer_get_sampler_state (layer);
              samplers[li] =
                cogl_driver_vk_peek_sampler (COGL_DRIVER_VK (drv), entry);
            }
        }
      if (want > 0)
        n_layers = want;
    }
  for (li = n_layers; li < max_layers; li++)
    {
      views[li] = views[0];
      samplers[li] = samplers[0];
    }
  cogl_vk_pipeline_update_cold_ds (cache, cold_ds, n_layers, views, samplers);
  *out_ds = cold_ds;
  return TRUE;
}

gboolean
cogl_vk_pipeline_ensure_dummy_white (CoglVkPipelineCache  *cache,
                                     GError              **error)
{
  static const uint8_t white[4] = { 0xff, 0xff, 0xff, 0xff };
  CoglVkDevice *device;
  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory staging_mem = VK_NULL_HANDLE;
  VkCommandPoolCreateInfo pool_info = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
  };
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkCommandBufferAllocateInfo alloc_cmd = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1,
  };
  VkCommandBufferBeginInfo begin = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  VkBufferCreateInfo buf_info = {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size = sizeof (white),
    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
  };
  VkMemoryRequirements reqs;
  VkMemoryAllocateInfo mai = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
  };
  VkPhysicalDeviceMemoryProperties mp;
  VkBufferImageCopy region = {0};
  VkSubmitInfo submit = {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount = 1,
  };
  void *mapped = NULL;
  uint32_t mi;
  VkResult res;

  g_return_val_if_fail (cache && cache->device, FALSE);
  device = cache->device;

  /* Host-visible LINEAR white was sampled by solid VBO (no layers) and
   * device-lost inside vkQueueWaitIdle on Intel (custom-attributes 245).
   * A/B: COGL_VK_AB_NO_DUMMY_WHITE → WaitIdle done. Use OPTIMAL 1×1.
   */
  if (!cache->dummy_white)
    {
      cache->dummy_white =
        cogl_vk_image_new_optimal (device,
                                   1, 1,
                                   VK_FORMAT_R8G8B8A8_UNORM,
                                   COGL_PIXEL_FORMAT_RGBA_8888_PRE,
                                   VK_IMAGE_USAGE_SAMPLED_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   error);
      if (!cache->dummy_white)
        return FALSE;

      res = vkCreateBuffer (device->device, &buf_info, NULL, &staging);
      if (res != VK_SUCCESS)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "dummy_white staging buffer failed (%d)", (int) res);
          goto fail_upload;
        }
      vkGetBufferMemoryRequirements (device->device, staging, &reqs);
      vkGetPhysicalDeviceMemoryProperties (device->physical_device, &mp);
      mai.allocationSize = reqs.size;
      for (mi = 0; mi < mp.memoryTypeCount; mi++)
        {
          if ((reqs.memoryTypeBits & (1u << mi)) &&
              (mp.memoryTypes[mi].propertyFlags &
               (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
              (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            {
              mai.memoryTypeIndex = mi;
              break;
            }
        }
      if (mi >= mp.memoryTypeCount ||
          vkAllocateMemory (device->device, &mai, NULL, &staging_mem) !=
          VK_SUCCESS ||
          vkBindBufferMemory (device->device, staging, staging_mem, 0) !=
          VK_SUCCESS ||
          vkMapMemory (device->device, staging_mem, 0, sizeof (white), 0,
                       &mapped) != VK_SUCCESS)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "dummy_white staging alloc/map failed");
          goto fail_upload;
        }
      memcpy (mapped, white, sizeof (white));
      vkUnmapMemory (device->device, staging_mem);

      pool_info.queueFamilyIndex = device->graphics_queue_family;
      if (vkCreateCommandPool (device->device, &pool_info, NULL, &pool) !=
          VK_SUCCESS)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "dummy_white cmd pool failed");
          goto fail_upload;
        }
      alloc_cmd.commandPool = pool;
      if (vkAllocateCommandBuffers (device->device, &alloc_cmd, &cmd) !=
          VK_SUCCESS ||
          vkBeginCommandBuffer (cmd, &begin) != VK_SUCCESS)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "dummy_white cmd begin failed");
          goto fail_upload;
        }

      cogl_vk_image_cmd_barrier (cache->dummy_white, cmd,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, VK_ACCESS_TRANSFER_WRITE_BIT);
      region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      region.imageSubresource.layerCount = 1;
      region.imageExtent.width = 1;
      region.imageExtent.height = 1;
      region.imageExtent.depth = 1;
      vkCmdCopyBufferToImage (cmd, staging, cache->dummy_white->image,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              1, &region);
      cogl_vk_image_cmd_barrier (cache->dummy_white, cmd,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_ACCESS_TRANSFER_WRITE_BIT,
                                 VK_ACCESS_SHADER_READ_BIT);
      vkEndCommandBuffer (cmd);
      submit.pCommandBuffers = &cmd;
      if (vkQueueSubmit (device->graphics_queue, 1, &submit, VK_NULL_HANDLE) !=
          VK_SUCCESS ||
          vkQueueWaitIdle (device->graphics_queue) != VK_SUCCESS)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "dummy_white upload submit failed");
          goto fail_upload;
        }

      cogl_vk_debug_log ("vbo",
                         "dummy_white: OPTIMAL 1x1 uploaded (not host-linear)");

      vkDestroyCommandPool (device->device, pool, NULL);
      pool = VK_NULL_HANDLE;
      vkDestroyBuffer (device->device, staging, NULL);
      staging = VK_NULL_HANDLE;
      vkFreeMemory (device->device, staging_mem, NULL);
      staging_mem = VK_NULL_HANDLE;
    }

  /* Hot combined-image DS so solid VBO can use VBO_FRAG_TEX (texture*color).
   * With push descriptors the ds_layout is push-only and cannot back
   * vkAllocateDescriptorSets, so the solid path pushes dummy_white->view
   * directly instead of binding this shared set.
   */
  if (!cache->device->has_push_descriptor &&
      cache->dummy_white_ds == VK_NULL_HANDLE)
    {
      if (!cogl_vk_pipeline_alloc_texture_ds (cache, FALSE,
                                              &cache->dummy_white_ds, error))
        return FALSE;
      cogl_vk_pipeline_update_texture_ds (cache, cache->dummy_white_ds,
                                          cache->dummy_white->view,
                                          FALSE, VK_NULL_HANDLE);
    }

  return TRUE;

fail_upload:
  if (pool != VK_NULL_HANDLE)
    vkDestroyCommandPool (device->device, pool, NULL);
  if (staging != VK_NULL_HANDLE)
    vkDestroyBuffer (device->device, staging, NULL);
  if (staging_mem != VK_NULL_HANDLE)
    vkFreeMemory (device->device, staging_mem, NULL);
  g_clear_pointer (&cache->dummy_white, cogl_vk_image_free);
  return FALSE;
}

/* The shared cold descriptor array has ordinary samplers. Native YCbCr
 * views require immutable samplers instead. Normalize only this compatibility
 * path on the GPU, keeping floating point values for subsequent snippets.
 * External producers may change the image without changing its Cogl object,
 * so reuse allocation but refresh pixels on every draw. */
static gboolean
prepare_cold_image (CoglVkPass          *pass,
                    CoglVkPipelineCache *cache,
                    CoglVkImage         *image,
                    GError             **error)
{
  CoglVkRenderFormatSetup *setup;
  VkDescriptorSet ds;
  CoglVkVertPush vert;
  VkViewport saved_viewport = pass->viewport;
  gboolean saved_viewport_valid = pass->viewport_valid;
  gboolean saved_scissor_valid = pass->scissor_valid;
  gboolean ok;

  if (!image->is_ycbcr)
    return TRUE;
  cogl_vk_pass_end_render (pass);
  if (!image->cold_rgba)
    {
      image->cold_rgba = cogl_vk_image_new_optimal (
        cache->device, image->width, image->height,
        VK_FORMAT_R16G16B16A16_SFLOAT, COGL_PIXEL_FORMAT_RGBA_FP_16161616,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT, error);
      if (!image->cold_rgba)
        return FALSE;
    }
  setup = cogl_vk_pipeline_cache_get_format_setup (
    cache, image->cold_rgba->format, VK_FORMAT_UNDEFINED,
    VK_SAMPLE_COUNT_1_BIT, error);
  if (!setup || !cogl_vk_pipeline_alloc_texture_ds (
        cache, image->ycbcr_index + 1, &ds, error))
    return FALSE;
  cogl_vk_pipeline_update_texture_ds (cache, ds, image->view,
                                      image->ycbcr_index + 1, VK_NULL_HANDLE);
  pass->viewport_valid = FALSE;
  pass->scissor_valid = FALSE;
  cogl_vk_vert_push_ortho_uv_unit (&vert, NULL, NULL);
  ok = cogl_vk_cmd_draw_texture (pass, cache, setup, image->cold_rgba,
                                 image, ds, 1.f, TRUE, &vert,
                                 VK_NULL_HANDLE, NULL, error);
  cogl_vk_pass_end_render (pass);
  pass->viewport = saved_viewport;
  pass->viewport_valid = saved_viewport_valid;
  pass->scissor_valid = saved_scissor_valid;
  cogl_vk_pipeline_free_texture_ds (cache, image->ycbcr_index + 1, ds);
  return ok;
}

gboolean
cogl_vk_pipeline_prepare_cold_images (CoglVkPass          *pass,
                                      CoglVkPipelineCache *cache,
                                      CoglPipeline        *pipeline,
                                      CoglVkImage        **src_layer0,
                                      GError             **error)
{
  g_autofree CoglPipelineLayer **layers = NULL;
  CoglVkImage *first = *src_layer0;
  int n_layers = 0;

  if (!prepare_cold_image (pass, cache, first, error))
    return FALSE;
  if (pipeline)
    cogl_vk_pipeline_collect_layers (pipeline, &layers, &n_layers);
  for (int i = 0; i < n_layers; i++)
    {
      CoglTexture2DVk *texture = cogl_texture_2d_vk_resolve (
        _cogl_pipeline_layer_get_texture (layers[i]));
      CoglVkImage *image = texture ? cogl_texture_2d_vk_peek_image (texture) : NULL;
      if (image && image != first &&
          !prepare_cold_image (pass, cache, image, error))
        return FALSE;
    }
  if (first->is_ycbcr)
    *src_layer0 = first->cold_rgba;
  return TRUE;
}

static gboolean
layer_image_needs_barrier (CoglVkPass  *pass,
                           CoglVkImage *image)
{
  if (image->is_dmabuf)
    return !cogl_vk_pass_has_foreign_image (pass, image);
  return image->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
         !image->shader_read_synced;
}

gboolean
cogl_vk_pipeline_layer_textures_need_barrier (CoglVkPass   *pass,
                                              CoglPipeline *pipeline,
                                              CoglVkImage  *src_layer0,
                                              gboolean      use_cold)
{
  g_autofree CoglPipelineLayer **layers = NULL;
  int want = 0;

  if (layer_image_needs_barrier (pass, src_layer0))
    return TRUE;
  if (!pipeline)
    return FALSE;

  cogl_vk_pipeline_collect_layers (pipeline, &layers, &want);
  for (int li = 0; li < want; li++)
    {
      CoglTexture *tex = layers[li]
        ? _cogl_pipeline_layer_get_texture (layers[li]) : NULL;
      CoglTexture2DVk *tex_vk = cogl_texture_2d_vk_resolve (tex);
      CoglVkImage *img = tex_vk ? cogl_texture_2d_vk_peek_image (tex_vk) : NULL;

      if (use_cold && img && img->is_ycbcr)
        img = img->cold_rgba;
      if (img && img != src_layer0 && layer_image_needs_barrier (pass, img))
        return TRUE;
    }
  return FALSE;
}

void
cogl_vk_pipeline_barrier_layer_textures (CoglVkPass   *pass,
                                         CoglPipeline *pipeline,
                                         CoglVkImage  *src_layer0,
                                         gboolean      use_cold)
{
  COGL_VK_PROFILE_SCOPE ("cogl_vk_pipeline_barrier_layer_textures");
  g_autofree CoglPipelineLayer **layers = NULL;
  int want;
  int li;

  g_return_if_fail (pass && src_layer0);

  if (src_layer0->is_dmabuf)
    {
      /* Acquire once per pass; re-acquiring would use oldLayout=GENERAL while still
       * SHADER_READ_ONLY. */
      if (!cogl_vk_pass_has_foreign_image (pass, src_layer0))
        {
          VkImageLayout old = src_layer0->layout;
          cogl_vk_image_cmd_foreign_acquire (
            src_layer0,
            pass->command_buffer,
            pass->device->graphics_queue_family,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
          cogl_vk_pass_note_foreign_image (pass, src_layer0);
          cogl_vk_debug_barrier ("layer0-dmabuf", src_layer0, old,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 FALSE);
        }
    }
  else
    {
      VkImageLayout old = src_layer0->layout;
      gboolean host_src =
        (old == VK_IMAGE_LAYOUT_PREINITIALIZED ||
         old == VK_IMAGE_LAYOUT_UNDEFINED ||
         src_layer0->host_visible);

      /* Host-visible linear uploads leave PREINITIALIZED after CPU write.
       * Must use HOST_WRITE (texture-driver staging path already does).
       */
      if (host_src &&
          (old == VK_IMAGE_LAYOUT_PREINITIALIZED ||
           old == VK_IMAGE_LAYOUT_UNDEFINED))
        {
          cogl_vk_image_cmd_barrier (src_layer0,
                                     pass->command_buffer,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     VK_PIPELINE_STAGE_HOST_BIT |
                                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     VK_ACCESS_HOST_WRITE_BIT,
                                     VK_ACCESS_SHADER_READ_BIT);
        }
      else
        {
          cogl_vk_image_cmd_barrier (src_layer0,
                                     pass->command_buffer,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT |
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                     VK_PIPELINE_STAGE_HOST_BIT |
                                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     VK_ACCESS_TRANSFER_WRITE_BIT |
                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                     VK_ACCESS_HOST_WRITE_BIT |
                                     VK_ACCESS_MEMORY_WRITE_BIT,
                                     VK_ACCESS_SHADER_READ_BIT);
        }
      src_layer0->shader_read_synced = TRUE;
      cogl_vk_debug_barrier ("layer0", src_layer0, old,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             host_src);
    }

  if (!pipeline)
    return;

  cogl_vk_pipeline_collect_layers (pipeline, &layers, &want);
  for (li = 0; li < want; li++)
    {
      CoglPipelineLayer *layer = layers[li];
      CoglTexture *tex = layer
        ? _cogl_pipeline_layer_get_texture (layer) : NULL;
      CoglTexture2DVk *tex_vk = cogl_texture_2d_vk_resolve (tex);
      CoglVkImage *img;

      if (!tex_vk)
        continue;
      img = cogl_texture_2d_vk_peek_image (tex_vk);
      if (use_cold && img && img->is_ycbcr)
        img = img->cold_rgba;
      if (!img || img == src_layer0)
        continue;
      if (img->is_dmabuf)
        {
          if (!cogl_vk_pass_has_foreign_image (pass, img))
            {
              cogl_vk_image_cmd_foreign_acquire (
                img, pass->command_buffer,
                pass->device->graphics_queue_family,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
              cogl_vk_pass_note_foreign_image (pass, img);
            }
        }
      else
        {
          VkImageLayout old = img->layout;
          gboolean host_src =
            (old == VK_IMAGE_LAYOUT_PREINITIALIZED ||
             old == VK_IMAGE_LAYOUT_UNDEFINED);

          if (host_src)
            {
              cogl_vk_image_cmd_barrier (
                img, pass->command_buffer,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_HOST_BIT |
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_HOST_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT);
            }
          else
            {
              cogl_vk_image_cmd_barrier (
                img, pass->command_buffer,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT |
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                VK_PIPELINE_STAGE_HOST_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT |
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                VK_ACCESS_HOST_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT);
            }
          img->shader_read_synced = TRUE;
          cogl_vk_debug_barrier ("layerN", img, old,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 host_src);
        }
    }
}
