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

/* GLSL compilation and shader caching for Cogl pipelines.
 * Simple draws use embedded SPIR-V; pipelines with multiple layers or
 * snippets compile generated GLSL through shaderc.
 */

#pragma once

#include "cogl/cogl-pipeline.h"
#include "cogl/driver/vk/cogl-device-vk-private.h"
#include "cogl/driver/vk/cogl-pipeline-vk-private.h"

#include <glib.h>
#include <vulkan/vulkan.h>

G_BEGIN_DECLS

typedef struct _CoglVkSpirvCache CoglVkSpirvCache;

/* True when fixed SPIR-V tex/quad path is enough (no snippets, ≤1 layer). */
gboolean cogl_vk_pipeline_is_hot_path (CoglPipeline *pipeline);

CoglVkSpirvCache * cogl_vk_spirv_cache_new (CoglVkDevice *device);

void cogl_vk_spirv_cache_free (CoglVkSpirvCache *cache);

/* Compile GLSL → SPIR-V (shaderc) and cache VkShaderModule by key.
 * Returns VK_NULL_HANDLE if shaderc unavailable or compile failed.
 */
VkShaderModule cogl_vk_spirv_cache_get_module (CoglVkSpirvCache     *cache,
                                               const char           *key,
                                               VkShaderStageFlagBits stage,
                                               const char           *glsl_source,
                                               GError              **error);

/* Look up or create a graphics pipeline for the Cogl shader and render state.
 * The cache owns the result. Returns VK_NULL_HANDLE on failure.
 */
VkPipeline cogl_vk_spirv_cache_get_pipeline_for_cogl (CoglVkSpirvCache        *cache,
                                                      CoglVkPipelineCache     *pipe_cache,
                                                      CoglVkRenderFormatSetup *setup,
                                                      CoglPipeline            *pipeline,
                                                      gboolean                 opaque,
                                                      GError                 **error);

/* Strip snippet uniforms + inject std140 UBO for ds_layout_cold. */
char * cogl_vk_glsl_finalize_cold_frag (CoglVkPipelineCache  *cache,
                                        CoglPipeline         *pipeline,
                                        const char           *assembled_frag,
                                        GError              **error);

char * cogl_vk_glsl_finalize_cold_vert (CoglVkPipelineCache  *cache,
                                        CoglPipeline         *pipeline,
                                        const char           *assembled_vert,
                                        GError              **error);

G_END_DECLS
