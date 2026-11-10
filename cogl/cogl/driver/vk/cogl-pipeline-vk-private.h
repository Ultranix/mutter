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

/* Vulkan graphics pipelines, render passes and descriptor resources for Cogl.
 * Render setups are cached by attachment format and sample count. NV12
 * sampling uses immutable samplers with explicit YCbCr conversion state.
 */

#pragma once

#include "cogl/driver/vk/cogl-device-vk-private.h"
#include "cogl/driver/vk/cogl-image-vk-private.h"
#include "cogl/driver/vk/cogl-pass-vk-private.h"

#include "cogl/cogl-framebuffer.h"
#include "cogl/cogl-pipeline.h"

#include <glib.h>
#include <graphene.h>
#include <vulkan/vulkan.h>

G_BEGIN_DECLS

int cogl_vk_pipeline_get_max_layers (CoglPipeline *pipeline);

typedef struct _CoglVkPipelineCache CoglVkPipelineCache;
typedef struct _CoglVkRenderFormatSetup CoglVkRenderFormatSetup;

/* Push-constant layout shared with the embedded vertex shader. */
typedef struct _CoglVkVertPush
{
  float mat4[4][4];
  float uv_off[2];
  float uv_size[2];
} CoglVkVertPush;

/* GL progend flip_uniform==-1: bake y_flip_matrix into projection when the
 * FB is y-flipped and the pipeline has no vertex snippets. Vertex-snippet
 * paths use UBO `_cogl_flip_vector` instead (do not bake — no double-flip).
 */
gboolean cogl_vk_should_bake_y_flip_projection (CoglFramebuffer *fb,
                                                CoglPipeline    *pipeline);
void cogl_vk_maybe_bake_y_flip_projection (CoglFramebuffer   *fb,
                                           CoglPipeline      *pipeline,
                                           graphene_matrix_t *projection);
/* modelview * (projection [× y_flip]); push applies GL→VK depth remap. */
void cogl_vk_fill_vert_push_mvp (CoglFramebuffer *fb,
                                 CoglPipeline    *pipeline,
                                 CoglVkVertPush  *out);
void cogl_vk_vert_push_set_matrix (CoglVkVertPush          *out,
                                   const graphene_matrix_t *matrix);
/* Unit [0,1]→NDC ortho; applies y_flip bake when appropriate. */
void cogl_vk_vert_push_ortho_uv_unit (CoglVkVertPush  *out,
                                      CoglFramebuffer *fb,
                                      CoglPipeline    *pipeline);
/* texture.frag wrap_s/wrap_t: 0=clamp-to-edge, 1=repeat, 2=mirrored. */
float cogl_vk_shader_wrap_mode (int wrap_enum);

#define COGL_VK_YCBCR_VARIANTS 6

struct _CoglVkRenderFormatSetup
{
  VkFormat format;
  /* VK_FORMAT_UNDEFINED → color-only RP; else packed DS attachment. */
  VkFormat ds_format;
  VkSampleCountFlagBits samples; /* 0 / 1 → single-sample */
  VkRenderPass render_pass;       /* loadOp=LOAD, defined layouts */
  /* First-use helpers (pipeline-compatible with render_pass): */
  VkRenderPass render_pass_clear;           /* color+DS CLEAR from UNDEFINED */
  VkRenderPass render_pass_clear_ds_only;   /* color LOAD + DS CLEAR; NULL if no DS */
  VkPipeline tex_pipe;          /* alpha blend (src-over) */
  VkPipeline tex_pipe_opaque;   /* blending disabled for opaque draws */
  VkPipeline tex_pipe_ycbcr[COGL_VK_YCBCR_VARIANTS];
  VkPipeline tex_pipe_ycbcr_opaque[COGL_VK_YCBCR_VARIANTS];
  VkPipeline quad_pipe;         /* solid color */
  /* Stencil-clip write PSOs (color writes off); only when ds_format set. */
  VkPipeline stencil_replace_pipe;
  VkPipeline stencil_incr_pipe;
  VkPipeline stencil_decr_pipe;
  CoglVkRenderFormatSetup *next;
};

