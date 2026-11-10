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

/* Vulkan image allocation, views and DMA-BUF imports for Cogl textures.
 * Images track layout and queue ownership across rendering and presentation.
 */

#pragma once

#include "cogl/driver/vk/cogl-device-vk-private.h"

#include "cogl/cogl-pixel-format.h"

#include <glib.h>
#include <vulkan/vulkan.h>

G_BEGIN_DECLS

typedef struct _CoglVkImage CoglVkImage;

struct _CoglVkImage
{
  CoglVkDevice *device; /* unowned */
  VkImage image;
  /* Single-plane or disjoint: memories[0..n_memories). */
  VkDeviceMemory memories[4];
  uint32_t n_memories;
  VkImageView view;
  VkImageView attachment_view; /* Identity swizzle, level zero only. */
  VkFormat format;
  uint32_t drm_fourcc; /* External storage order, including opaque X channels. */
  /* Cogl pixel format used for component swizzle; 0/ANY → identity.
   * Dmabuf imports leave this unset (fourcc already matches VkFormat).
   */
  CoglPixelFormat cogl_format;
  uint32_t width;
  uint32_t height;
  uint32_t mip_levels; /* 1 unless generate_mipmap upgraded the image */
  VkSampleCountFlagBits samples; /* 1 unless MSAA offscreen */
  VkImageLayout layout;
  /* A sampling barrier was recorded for the current contents, so further
   * draws can read this image inside the open render pass. */
  gboolean shader_read_synced;
  gboolean owns_memory;
  /* Imported DMA-BUF images require external queue ownership transfers. */
  gboolean is_dmabuf;
  /* TRUE while the graphics queue owns the dmabuf. KMS cannot see writes
   * until a foreign release, and the next use must acquire again.
   */
  gboolean dmabuf_graphics_owned;
  /* dma-buf inode, so a draw can be matched to the KMS scanout BO. */
  uint64_t debug_dmabuf_ino;
  /* Image view uses a sampler YCbCr conversion for NV12 sampling. */
  gboolean is_ycbcr;
  unsigned int ycbcr_index; /* matrix * 2 + range; matches immutable sampler */
  /* Reused storage, refreshed for each cold draw; owned by this image. */
  CoglVkImage *cold_rgba;
  /* Linear HOST_VISIBLE upload path (SIZE/bitmap textures). */
  gboolean host_visible;
  /* Some command buffer has recorded a barrier on it, so the GPU may still
   * be reading it; until then the CPU may write a host-visible image in
   * place without waiting. */
  gboolean gpu_used;
  /* Lazily created for graphics draws. */
  VkFramebuffer framebuffer;
  VkRenderPass framebuffer_render_pass;
  /* Unowned DS companion used when creating a 2-attachment VkFramebuffer. */
  CoglVkImage *framebuffer_ds;
  /* Unowned DS attached by CoglFramebufferVk for offscreen color targets. */
  CoglVkImage *attached_depth_stencil;
};

VkFormat cogl_vk_format_from_cogl (CoglPixelFormat format);

/* Component swizzle so sampled rgba matches Cogl channel expectations
 * given the VkFormat chosen by cogl_vk_format_from_cogl(). Default identity.
 */
void cogl_vk_format_swizzle_from_cogl (CoglPixelFormat     format,
                                       VkComponentMapping *out);

VkFormat cogl_vk_format_from_drm_fourcc (uint32_t fourcc);

void cogl_vk_format_swizzle_from_drm (uint32_t            fourcc,
                                      VkComponentMapping *components);

VkImageUsageFlags cogl_vk_dmabuf_image_usage (uint32_t fourcc);

gboolean cogl_vk_dmabuf_format_supported (CoglVkDevice     *device,
                                          uint32_t          fourcc,
                                          uint64_t          modifier,
                                          VkImageUsageFlags usage,
                                          uint32_t         *n_planes);

CoglVkImage * cogl_vk_image_new_optimal (CoglVkDevice     *device,
                                         uint32_t          width,
                                         uint32_t          height,
                                         VkFormat          format,
                                         CoglPixelFormat   cogl_format,
                                         VkImageUsageFlags usage,
                                         GError          **error);

/* Allocate an optimal image with an explicit sample count. */
CoglVkImage * cogl_vk_image_new_optimal_samples (CoglVkDevice          *device,
                                                 uint32_t               width,
                                                 uint32_t               height,
                                                 VkFormat               format,
                                                 CoglPixelFormat        cogl_format,
                                                 VkImageUsageFlags      usage,
                                                 VkSampleCountFlagBits  samples,
                                                 GError               **error);

/* Prefer D24_UNORM_S8_UINT, then D32_SFLOAT_S8_UINT or D16_UNORM_S8_UINT.
 * samples > 1 → MS packed DS matching MS color (B5/B6). */
