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

#include "cogl/cogl-texture-driver.h"
#include "cogl/cogl-texture-2d-private.h"
#include "cogl/driver/vk/cogl-image-vk-private.h"

G_DECLARE_FINAL_TYPE (CoglTextureDriverVk,
                      cogl_texture_driver_vk,
                      COGL,
                      TEXTURE_DRIVER_VK,
                      CoglTextureDriver)

#define COGL_TYPE_TEXTURE_DRIVER_VK (cogl_texture_driver_vk_get_type ())

G_DECLARE_FINAL_TYPE (CoglTexture2DVk,
                      cogl_texture_2d_vk,
                      COGL,
                      TEXTURE_2D_VK,
                      CoglTexture2D)

#define COGL_TYPE_TEXTURE_2D_VK (cogl_texture_2d_vk_get_type ())

CoglVkImage * cogl_texture_2d_vk_peek_image (CoglTexture2DVk * tex);

/* Resolve atlas / subtexture wrappers to the leaf Texture2DVk for sampling. */
CoglTexture2DVk * cogl_texture_2d_vk_resolve (CoglTexture *tex);

VkDescriptorSet cogl_texture_2d_vk_ensure_descriptor_set (CoglTexture2DVk *tex,
                                                          GError         **error);

/* Ownership of sync_fd transferred; -1 clears. Consumed on next draw wait. */
void cogl_texture_2d_vk_set_acquire_sync_fd (CoglTexture2DVk *tex,
                                             int              sync_fd);

int cogl_texture_2d_vk_take_acquire_sync_fd (CoglTexture2DVk *tex);

CoglTexture * cogl_texture_2d_vk_new_from_dmabuf (CoglContext    *ctx,
                                                  int             width,
                                                  int             height,
                                                  CoglPixelFormat format,
                                                  uint32_t        drm_fourcc,
                                                  int             n_planes,
                                                  const int      *fds,
                                                  const uint32_t *strides,
                                                  const uint32_t *offsets,
                                                  const uint64_t *modifiers,
                                                  CoglYuvMatrix   matrix,
                                                  CoglYuvRange    range,
                                                  GError        **error);

gboolean cogl_texture_2d_vk_set_yuv_colorimetry (CoglTexture2DVk *texture,
                                                 CoglYuvMatrix    matrix,
                                                 CoglYuvRange     range,
                                                 GError         **error);
COGL_EXPORT CoglTexture * cogl_texture_2d_vk_new_from_image (CoglContext *ctx,
                                                             CoglVkImage *image);