struct _CoglVkPipelineCache
{
  CoglVkDevice *device; /* unowned */
  VkSampler sampler;
  VkDescriptorSetLayout ds_layout;
  VkDescriptorSetLayout ds_layout_push;
  VkPipelineLayout pipe_layout;
  /* Cold path (multi-layer + snippets + UBO):
   * set0 binding0 = sampler2D[N] (N derived from device limits),
   * binding1 = std140 UBO. Push constants alone cannot hold color-mgmt
   * matrices (Vulkan min maxPushConstantsSize=128; vert push uses 80).
   */
  VkDescriptorSetLayout ds_layout_cold;
  VkPipelineLayout pipe_layout_cold;
  VkBuffer uniform_buffer;
  VkDeviceMemory uniform_memory;
  void *uniform_mapped;
  size_t uniform_size;
  size_t uniform_bump;   /* next free slice; each cold draw snapshots */
  size_t uniform_packed; /* bytes packed at offset 0 by the last pack */
  uint32_t uniform_dyn_offset; /* recorded via UNIFORM_BUFFER_DYNAMIC */
  GArray *ds_free_cold; /* VkDescriptorSet (sampler+ubo) */
  /* White texture and descriptor used by solid vertex-buffer draws. */
  CoglVkImage *dummy_white;
  VkDescriptorSet dummy_white_ds;
  /* Immutable YCbCr samplers indexed by color matrix and range. */
  VkSamplerYcbcrConversion ycbcr_conversion[COGL_VK_YCBCR_VARIANTS];
  VkSampler ycbcr_sampler[COGL_VK_YCBCR_VARIANTS];
  VkDescriptorSetLayout ds_layout_ycbcr[COGL_VK_YCBCR_VARIANTS];
  VkPipelineLayout pipe_layout_ycbcr[COGL_VK_YCBCR_VARIANTS];
  VkShaderModule vert_module;
  VkShaderModule tex_frag_module;
  VkShaderModule quad_frag_module;
  VkDescriptorPool descriptor_pool;
  VkDescriptorPool texture_descriptor_pool;
  GArray *ds_retired; /* CoglVkRetiredDescriptor */
  /* Extra pools created after OUT_OF_POOL_MEMORY. Closed pools stay
   * alive until the submit that last referenced them has signaled.
   */
  GPtrArray *ds_pool_slots;
  GHashTable *ds_texture_owners; /* uint64 descriptor handle -> pool slot */
  /* Available descriptor sets that can be reused without pending GPU users. */
  GArray *ds_free;       /* VkDescriptorSet */
  GArray *ds_free_ycbcr; /* VkDescriptorSet */
  CoglVkRenderFormatSetup *format_setups;
  /* Hot SPIR-V tex/quad/ycbcr pipes keyed by blend/cull/depth/front.
   * Baked format_setup pipes stay the default src-over / no-cull fast path.
   */
  GHashTable *hot_extras_pipes; /* CoglVkHotExtrasKey → owns VkPipeline */
  /* Shader modules and pipelines compiled from generated Cogl GLSL. */
  struct _CoglVkSpirvCache *spirv_cache;
};

void cogl_vk_pipeline_cache_note_submit (CoglVkPipelineCache *cache,
                                         VkSemaphore          timeline,
                                         uint64_t             point);

CoglVkPipelineCache * cogl_vk_pipeline_cache_new (CoglVkDevice *device,
                                                  GError      **error);

void cogl_vk_pipeline_cache_free (CoglVkPipelineCache *cache);

COGL_EXPORT VkSamplerYcbcrConversion
cogl_vk_pipeline_cache_peek_ycbcr_conversion (CoglVkPipelineCache *cache,
                                              unsigned int         index);

CoglVkRenderFormatSetup *
cogl_vk_pipeline_cache_get_format_setup (CoglVkPipelineCache  *cache,
                                         VkFormat              format,
                                         VkFormat              ds_format,
                                         VkSampleCountFlagBits samples,
                                         GError              **error);

gboolean cogl_vk_pipeline_alloc_texture_ds (CoglVkPipelineCache *cache,
                                            unsigned int         ycbcr,
                                            VkDescriptorSet     *out_ds,
                                            GError             **error);

void cogl_vk_pipeline_free_texture_ds (CoglVkPipelineCache *cache,
                                       unsigned int         ycbcr,
                                       VkDescriptorSet      ds);

/* Non-immutable combined-image-sampler: writes sampler + view.
 * sampler == VK_NULL_HANDLE → cache default (or ycbcr immutable).
 */
void cogl_vk_pipeline_update_texture_ds (CoglVkPipelineCache *cache,
                                         VkDescriptorSet      ds,
                                         VkImageView          view,
                                         unsigned int         ycbcr,
                                         VkSampler            sampler);

