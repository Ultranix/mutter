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

#pragma once

#include "cogl/cogl-framebuffer-driver.h"
#include "cogl/cogl-framebuffer-private.h"
#include "cogl/cogl-macros.h"
#include "cogl/driver/vk/cogl-image-vk-private.h"
#include "cogl/driver/vk/cogl-pass-vk-private.h"

G_DECLARE_FINAL_TYPE (CoglFramebufferVk,
                      cogl_framebuffer_vk,
                      COGL,
                      FRAMEBUFFER_DRIVER_VK,
                      CoglFramebufferDriver)

#define COGL_TYPE_FRAMEBUFFER_DRIVER_VK (cogl_framebuffer_vk_get_type ())

CoglVkImage * cogl_framebuffer_vk_peek_color_image (CoglFramebufferVk * fb);

void cogl_framebuffer_vk_replace_color_image (CoglFramebufferVk   *fb,
                                              CoglVkImage         *old_image,
                                              CoglVkImage         *new_image);

/* Point dst color / ms_color / DS at src's images without stealing ownership
 * (MetaOnscreenNative BIND → per-BO Offscreen; mirror GL FBO redirect).
 */
void cogl_framebuffer_vk_adopt_targets_from (CoglFramebufferVk *dst,
                                             CoglFramebufferVk *src);

/* Mutter-facing: resolve FB drivers then adopt (symbols stay inside libcogl).
 */
COGL_EXPORT void
cogl_framebuffer_vk_adopt_onscreen_targets (CoglFramebuffer *dst_fb,
                                            CoglFramebuffer *src_fb);

/* Draw attachment DS (packed / depth-only / stencil-only). */
CoglVkImage * cogl_framebuffer_vk_peek_depth_stencil (CoglFramebufferVk *fb);

gboolean cogl_framebuffer_vk_ensure_depth_stencil (CoglFramebufferVk *fb);

void cogl_framebuffer_vk_apply_driver_config (CoglFramebufferVk                 *fb,
                                              const CoglFramebufferDriverConfig *cfg);

/* MS color (if any) + matching-sample DS; returns draw color target. */
CoglVkImage * cogl_framebuffer_vk_prepare_draw_targets (CoglFramebufferVk *fb);

/* True VK blit/copy (not glBlitFramebuffer). */
gboolean cogl_framebuffer_vk_blit_box (CoglFramebuffer *src_fb,
                                       CoglFramebuffer *dst_fb,
                                       int              src_x1,
                                       int              src_y1,
                                       int              src_x2,
                                       int              src_y2,
                                       int              dst_x1,
                                       int              dst_y1,
                                       int              dst_x2,
                                       int              dst_y2);

COGL_EXPORT void cogl_framebuffer_vk_peek_depth_stencil_images (CoglFramebufferVk *fb,
                                                                CoglVkImage      **depth,
                                                                CoglVkImage      **stencil,
                                                                VkImageView       *depth_view,
                                                                VkImageView       *stencil_view);
