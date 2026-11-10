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

/* Generate Vulkan GLSL from Cogl texture layers, combine state and snippets.
 * The resulting shader sources are compiled and cached by the Vulkan driver.
 */

#pragma once

#include "cogl/cogl-pipeline.h"
#include "cogl/cogl-pipeline-private.h"
#include "cogl/cogl-pipeline-layer-private.h"

#include <glib.h>

G_BEGIN_DECLS


typedef enum
{
  /* Unit-quad via gl_VertexIndex + push-constant matrix (journal hot). */
  COGL_VK_ASSEMBLE_VERT_PUSH_QUAD = 0,
  /* True VBO attributes using the draw's compact location mapping. */
  COGL_VK_ASSEMBLE_VERT_VBO = 1,
} CoglVkAssembleVertMode;

/* Optional VERT_VBO layout. Arrays are borrowed for the duration of assembly. */
typedef struct _CoglVkAssembleVboAttrs
{
  gboolean flat_varyings;
  gboolean has_normal;
  int normal_components; /* typically 3 */
  /* Non-normalized BYTE/SHORT positions: VI uses SINT/UINT; shader
   * i/uvec→vec4. SSCALED+float `in` read as 0 on Intel → black geometry.
   */
  gboolean position_as_int;
  gboolean position_unsigned;
  int position_components;
  int n_texcoords;
  const int *texcoord_layers;
  int custom_base;
  int n_custom;
  int *custom_components;
  const char **custom_names; /* unowned */
  /* Non-normalized BYTE/SHORT customs: SINT/UINT VI + i/uvec `in`. */
  gboolean *custom_as_int;
  gboolean *custom_unsigned;
} CoglVkAssembleVboAttrs;

typedef struct _CoglVkAssembleResult
{
  char *vert_glsl;
  char *frag_glsl;
  int n_layers; /* samplers required in set0 binding0..n-1 */
  gboolean has_vertex_hooks;
  /* GL vertend: any vertex snippets → multiply by UBO `_cogl_flip_vector`
   * (do not bake y-flip into projection). No-snippet paths bake via
   * cogl_vk_maybe_bake_y_flip_projection instead.
   */
  gboolean needs_flip_vector;
  /* VBO / cold DS: per-layer texcoord varyings + texture-matrix UBO. */
  int n_texcoord_varyings; /* == n_layers; locations 0..n-1, color at n */
  gboolean needs_texture_matrix_ubo; /* always TRUE when assembled */
  int texcoord0_attr_location; /* VBO: first cogl_tex_coord*_in location */
  int point_size_attr_location; /* VBO: -1 if unused */
  int normal_attr_location; /* VBO: always 4; -1 for PUSH_QUAD */
  int custom_attr_base_location; /* VBO: packed after texcoords */
} CoglVkAssembleResult;

void cogl_vk_assemble_result_clear (CoglVkAssembleResult *result);

/* Emit Vulkan GLSL for pipeline (layers + combine + FRAGMENT/VERTEX hooks).
 * vbo_attrs may be NULL (PUSH_QUAD / no CUSTOM). For VERT_VBO, pass
 * mask-derived
 * NORMAL/CUSTOM so locations match cogl-pipeline-vk-vbo-private.h.
 * Returns FALSE on unsupported combine/hook with error set. */
COGL_EXPORT_TEST
gboolean cogl_vk_assemble_pipeline_glsl (CoglPipeline                 *pipeline,
                                         CoglVkAssembleVertMode        vert_mode,
                                         const CoglVkAssembleVboAttrs *vbo_attrs,
                                         CoglVkAssembleResult         *out,
                                         GError                      **error);

/* Hash fingerprint of assemble inputs (for pipeline cache key). */
COGL_EXPORT_TEST
char * cogl_vk_assemble_pipeline_fingerprint (CoglPipeline *pipeline);

void cogl_vk_assemble_pipeline_pre_change (CoglPipeline           *pipeline,
                                           CoglPipelineState       pipeline_change,
                                           CoglPipelineLayerState  layer_change);

G_END_DECLS