/* Push the hot combined-image-sampler (binding 0) straight into the command
 * buffer via VK_KHR_push_descriptor — no set alloc/update/free. Only valid
 * when device->has_push_descriptor and the hot pipe_layout was created
 * push-enabled; callers use the alloc/update/bind path otherwise.
 */
void cogl_vk_pipeline_push_texture_ds (CoglVkPipelineCache *cache,
                                       VkCommandBuffer      cmd,
                                       VkImageView          view,
                                       VkSampler            sampler);

/* Ensure COLOR_ATTACHMENT (+ optional DS) layout + begin render pass (LOAD). */
gboolean cogl_vk_pass_begin_render (CoglVkPass              *pass,
                                    CoglVkPipelineCache     *cache,
                                    CoglVkImage             *color,
                                    CoglVkRenderFormatSetup *setup,
                                    uint32_t                 width,
                                    uint32_t                 height,
                                    GError                 **error);

void cogl_vk_pass_end_render (CoglVkPass *pass);

void cogl_vk_pass_apply_stencil_dynamic (CoglVkPass *pass,
                                         uint32_t    write_mask);

typedef enum
{
  COGL_VK_STENCIL_CLIP_OP_REPLACE,
  COGL_VK_STENCIL_CLIP_OP_INCR,
  COGL_VK_STENCIL_CLIP_OP_DECR,
} CoglVkStencilClipOp;

/* Solid quad with stencil write ops; color writes disabled. */
gboolean cogl_vk_cmd_draw_stencil_quad (CoglVkPass              *pass,
                                        CoglVkPipelineCache     *cache,
                                        CoglVkRenderFormatSetup *setup,
                                        CoglVkImage             *dst,
                                        const CoglVkVertPush    *vert_push,
                                        CoglVkStencilClipOp      op,
                                        GError                 **error);

/* Textured quad covering the destination with an orthographic transform.
 * If vert_push is NULL, uses unit UV + NDC ortho.
 */
gboolean cogl_vk_cmd_draw_texture (CoglVkPass              *pass,
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
                                   GError                 **error);

/* Optional extras for cold-path blend/cull/depth (NULL = hot defaults). */
typedef struct _CoglVkTexPipeExtras
{
  gboolean blend_enable;
  VkBlendFactor src_color;
  VkBlendFactor dst_color;
  VkBlendOp color_op;
  VkBlendFactor src_alpha;
  VkBlendFactor dst_alpha;
  VkBlendOp alpha_op;
  VkCullModeFlags cull_mode;
  VkFrontFace front_face;
  gboolean depth_test_enable;
  gboolean depth_write_enable;
  VkCompareOp depth_compare_op;
  /* When TRUE: stencil EQUAL/KEEP + dynamic compare/write/ref masks. */
  gboolean stencil_equal_keep;
  gboolean disable_color_writes;
  /* Stencil write presets for clip construction (mutually exclusive). */
  gboolean stencil_op_replace;
  gboolean stencil_op_incr;
  gboolean stencil_op_decr;
  /* VBO topology / MSAA / vertex input (NULL → empty / defaults). */
  VkSampleCountFlagBits samples; /* 0 → 1 */
  gboolean has_topology;
  VkPrimitiveTopology topology;
  const VkPipelineVertexInputStateCreateInfo *vertex_input;
  /* Declare VK_DYNAMIC_STATE_BLEND_CONSTANTS (cold/VBO always; or
   * when any blend factor is CONSTANT_*).
   */
  gboolean dynamic_blend_constants;
} CoglVkTexPipeExtras;

/* Compact fingerprint of PSO-affecting raster/blend/depth state.
 *
 * Depth *range* (near/far) is intentionally omitted: it is applied via
 * dynamic VkViewport.minDepth/maxDepth each draw / FB flush, so two
 * pipelines that differ only in range share a PSO. Depth test/write/compare
 * remain keyed because they are baked into VkPipelineDepthStencilState.
 */
