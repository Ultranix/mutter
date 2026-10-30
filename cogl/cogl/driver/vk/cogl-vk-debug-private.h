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

/* Env-gated Vulkan diagnostics. Set COGL_VK_DEBUG to a comma list:
 * vbo,barrier,readback,mvp,tex,fence,buf,draw,all
 * Logs go to stderr and /tmp/cogl-vk-debug.log
 */
#pragma once

#include "cogl/driver/vk/cogl-pipeline-vk-private.h"
#include "cogl/driver/vk/cogl-image-vk-private.h"
#include "cogl/driver/vk/cogl-pass-vk-private.h"

#include "cogl/cogl-attribute.h"
#include "cogl/cogl-framebuffer.h"
#include "cogl/cogl-pipeline.h"

#include <glib.h>
#include <vulkan/vulkan.h>

G_BEGIN_DECLS

gboolean cogl_vk_debug_enabled (const char *channel);

void cogl_vk_debug_log (const char *channel,
                        const char *fmt,
                        ...) G_GNUC_PRINTF (2, 3);

/* Empty marker for gdb: break cogl_vk_debug_mid_rp_allocate */
void cogl_vk_debug_mid_rp_allocate (int   width,
                                    int   height,
                                    void *tex);

void cogl_vk_debug_vbo_draw (CoglVkPass              *pass,
                             CoglFramebuffer         *fb,
                             CoglPipeline            *pipeline,
                             CoglVerticesMode         mode,
                             int                      first,
                             int                      count,
                             gboolean                 opaque,
                             float                    alpha,
                             gboolean                 textured,
                             gboolean                 use_cold,
                             gboolean                 dummy_white,
                             CoglVkImage             *src,
                             CoglVkImage             *dst,
                             const CoglVkVertPush    *vert,
                             CoglAttribute          **attributes,
                             int                      n_attributes,
                             unsigned int             has_color,
                             unsigned int             has_texcoord,
                             unsigned int             color_type,
                             unsigned int             color_components,
                             gboolean                 normalized_color,
                             VkPipeline               pipe,
                             VkDescriptorSet          ds,
                             VkCullModeFlags          cull_mode,
                             VkFrontFace              front_face,
                             VkPrimitiveTopology      topology);

void cogl_vk_debug_barrier (const char   *tag,
                            CoglVkImage  *image,
                            VkImageLayout old_layout,
                            VkImageLayout new_layout,
                            gboolean      used_host_src);

void cogl_vk_debug_readback (CoglFramebuffer *fb,
                             int              x,
                             int              y,
                             int              w,
                             int              h,
                             const uint8_t   *rgba,
                             int              rowstride);

void cogl_vk_debug_image_texels (const char  *tag,
                                 CoglVkImage *image);

G_END_DECLS

/* Optional CPU wall-clock scopes; nested scopes are inclusive. */
typedef struct {
  const char *name;
  int64_t window_start, total_us, max_us;
  uint64_t count;
} CoglVkProfileStats;
typedef struct {
  CoglVkProfileStats *stats;
  int64_t start;
} CoglVkProfileScope;
CoglVkProfileScope cogl_vk_profile_begin (CoglVkProfileStats *stats);
void cogl_vk_profile_end (CoglVkProfileScope *scope);
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (CoglVkProfileScope, cogl_vk_profile_end)
#define COGL_VK_PROFILE_SCOPE(label) \
  static CoglVkProfileStats profile_stats = { .name = label }; \
  g_auto (CoglVkProfileScope) profile_scope = cogl_vk_profile_begin (&profile_stats)