CoglVkImage * cogl_vk_image_new_depth_stencil (CoglVkDevice          *device,
                                               uint32_t               width,
                                               uint32_t               height,
                                               VkSampleCountFlagBits  samples,
                                               GError               **error);

/* Depth-only (D16 / D32 / X8D24) — GL FLAG_DEPTH. */
CoglVkImage * cogl_vk_image_new_depth (CoglVkDevice          *device,
                                       uint32_t               width,
                                       uint32_t               height,
                                       VkSampleCountFlagBits  samples,
                                       GError               **error);

/* Stencil-only (S8) — GL FLAG_STENCIL. */
CoglVkImage * cogl_vk_image_new_stencil (CoglVkDevice          *device,
                                         uint32_t               width,
                                         uint32_t               height,
                                         VkSampleCountFlagBits  samples,
                                         GError               **error);

void cogl_vk_format_color_bits (VkFormat format,
                                int     *red,
                                int     *green,
                                int     *blue,
                                int     *alpha);

void cogl_vk_format_depth_stencil_bits (VkFormat format,
                                        int     *depth,
                                        int     *stencil);

/* Optimal image with mipLevels for generate_mipmap blit chains. */
CoglVkImage * cogl_vk_image_new_optimal_mips (CoglVkDevice     *device,
                                              uint32_t          width,
                                              uint32_t          height,
                                              uint32_t          mip_levels,
                                              VkFormat          format,
                                              CoglPixelFormat   cogl_format,
                                              VkImageUsageFlags usage,
                                              GError          **error);

/* Recreate image view covering mip_levels (after upgrade). */
gboolean cogl_vk_image_recreate_view (CoglVkImage *image,
                                      GError     **error);
gboolean cogl_vk_image_ensure_attachment_view (CoglVkImage *image,
                                               GError     **error);

CoglVkImage * cogl_vk_image_new_host_visible (CoglVkDevice   *device,
                                              uint32_t        width,
                                              uint32_t        height,
                                              VkFormat        format,
                                              CoglPixelFormat cogl_format,
                                              const uint8_t  *pixels,
                                              uint32_t        rowstride,
                                              GError        **error);

CoglVkImage * cogl_vk_image_import_dmabuf (CoglVkDevice            *device,
                                           uint32_t                 width,
                                           uint32_t                 height,
                                           uint32_t                 drm_fourcc,
                                           int                      n_planes,
                                           const int               *fds,
                                           const uint32_t          *strides,
                                           const uint32_t          *offsets,
                                           const uint64_t          *modifiers,
                                           VkSamplerYcbcrConversion ycbcr_conversion,
                                           GError                 **error);

void cogl_vk_image_free (CoglVkImage *image);

/* Destroy now, without flushing or waiting: only for an image the GPU is
 * known to be done with (a signaled retire ticket, or an idle device). */
void cogl_vk_image_destroy_now (CoglVkImage *image);

COGL_EXPORT void cogl_vk_image_cmd_barrier (CoglVkImage         *image,
                                            VkCommandBuffer      cmd,
                                            VkImageLayout        new_layout,
                                            VkPipelineStageFlags src_stage,
                                            VkPipelineStageFlags dst_stage,
                                            VkAccessFlags        src_access,
                                            VkAccessFlags        dst_access);

/* Transfer DMA-BUF ownership between the external and graphics queues. */
void cogl_vk_image_cmd_foreign_acquire (CoglVkImage    *image,
                                        VkCommandBuffer cmd,
                                        uint32_t        graphics_queue_family,
                                        VkImageLayout   new_layout);

void cogl_vk_image_cmd_foreign_release (CoglVkImage    *image,
                                        VkCommandBuffer cmd,
                                        uint32_t        graphics_queue_family,
                                        VkImageLayout   new_layout);

/* One-shot clear UNDEFINED → COLOR_ATTACHMENT with defined contents so
 * begin_render loadOp=LOAD is legal (avoids recording Clear inside draw CB).
 */
gboolean cogl_vk_image_oneshot_clear_to_color_attachment (CoglVkImage *image,
                                                          GError     **error);

/* Same for depth/stencil: UNDEFINED → DEPTH_STENCIL_ATTACHMENT with
 * depth=1 / stencil=0. Do not pair DS pipelines with a color-only RP.
 */
gboolean cogl_vk_image_oneshot_clear_to_depth_stencil_attachment (CoglVkImage *image,
                                                                  GError     **error);

G_END_DECLS

COGL_EXPORT CoglVkImage * cogl_vk_image_new_nv12 (CoglVkDevice            *device,
                                                  uint32_t                 width,
                                                  uint32_t                 height,
                                                  VkSamplerYcbcrConversion conversion,
                                                  GError                 **error);
gboolean cogl_vk_image_set_ycbcr_conversion (CoglVkImage             *image,
                                             VkSamplerYcbcrConversion conversion,
                                             GError                 **error);