typedef struct _CoglVkPipeStateKey
{
  uint8_t blend_enable;
  uint8_t depth_test_enable;
  uint8_t depth_write_enable;
  uint8_t stencil_equal_keep;
  uint8_t stencil_op_replace;
  uint8_t stencil_op_incr;
  uint8_t stencil_op_decr;
  uint8_t has_topology;
  uint8_t dynamic_blend_constants;
  uint8_t pad[3];
  VkBlendFactor src_color;
  VkBlendFactor dst_color;
  VkBlendOp color_op;
  VkBlendFactor src_alpha;
  VkBlendFactor dst_alpha;
  VkBlendOp alpha_op;
  VkCullModeFlags cull_mode;
  VkFrontFace front_face;
  VkCompareOp depth_compare_op;
  VkSampleCountFlagBits samples;
  VkPrimitiveTopology topology;
  /* VBO attribute presence/layout fingerprint; cold textured → 0. */
  uint32_t vertex_input_mask;
} CoglVkPipeStateKey;

/* Fill state key from extras (+ blend_enable / vertex_input_mask). */
void cogl_vk_pipe_state_key_from_extras (CoglVkPipeStateKey        *out,
                                         const CoglVkTexPipeExtras *extras,
                                         gboolean                   blend_enable,
                                         uint32_t                   vertex_input_mask);

/* Build key from pipeline + draw-time bits (samples / stencil / topology). */
void cogl_vk_pipe_state_key_from_pipeline (CoglVkPipeStateKey     *out,
                                           CoglPipeline           *pipeline,
                                           VkFrontFace             front_face,
                                           gboolean                blend_enable,
                                           VkSampleCountFlagBits   samples,
                                           gboolean                stencil_equal_keep,
                                           gboolean                has_topology,
                                           VkPrimitiveTopology     topology,
                                           uint32_t                vertex_input_mask);

unsigned int cogl_vk_pipe_state_key_hash (const CoglVkPipeStateKey *key);

gboolean cogl_vk_pipe_state_key_equal (const CoglVkPipeStateKey *a,
                                       const CoglVkPipeStateKey *b);

/* Append "|pso=..." fragment for string-keyed cold caches. */
void cogl_vk_pipe_state_key_append (GString                  *str,
                                    const CoglVkPipeStateKey *key);

/* Create a textured graphics pipeline from the supplied shader modules. */
gboolean cogl_vk_create_tex_pipeline (CoglVkPipelineCache       *cache,
                                      VkRenderPass               rp,
                                      VkShaderModule             vert,
                                      VkShaderModule             frag,
                                      VkPipelineLayout           layout,
                                      gboolean                   blend_enable,
                                      const CoglVkTexPipeExtras *extras,
                                      VkPipeline                *out_pipe,
                                      GError                   **error);

/* Fill extras from CoglPipeline (+ optional driver pending front-face).
 * Depth write is AND'd with driver pending_depth_write (GL flush parity).
 */
void cogl_vk_tex_pipe_extras_from_pipeline (CoglPipeline        *pipeline,
                                            VkFrontFace          front_face,
                                            CoglVkTexPipeExtras *out);

/* TRUE when baked format_setup hot pipes cannot represent @extras. */
gboolean cogl_vk_tex_pipe_extras_need_keyed_hot (const CoglVkTexPipeExtras *extras,
                                                 gboolean                   blend_enable);

/* Look up / create embedded-SPIR-V hot pipe with @extras.
 * kind: 0=tex, 1=tex_opaque, 2=ycbcr, 3=ycbcr_opaque, 4=quad.
 */
VkPipeline cogl_vk_pipeline_cache_get_hot_extras_pipe (CoglVkPipelineCache     *cache,
                                                       CoglVkRenderFormatSetup *setup,
                                                       CoglPipeline            *pipeline,
                                                       uint8_t                  kind,
                                                       unsigned int             ycbcr_index,
                                                       GError                 **error);

/* vkCmdSetBlendConstants from pipeline blend_constant (zeros if NULL). */
void cogl_vk_pass_apply_blend_constants (CoglVkPass   *pass,
                                         CoglPipeline *pipeline);

/* Apply pipeline depth range to dynamic viewport minDepth/maxDepth. */
void cogl_vk_pass_apply_depth_range (CoglVkPass   *pass,
                                     CoglPipeline *pipeline);

/* Cold-path UBO layout (binding 1, std140), always starts with:
 * mat4 cogl_texture_matrix[MAX (pipeline_layer_count, 1)];
 * then snippet uniforms. #defines map cogl_texture_matrixN → snip array.
 * decls_inout: strip uniform lines when non-NULL (compile-time).
 * ubo_glsl_out/defines_out: emit when non-NULL.
 * cache non-NULL: write layer matrices + override values into mapped UBO.
 */
