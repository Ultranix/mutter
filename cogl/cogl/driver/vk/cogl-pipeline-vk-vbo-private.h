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

/* Vertex and index buffer drawing for the Cogl Vulkan driver.
 * Journal quads use the separate push-constant path in CoglFramebufferVk.
 */

#pragma once

#include "cogl/driver/vk/cogl-pipeline-vk-private.h"
#include "cogl/cogl-attribute-private.h"
#include "cogl/cogl-framebuffer.h"
#include "cogl/cogl-indices.h"
#include "cogl/cogl-pipeline.h"
#include "cogl/cogl-types.h"

#include <glib.h>
#include <vulkan/vulkan.h>

G_BEGIN_DECLS

#define COGL_VK_VBO_LOC_POSITION      0
#define COGL_VK_VBO_LOC_COLOR         1
#define COGL_VK_VBO_LOC_TEXCOORD0     2
#define COGL_VK_VBO_LOC_POINT_SIZE    3
#define COGL_VK_VBO_LOC_NORMAL        4
/* Locations 0..4 retain the hot shader ABI. Additional texcoords and
 * custom attributes are packed consecutively from location 5 per draw.
 */
#define COGL_VK_VBO_LOC_TEXCOORD_BASE 5

gboolean cogl_vk_vbo_should_use_push_constant_fast_path (CoglVerticesMode  mode,
                                                         int               n_vertices,
                                                         CoglAttribute   **attributes,
                                                         int               n_attributes,
                                                         CoglPipeline     *pipeline);

gboolean cogl_vk_cmd_draw_vbo (CoglVkPass              *pass,
                               CoglVkPipelineCache     *cache,
                               CoglVkRenderFormatSetup *setup,
                               CoglVkImage             *dst,
                               CoglVkImage             *src,
                               VkDescriptorSet          src_ds,
                               CoglPipeline            *pipeline,
                               CoglFramebuffer         *framebuffer,
                               CoglVerticesMode         mode,
                               int                      first_vertex,
                               int                      n_vertices,
                               CoglAttribute          **attributes,
                               int                      n_attributes,
                               gboolean                 opaque,
                               float                    alpha,
                               VkPipeline               pipe_override,
                               GError                 **error);

gboolean cogl_vk_cmd_draw_indexed_vbo (CoglVkPass              *pass,
                                       CoglVkPipelineCache     *cache,
                                       CoglVkRenderFormatSetup *setup,
                                       CoglVkImage             *dst,
                                       CoglVkImage             *src,
                                       VkDescriptorSet          src_ds,
                                       CoglPipeline            *pipeline,
                                       CoglFramebuffer         *framebuffer,
                                       CoglVerticesMode         mode,
                                       int                      first_index,
                                       int                      n_indices,
                                       CoglIndices             *indices,
                                       CoglAttribute          **attributes,
                                       int                      n_attributes,
                                       gboolean                 opaque,
                                       float                    alpha,
                                       VkPipeline               pipe_override,
                                       GError                 **error);

/* Device must be idle. Clears only resources owned by this device. */
void cogl_vk_vbo_device_clear (CoglVkDevice *device);

G_END_DECLS