gboolean cogl_vk_pipeline_pack_uniforms (CoglVkPipelineCache  *cache,
                                         CoglPipeline         *pipeline,
                                         GString              *decls_inout,
                                         GString              *ubo_glsl_out,
                                         GString              *defines_out,
                                         GError              **error);

/* Replace the COGL_VK_UBO_SLOT marker in assembled GLSL with UBO + defines. */
char * cogl_vk_pipeline_inject_ubo (const char *glsl,
                                    const char *ubo_glsl,
                                    const char *defines);

gboolean cogl_vk_pipeline_alloc_cold_ds (CoglVkPipelineCache *cache,
                                         VkDescriptorSet     *out_ds,
                                         GError             **error);

void cogl_vk_pipeline_free_cold_ds (CoglVkPipelineCache *cache,
                                    VkDescriptorSet      ds);

/* Binding-1 range. Dynamic offset selects the per-draw slice; the range
 * itself is shared by every recycled set, so it must stay constant.
 */
#define COGL_VK_UBO_SLICE 16384

/* Write sampler array (binding 0, count=N) + UBO (binding 1).
 * n_layers views/samplers; unused slots repeat slot 0. n_layers<=0 → 1.
 */
void cogl_vk_pipeline_update_cold_ds (CoglVkPipelineCache *cache,
                                      VkDescriptorSet      ds,
                                      int                  n_layers,
                                      const VkImageView   *views,
                                      const VkSampler     *samplers);

/* Pack UBO + alloc/update cold DS for multilayer assemble draws.
 * *out_ds must be freed with cogl_vk_pipeline_free_cold_ds.
 */
gboolean cogl_vk_pipeline_prepare_cold_ds (CoglVkPipelineCache  *cache,
                                           CoglPipeline         *pipeline,
                                           CoglVkImage          *src_layer0,
                                           VkDescriptorSet      *out_ds,
                                           GError              **error);

/* Lazy 1×1 white sampled image for cold DS when pipeline has no layer0 tex. */
gboolean cogl_vk_pipeline_ensure_dummy_white (CoglVkPipelineCache  *cache,
                                              GError              **error);

/* FALSE when every layer image is already readable by this pass's shaders,
 * so the draw can stay inside the open render pass. */
gboolean cogl_vk_pipeline_layer_textures_need_barrier (CoglVkPass   *pass,
                                                       CoglPipeline *pipeline,
                                                       CoglVkImage  *src_layer0,
                                                       gboolean      use_cold);

/* Transition every pipeline layer image to SHADER_READ_ONLY (pre-RP). */
void cogl_vk_pipeline_barrier_layer_textures (CoglVkPass   *pass,
                                              CoglPipeline *pipeline,
                                              CoglVkImage  *src_layer0,
                                              gboolean      use_cold);

gboolean cogl_vk_pipeline_prepare_cold_images (CoglVkPass          *pass,
                                               CoglVkPipelineCache *cache,
                                               CoglPipeline        *pipeline,
                                               CoglVkImage        **src_layer0,
                                               GError             **error);

/* Solid-color quad (quad.frag). vert_push NULL → unit NDC ortho (y-flip baked
 * from fb/pipeline like GL no-snippet projection).
 */
gboolean cogl_vk_cmd_draw_quad (CoglVkPass              *pass,
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
                                GError                 **error);

/* Image blit/copy between two CoglVkImages (GL blit parity).
 * src_mip / dst_mip select the mip level (0 = base).
 */
gboolean cogl_vk_cmd_blit_images (CoglVkPass  *pass,
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
                                  GError     **error);

/* MSAA resolve (src samples>1 → dst samples==1). */
gboolean cogl_vk_cmd_resolve_image (CoglVkPass  *pass,
                                    CoglVkImage *src_ms,
                                    CoglVkImage *dst_1x,
                                    GError     **error);

/* Collect layers in unit order. Never use get_layer(pipeline, unit).
 * Caller frees with g_free.
 */
typedef struct _CoglPipelineLayer CoglPipelineLayer;
void cogl_vk_pipeline_collect_layers (CoglPipeline        *pipeline,
                                      CoglPipelineLayer ***layers_out,
                                      int                 *n_out);

G_END_DECLS

/* Only after the queue is idle and the recording will be discarded. */
void cogl_vk_pipeline_cache_discard_recording (CoglVkPipelineCache *cache);
