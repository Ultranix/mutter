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

#include "cogl/driver/vk/cogl-texture-driver-vk-private.h"
#include "cogl/driver/vk/cogl-driver-vk-private.h"
#include "cogl/driver/vk/cogl-pipeline-vk-private.h"
#include "cogl/driver/vk/cogl-dmabuf-memo-vk-private.h"
#include "cogl/driver/vk/cogl-framebuffer-vk-private.h"
#include "cogl/driver/vk/cogl-pass-vk-private.h"
#include "cogl/driver/vk/cogl-vk-debug-private.h"

#include "cogl/cogl-texture-2d-private.h"
#include "cogl/cogl-texture-private.h"
#include "cogl/cogl-bitmap-private.h"
#include "cogl/cogl-bitmap.h"
#include "cogl/cogl-context-private.h"
#include "cogl/cogl-framebuffer-private.h"
#include "cogl/cogl-private.h"
#include "cogl/cogl-renderer.h"

#if defined (HAVE_EGL) && defined (EGL_KHR_image_base)
#include "cogl/cogl-renderer-egl.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif

#include <drm_fourcc.h>
#include <string.h>
#include <unistd.h>

struct _CoglTexture2DVk
{
  CoglTexture2D parent_instance;
  CoglVkImage *image;
  VkDescriptorSet descriptor_set;
  /* Wayland drm_syncobj acquire → TEMPORARY SYNC_FD wait on next draw. */
  int pending_acquire_sync_fd;
};

struct _CoglTextureDriverVk
{
  CoglTextureDriver parent_instance;
};

G_DEFINE_FINAL_TYPE (CoglTexture2DVk, cogl_texture_2d_vk, COGL_TYPE_TEXTURE_2D)
G_DEFINE_FINAL_TYPE (CoglTextureDriverVk,
                     cogl_texture_driver_vk,
                     COGL_TYPE_TEXTURE_DRIVER)

CoglVkImage *
cogl_texture_2d_vk_peek_image (CoglTexture2DVk * tex)
{
  g_return_val_if_fail (COGL_IS_TEXTURE_2D_VK (tex), NULL);
  return tex->image;
}

VkDescriptorSet
cogl_texture_2d_vk_ensure_descriptor_set (CoglTexture2DVk *tex,
                                          GError         **error)
{
  CoglDriverVk *driver_vk;
  CoglVkPipelineCache *cache;

  g_return_val_if_fail (COGL_IS_TEXTURE_2D_VK (tex), VK_NULL_HANDLE);

  if (tex->descriptor_set != VK_NULL_HANDLE)
    return tex->descriptor_set;

  if (!tex->image || tex->image->view == VK_NULL_HANDLE)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "texture has no image view");
      return VK_NULL_HANDLE;
    }

  driver_vk = COGL_DRIVER_VK (cogl_context_get_driver (
                                cogl_texture_get_context (COGL_TEXTURE (tex))));
  cache = cogl_driver_vk_peek_pipeline_cache (driver_vk);
  if (!cache)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "no pipeline cache");
      return VK_NULL_HANDLE;
    }

  if (!cogl_vk_pipeline_alloc_texture_ds (cache, tex->image->is_ycbcr ? tex->image->ycbcr_index + 1 : 0,
                                          &tex->descriptor_set, error))
    return VK_NULL_HANDLE;

  cogl_vk_pipeline_update_texture_ds (cache, tex->descriptor_set,
                                      tex->image->view,
                                      tex->image->is_ycbcr ? tex->image->ycbcr_index + 1 : 0,
                                      VK_NULL_HANDLE);
  return tex->descriptor_set;
}

void
cogl_texture_2d_vk_set_acquire_sync_fd (CoglTexture2DVk *tex,
                                        int              sync_fd)
{
  g_return_if_fail (COGL_IS_TEXTURE_2D_VK (tex));

  if (tex->pending_acquire_sync_fd >= 0)
    close (tex->pending_acquire_sync_fd);
  tex->pending_acquire_sync_fd = sync_fd;
}

int
cogl_texture_2d_vk_take_acquire_sync_fd (CoglTexture2DVk *tex)
{
  int fd;

  g_return_val_if_fail (COGL_IS_TEXTURE_2D_VK (tex), -1);

  fd = tex->pending_acquire_sync_fd;
  tex->pending_acquire_sync_fd = -1;
  return fd;
}

static CoglVkDevice *
peek_vk_device_from_texture (CoglTexture *tex)
{
  CoglContext *ctx = cogl_texture_get_context (tex);
  CoglDriver *driver = cogl_context_get_driver (ctx);

  if (!COGL_IS_DRIVER_VK (driver))
    return NULL;

  return cogl_driver_vk_peek_device (COGL_DRIVER_VK (driver));
}

static void
cogl_texture_2d_vk_pre_paint (CoglTexture              *tex,
                              CoglTexturePrePaintFlags  flags)
{
  CoglTexture2D *tex_2d = COGL_TEXTURE_2D (tex);

  if ((flags & COGL_TEXTURE_NEEDS_MIPMAP) &&
      tex_2d->auto_mipmap && tex_2d->mipmaps_dirty)
    {
      CoglContext *ctx = cogl_texture_get_context (tex);
      CoglDriver *driver = cogl_context_get_driver (ctx);

      _cogl_texture_flush_journal_rendering (tex);

      if (cogl_driver_has_feature (driver,
                                   COGL_FEATURE_ID_QUIRK_GENERATE_MIPMAP_NEEDS_FLUSH))
        {
          const GList *fbs = _cogl_texture_get_associated_framebuffers (tex);

          g_list_foreach ((GList *) fbs, (GFunc) cogl_framebuffer_flush, NULL);
        }

      COGL_TEXTURE_2D_GET_CLASS (tex_2d)->generate_mipmap (tex_2d);

      tex_2d->mipmaps_dirty = FALSE;
    }
}

static void
cogl_texture_2d_vk_retire_replaced_image (CoglTexture2DVk *tex_vk,
                                          CoglVkImage     *old_image)
{
  const GList *framebuffers;

  if (!old_image)
    return;

  framebuffers = _cogl_texture_get_associated_framebuffers (COGL_TEXTURE (tex_vk));
  for (const GList *l = framebuffers; l; l = l->next)
    {
      CoglFramebufferDriver *driver = cogl_framebuffer_get_driver (l->data);

      if (driver && COGL_IS_FRAMEBUFFER_DRIVER_VK (driver))
        cogl_framebuffer_vk_replace_color_image (COGL_FRAMEBUFFER_DRIVER_VK (driver),
                                                 old_image, tex_vk->image);
    }

  /* Flushes pending uses and waits before destroying the old attachment. */
  cogl_vk_image_free (old_image);
}

static void
cogl_texture_2d_vk_generate_mipmap (CoglTexture2D *tex_2d)
{
  CoglTexture2DVk *tex_vk = COGL_TEXTURE_2D_VK (tex_2d);
  CoglTexture *tex = COGL_TEXTURE (tex_2d);
  CoglVkDevice *device;
  CoglDriverVk *driver_vk;
  CoglVkPass *pass;
  CoglVkImage *src;
  CoglVkImage *mips = NULL;
  CoglVkImage *old_image = NULL;
  g_autoptr (GError) error = NULL;
  int n_levels;
  uint32_t level;
  uint32_t w, h;

  if (!tex_vk->image || tex_vk->image->is_dmabuf || tex_vk->image->is_ycbcr)
    return;

  n_levels = _cogl_texture_get_n_levels (tex);
  if (n_levels <= 1)
    return;

  device = peek_vk_device_from_texture (tex);
  if (!device)
    return;

  driver_vk = COGL_DRIVER_VK (cogl_context_get_driver (
                                cogl_texture_get_context (tex)));
  if (!driver_vk)
    return;

  src = tex_vk->image;

  /* Upgrade to an optimal multi-mip image when needed. */
  if (src->mip_levels < (uint32_t) n_levels || src->host_visible)
    {
      VkImageUsageFlags usage =
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;

      if (_cogl_texture_get_associated_framebuffers (tex))
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

      mips = cogl_vk_image_new_optimal_mips (device,
                                             src->width,
                                             src->height,
                                             (uint32_t) n_levels,
                                             src->format,
                                             src->cogl_format,
                                             usage,
                                             &error);
      if (!mips)
        {
          g_warning ("Cogl Vulkan: mipmap image alloc failed: %s",
                     error ? error->message : "unknown");
          return;
        }
    }

  if (!driver_vk->current_pass)
    {
      driver_vk->current_pass = cogl_vk_pass_begin (device, &error);
      if (!driver_vk->current_pass)
        {
          g_clear_pointer (&mips, cogl_vk_image_free);
          return;
        }
    }
  pass = driver_vk->current_pass;
  cogl_vk_pass_end_render (pass);

  if (mips)
    {
      /* Copy base level from existing image into the new mip chain. */
      if (!cogl_vk_cmd_blit_images (pass, src, mips,
                                    0, 0, (int) src->width, (int) src->height,
                                    0, 0, (int) src->width, (int) src->height,
                                    0, 0, &error))
        {
          g_warning ("Cogl Vulkan: mipmap base copy failed: %s",
                     error ? error->message : "unknown");
          g_clear_pointer (&mips, cogl_vk_image_free);
          return;
        }

      if (tex_vk->descriptor_set != VK_NULL_HANDLE &&
          driver_vk->pipeline_cache)
        {
          cogl_vk_pipeline_free_texture_ds (driver_vk->pipeline_cache,
                                            src->is_ycbcr,
                                            tex_vk->descriptor_set);
          tex_vk->descriptor_set = VK_NULL_HANDLE;
        }

      /* Defer free until after the mip blit loop: image_free flushes the
       * current pass, which would discard the transfer CB mid-generation
       * and leave levels 1..N in UNDEFINED (wrap-modes validation).
       */
      old_image = tex_vk->image;
      tex_vk->image = mips;
      src = mips;
    }

  /* Blit level N-1 → N for the remaining chain. */
  w = src->width;
  h = src->height;
  for (level = 1; level < src->mip_levels; level++)
    {
      VkImageBlit region = {0};
      VkImageMemoryBarrier barriers[2];
      uint32_t next_w = MAX (1u, w / 2);
      uint32_t next_h = MAX (1u, h / 2);

      memset (barriers, 0, sizeof (barriers));
      barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      barriers[0].oldLayout = (level == 1)
        ? src->layout
        : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barriers[0].image = src->image;
      barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      barriers[0].subresourceRange.baseMipLevel = level - 1;
      barriers[0].subresourceRange.levelCount = 1;
      barriers[0].subresourceRange.layerCount = 1;

      barriers[1] = barriers[0];
      barriers[1].srcAccessMask = 0;
      barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barriers[1].subresourceRange.baseMipLevel = level;

      vkCmdPipelineBarrier (pass->command_buffer,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 0, NULL, 0, NULL, 2, barriers);

      region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      region.srcSubresource.mipLevel = level - 1;
      region.srcSubresource.layerCount = 1;
      region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      region.dstSubresource.mipLevel = level;
      region.dstSubresource.layerCount = 1;
      region.srcOffsets[1].x = (int32_t) w;
      region.srcOffsets[1].y = (int32_t) h;
      region.srcOffsets[1].z = 1;
      region.dstOffsets[1].x = (int32_t) next_w;
      region.dstOffsets[1].y = (int32_t) next_h;
      region.dstOffsets[1].z = 1;

      vkCmdBlitImage (pass->command_buffer,
                      src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      src->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      1, &region, VK_FILTER_LINEAR);

      w = next_w;
      h = next_h;
    }

  /* Transition all levels to shader-read with correct old layouts.
   * After the blit loop (mip_levels > 1): levels 0..mip_levels-2 are
   * TRANSFER_SRC_OPTIMAL; the last level is TRANSFER_DST_OPTIMAL.
   * Using UNDEFINED here would discard the just-blitted mip contents.
   */
  {
    VkImageMemoryBarrier barriers[2];
    uint32_t n_barriers;

    memset (barriers, 0, sizeof (barriers));

    if (src->mip_levels == 1)
      {
        barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT |
                                    VK_ACCESS_TRANSFER_READ_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[0].oldLayout = src->layout;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].image = src->image;
        barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[0].subresourceRange.baseMipLevel = 0;
        barriers[0].subresourceRange.levelCount = 1;
        barriers[0].subresourceRange.layerCount = 1;
        n_barriers = 1;
      }
    else
      {
        barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT |
                                    VK_ACCESS_TRANSFER_READ_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].image = src->image;
        barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[0].subresourceRange.baseMipLevel = 0;
        barriers[0].subresourceRange.levelCount = src->mip_levels - 1;
        barriers[0].subresourceRange.layerCount = 1;

        barriers[1] = barriers[0];
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].subresourceRange.baseMipLevel = src->mip_levels - 1;
        barriers[1].subresourceRange.levelCount = 1;
        n_barriers = 2;
      }

    vkCmdPipelineBarrier (pass->command_buffer,
                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                          0, 0, NULL, 0, NULL, n_barriers, barriers);
    src->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }

  cogl_texture_2d_vk_retire_replaced_image (tex_vk, old_image);

  cogl_texture_set_max_level_set (tex, (int) src->mip_levels - 1);
  /* Optimal mip chain: get_data uses staging Image→Buffer (R4). */
  tex_2d->is_get_data_supported = TRUE;
}

static void
cogl_texture_2d_vk_copy_from_framebuffer (CoglTexture2D   *tex_2d,
                                          int              src_x,
                                          int              src_y,
                                          int              width,
                                          int              height,
                                          CoglFramebuffer *src_fb,
                                          int              dst_x,
                                          int              dst_y,
                                          int              level)
{
  CoglTexture2DVk *tex_vk = COGL_TEXTURE_2D_VK (tex_2d);
  CoglTexture *tex = COGL_TEXTURE (tex_2d);
  CoglFramebufferDriver *src_drv;
  CoglFramebufferVk *src_fb_vk;
  CoglDriverVk *driver_vk;
  CoglVkPass *pass;
  CoglVkImage *dst;
  uint32_t need_levels;
  int level_w = 0, level_h = 0;
  g_autoptr (GError) error = NULL;

  if (!tex_vk->image || level < 0)
    return;

  /* dmabuf/ycbcr: base-level blit only (no mip upgrade). */
  if ((tex_vk->image->is_dmabuf || tex_vk->image->is_ycbcr) && level != 0)
    return;

  _cogl_texture_get_level_size (tex, level, &level_w, &level_h, NULL);
  if (dst_x < 0 || dst_y < 0 || width <= 0 || height <= 0 ||
      dst_x + width > level_w || dst_y + height > level_h)
    return;

  src_drv = cogl_framebuffer_get_driver (src_fb);
  if (!COGL_IS_FRAMEBUFFER_DRIVER_VK (src_drv))
    return;

  src_fb_vk = COGL_FRAMEBUFFER_DRIVER_VK (src_drv);
  if (!cogl_framebuffer_vk_peek_color_image (src_fb_vk))
    return;

  driver_vk = COGL_DRIVER_VK (
    cogl_context_get_driver (cogl_texture_get_context (tex)));
  if (!driver_vk || !driver_vk->vk_device)
    return;

  if (!driver_vk->current_pass)
    {
      driver_vk->current_pass = cogl_vk_pass_begin (driver_vk->vk_device,
                                                    &error);
      if (!driver_vk->current_pass)
        return;
    }
  pass = driver_vk->current_pass;
  dst = tex_vk->image;

  /* Upgrade host-visible / single-mip to optimal mip chain when level≠0
   * (GL copyTexSubImage level parity; level 0 keeps existing image).
   */
  need_levels = (uint32_t) level + 1;
  if (dst->mip_levels > need_levels)
    need_levels = dst->mip_levels;
  /* Same as upload_mip_level: do not pre-size to get_n_levels. */

  if (!dst->is_dmabuf && !dst->is_ycbcr &&
      (dst->host_visible || dst->mip_levels < need_levels))
    {
      VkImageUsageFlags usage =
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;

      if (_cogl_texture_get_associated_framebuffers (tex))
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      CoglVkImage *mips;
      CoglVkImage *old_image;

      mips = cogl_vk_image_new_optimal_mips (driver_vk->vk_device,
                                             dst->width,
                                             dst->height,
                                             need_levels,
                                             dst->format,
                                             dst->cogl_format,
                                             usage,
                                             &error);
      if (!mips)
        return;

      if (!cogl_vk_cmd_blit_images (pass, dst, mips,
                                    0, 0, (int) dst->width, (int) dst->height,
                                    0, 0, (int) dst->width, (int) dst->height,
                                    0, 0, &error))
        {
          g_clear_pointer (&mips, cogl_vk_image_free);
          return;
        }

      if (tex_vk->descriptor_set != VK_NULL_HANDLE &&
          driver_vk->pipeline_cache)
        {
          cogl_vk_pipeline_free_texture_ds (driver_vk->pipeline_cache,
                                            dst->is_ycbcr,
                                            tex_vk->descriptor_set);
          tex_vk->descriptor_set = VK_NULL_HANDLE;
        }

      /* Same as generate_mipmap: freeing flushes the pass — defer until
       * the level blit below is recorded.
       */
      old_image = tex_vk->image;
      tex_vk->image = mips;
      dst = mips;
      tex_2d->is_get_data_supported = TRUE;

      if ((uint32_t) level >= MAX (1u, dst->mip_levels))
        {
          cogl_texture_2d_vk_retire_replaced_image (tex_vk, old_image);
          return;
        }

      if (!cogl_vk_cmd_blit_images (pass,
                                    cogl_framebuffer_vk_peek_color_image (src_fb_vk),
                                    dst,
                                    src_x, src_y, src_x + width, src_y + height,
                                    dst_x, dst_y, dst_x + width, dst_y + height,
                                    0, (uint32_t) level, &error))
        {
          cogl_texture_2d_vk_retire_replaced_image (tex_vk, old_image);
          return;
        }

      cogl_texture_2d_vk_retire_replaced_image (tex_vk, old_image);

      if (cogl_texture_get_max_level_set (tex) < level)
        cogl_texture_set_max_level_set (tex, level);
      return;
    }

  if ((uint32_t) level >= MAX (1u, dst->mip_levels))
    return;

  if (!cogl_vk_cmd_blit_images (pass,
                                cogl_framebuffer_vk_peek_color_image (src_fb_vk),
                                dst,
                                src_x, src_y, src_x + width, src_y + height,
                                dst_x, dst_y, dst_x + width, dst_y + height,
                                0, (uint32_t) level, &error))
    return;

  if (cogl_texture_get_max_level_set (tex) < level)
    cogl_texture_set_max_level_set (tex, level);
}

static int
find_host_visible_mem_type (CoglVkDevice *device,
                            uint32_t      type_bits)
{
  VkPhysicalDeviceMemoryProperties mem_props;
  uint32_t i;

  vkGetPhysicalDeviceMemoryProperties (device->physical_device, &mem_props);
  for (i = 0; i < mem_props.memoryTypeCount; i++)
    {
      if ((type_bits & (1u << i)) &&
          (mem_props.memoryTypes[i].propertyFlags &
           (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
          (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        return (int) i;
    }
  return -1;
}

static gboolean
flush_current_pass_for_upload (CoglDriverVk *driver_vk,
                               GError      **error)
{
  if (driver_vk->submission_failed || driver_vk->vk_device->device_lost)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Vulkan frame submission has failed");
      return FALSE;
    }

  if (!driver_vk->current_pass)
    return TRUE;

  /* Full submit+free: destroying/replacing VkImages while a CB still
   * references them invalidates the CB (VUID-*-commandBuffer-recording)
   * and ANV DEVICE_LOST. Atlas page allocate mid-frame hit this.
   */
  cogl_vk_debug_log (
    "tex",
    "upload: submit+free pass=%p in_rp=%d before image work",
    (void *) driver_vk->current_pass,
    driver_vk->current_pass->in_render_pass);

  if (!cogl_vk_pass_submit (driver_vk->current_pass, error))
    {
      g_clear_pointer (&driver_vk->current_pass, cogl_vk_pass_free);
      return FALSE;
    }

  cogl_driver_vk_store_sync_fd_from_pass (driver_vk);
  g_clear_pointer (&driver_vk->current_pass, cogl_vk_pass_free);
  return TRUE;
}

static void
cmd_barrier_mip_level (CoglVkImage         *image,
                       VkCommandBuffer      cmd,
                       uint32_t             mip_level,
                       VkImageLayout        old_layout,
                       VkImageLayout        new_layout,
                       VkPipelineStageFlags src_stage,
                       VkPipelineStageFlags dst_stage,
                       VkAccessFlags        src_access,
                       VkAccessFlags        dst_access)
{
  VkImageMemoryBarrier barrier = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
    .srcAccessMask = src_access,
    .dstAccessMask = dst_access,
    .oldLayout = old_layout,
    .newLayout = new_layout,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .image = image->image,
    .subresourceRange = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .baseMipLevel = mip_level,
      .levelCount = 1,
      .layerCount = 1,
    },
  };

  vkCmdPipelineBarrier (cmd, src_stage, dst_stage, 0,
                        0, NULL, 0, NULL, 1, &barrier);
}

static CoglPixelFormat
texture_storage_format (CoglPixelFormat format)
{
  /* Vulkan maps packed RGB to a four-channel image. Keep the Cogl image
   * format consistent so uploads, partial updates and readback use four
   * bytes per texel, while the public API can still convert packed RGB.
   */
  if (format == COGL_PIXEL_FORMAT_RGB_888)
    return COGL_PIXEL_FORMAT_RGBX_8888;
  if (format == COGL_PIXEL_FORMAT_BGR_888)
    return COGL_PIXEL_FORMAT_BGRX_8888;
  return format;
}

static gboolean
can_upload_linear (CoglVkDevice *device,
                   VkFormat      format)
{
  VkImageFormatProperties properties;

  return vkGetPhysicalDeviceImageFormatProperties (
    device->physical_device, format, VK_IMAGE_TYPE_2D,
    VK_IMAGE_TILING_LINEAR,
    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &properties) == VK_SUCCESS;
}

/* Staging + vkCmdCopyBufferToImage for any mip level (GL upload_subregion
 * parity).
 * Temporary HOST_VISIBLE staging buffer → optimal image (CoglBuffer
 * DEVICE_LOCAL
 * path is separate; see BUFFER-STAGING in cogl-buffer-impl-vk).
 */
static gboolean
upload_mip_level_via_staging (CoglTexture2DVk *tex_vk,
                              CoglVkDevice    *device,
                              CoglDriverVk    *driver_vk,
                              const uint8_t   *data,
                              int              rowstride,
                              uint32_t         bpp,
                              CoglPixelFormat  format,
                              VkFormat         vk_format,
                              int              src_x,
                              int              src_y,
                              int              width,
                              int              height,
                              int              dst_x,
                              int              dst_y,
                              int              level,
                              GError         **error)
{
  CoglTexture *tex = COGL_TEXTURE (tex_vk);
  CoglVkImage *src = tex_vk->image;
  CoglVkImage *dst = NULL;
  CoglVkImage *upload_img;
  gboolean replace = FALSE;
  uint32_t need_levels;
  uint32_t tex_w, tex_h;
  size_t staging_row;
  size_t staging_size;
  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory staging_mem = VK_NULL_HANDLE;
  VkBufferCreateInfo buf_info;
  VkMemoryRequirements reqs;
  VkMemoryAllocateInfo alloc_info;
  VkCommandBufferAllocateInfo cb_alloc;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkCommandPool cmd_pool = VK_NULL_HANDLE;
  VkCommandBufferBeginInfo begin_info = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  VkFenceCreateInfo fence_info = {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
  };
  VkFence fence = VK_NULL_HANDLE;
  VkSubmitInfo submit_info;
  VkBufferImageCopy region;
  void *mapped = NULL;
  int mem_type;
  int y;
  int level_w = 0, level_h = 0;
  VkResult res;
  gboolean ok = FALSE;

  if (src && (src->is_dmabuf || src->is_ycbcr))
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR,
                           COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "Mip upload not supported for dmabuf/ycbcr textures");
      return FALSE;
    }

  if (vk_format == VK_FORMAT_UNDEFINED)
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR,
                           COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "Unsupported format for Vulkan mip upload");
      return FALSE;
    }

  _cogl_texture_get_level_size (tex, level, &level_w, &level_h, NULL);
  if (dst_x < 0 || dst_y < 0 || width <= 0 || height <= 0 ||
      dst_x + width > level_w || dst_y + height > level_h)
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR,
                           COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "Mip upload region exceeds level size");
      return FALSE;
    }

  tex_w = src ? src->width : (uint32_t) cogl_texture_get_width (tex);
  tex_h = src ? src->height : (uint32_t) cogl_texture_get_height (tex);
  if (tex_w == 0 || tex_h == 0)
    {
      tex_w = (uint32_t) MAX (level_w << level, 1);
      tex_h = (uint32_t) MAX (level_h << level, 1);
    }

  need_levels = (uint32_t) level + 1;
  if (src && src->mip_levels > need_levels)
    need_levels = src->mip_levels;
  /* Do not expand to _cogl_texture_get_n_levels here — that allocated a
   * full UNDEFINED mip chain while only level 0 was uploaded, and
   * sampling then tripped VUID-vkCmdDraw-None-09600 (wrap-modes).
   * generate_mipmap upgrades the image when AUTO_MIPMAP needs it.
   */

  if (!src || src->host_visible || src->mip_levels < need_levels)
    {
      VkImageUsageFlags usage =
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;

      if (_cogl_texture_get_associated_framebuffers (tex))
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

      dst = cogl_vk_image_new_optimal_mips (device, tex_w, tex_h, need_levels,
                                            src ? src->format : vk_format,
                                            src ? src->cogl_format : format,
                                            usage, error);
      if (!dst)
        return FALSE;
      replace = TRUE;
    }

  upload_img = replace ? dst : src;

  staging_row = (size_t) width * bpp;
  staging_size = staging_row * (size_t) height;

  if (!flush_current_pass_for_upload (driver_vk, error))
    {
      g_clear_pointer (&dst, cogl_vk_image_free);
      return FALSE;
    }

  memset (&buf_info, 0, sizeof (buf_info));
  buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buf_info.size = staging_size;
  buf_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  res = vkCreateBuffer (device->device, &buf_info, NULL, &staging);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkCreateBuffer (mip upload) failed (%d)", (int) res);
      g_clear_pointer (&dst, cogl_vk_image_free);
      return FALSE;
    }

  vkGetBufferMemoryRequirements (device->device, staging, &reqs);
  mem_type = find_host_visible_mem_type (device, reqs.memoryTypeBits);
  if (mem_type < 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "No HOST_VISIBLE memory for mip upload staging");
      goto out;
    }

  memset (&alloc_info, 0, sizeof (alloc_info));
  alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc_info.allocationSize = reqs.size;
  alloc_info.memoryTypeIndex = (uint32_t) mem_type;
  res = vkAllocateMemory (device->device, &alloc_info, NULL, &staging_mem);
  if (res != VK_SUCCESS)
    goto out;
  res = vkBindBufferMemory (device->device, staging, staging_mem, 0);
  if (res != VK_SUCCESS)
    goto out;

  res = vkMapMemory (device->device, staging_mem, 0, staging_size, 0, &mapped);
  if (res != VK_SUCCESS)
    goto out;

  for (y = 0; y < height; y++)
    {
      memcpy ((uint8_t *) mapped + (size_t) y * staging_row,
              data + (src_y + y) * rowstride + src_x * (int) bpp,
              staging_row);
    }
  vkUnmapMemory (device->device, staging_mem);
  mapped = NULL;

  cmd_pool = device->oneshot_command_pool != VK_NULL_HANDLE
    ? device->oneshot_command_pool
    : device->command_pool;

  memset (&cb_alloc, 0, sizeof (cb_alloc));
  cb_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cb_alloc.commandPool = cmd_pool;
  cb_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cb_alloc.commandBufferCount = 1;
  res = vkAllocateCommandBuffers (device->device, &cb_alloc, &cmd);
  if (res != VK_SUCCESS)
    goto out;

  res = vkBeginCommandBuffer (cmd, &begin_info);
  if (res != VK_SUCCESS)
    goto out;

  /* Preserve base level when upgrading from an existing image. */
  if (replace && src)
    {
      VkImageCopy copy = {0};

      cogl_vk_image_cmd_barrier (src, cmd,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_TRANSFER_BIT |
                                 VK_PIPELINE_STAGE_HOST_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_ACCESS_SHADER_READ_BIT |
                                 VK_ACCESS_TRANSFER_WRITE_BIT |
                                 VK_ACCESS_HOST_WRITE_BIT,
                                 VK_ACCESS_TRANSFER_READ_BIT);
      cmd_barrier_mip_level (upload_img, cmd, 0,
                             VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, VK_ACCESS_TRANSFER_WRITE_BIT);

      copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      copy.srcSubresource.mipLevel = 0;
      copy.srcSubresource.layerCount = 1;
      copy.dstSubresource = copy.srcSubresource;
      copy.extent.width = src->width;
      copy.extent.height = src->height;
      copy.extent.depth = 1;
      vkCmdCopyImage (cmd,
                      src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      upload_img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      1, &copy);

      if (level == 0)
        {
          /* Base already DST; subregion overwrite below. */
        }
      else
        {
          cmd_barrier_mip_level (upload_img, cmd, 0,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_ACCESS_TRANSFER_WRITE_BIT,
                                 VK_ACCESS_SHADER_READ_BIT);
        }
    }

  {
    VkImageLayout old_layout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (!replace && upload_img->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
      old_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    else if (!replace &&
             upload_img->layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
      old_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    else if (replace && level == 0 && src)
      old_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
      {
        /* Preserving the old base level and then updating a subregion
         * are two writes to the same texels, despite the same layout.
         */
        cmd_barrier_mip_level (upload_img, cmd, (uint32_t) level,
                               old_layout, old_layout,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_ACCESS_TRANSFER_WRITE_BIT,
                               VK_ACCESS_TRANSFER_WRITE_BIT);
      }
    else
      {
        cmd_barrier_mip_level (upload_img, cmd, (uint32_t) level,
                               old_layout,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               (old_layout == VK_IMAGE_LAYOUT_UNDEFINED)
                               ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                               : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                               ? VK_ACCESS_SHADER_READ_BIT : 0,
                               VK_ACCESS_TRANSFER_WRITE_BIT);
      }
  }

  memset (&region, 0, sizeof (region));
  region.bufferOffset = 0;
  region.bufferRowLength = 0;
  region.bufferImageHeight = 0;
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.mipLevel = (uint32_t) level;
  region.imageSubresource.layerCount = 1;
  region.imageOffset.x = dst_x;
  region.imageOffset.y = dst_y;
  region.imageExtent.width = (uint32_t) width;
  region.imageExtent.height = (uint32_t) height;
  region.imageExtent.depth = 1;

  vkCmdCopyBufferToImage (cmd, staging,
                          upload_img->image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          1, &region);

  cmd_barrier_mip_level (upload_img, cmd, (uint32_t) level,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT);
  upload_img->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  res = vkEndCommandBuffer (cmd);
  if (res != VK_SUCCESS)
    goto out;

  res = vkCreateFence (device->device, &fence_info, NULL, &fence);
  if (res != VK_SUCCESS)
    goto out;

  memset (&submit_info, 0, sizeof (submit_info));
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &cmd;
  res = vkQueueSubmit (device->graphics_queue, 1, &submit_info, fence);
  if (res != VK_SUCCESS)
    goto out;

  res = vkWaitForFences (device->device, 1, &fence, VK_TRUE, UINT64_MAX);
  if (res != VK_SUCCESS)
    goto out;

  if (replace)
    {
      if (tex_vk->descriptor_set != VK_NULL_HANDLE &&
          driver_vk->pipeline_cache)
        {
          cogl_vk_pipeline_free_texture_ds (driver_vk->pipeline_cache,
                                            src && src->is_ycbcr,
                                            tex_vk->descriptor_set);
          tex_vk->descriptor_set = VK_NULL_HANDLE;
        }
      CoglVkImage *old_image = tex_vk->image;
      tex_vk->image = upload_img;
      cogl_texture_2d_vk_retire_replaced_image (tex_vk, old_image);
      dst = NULL;
    }

  if (cogl_texture_get_max_level_set (tex) < level)
    cogl_texture_set_max_level_set (tex, level);

  COGL_TEXTURE_2D (tex_vk)->internal_format = format;
  /* Optimal images use staging get_data (R4); keep flag set. */
  if (replace)
    COGL_TEXTURE_2D (tex_vk)->is_get_data_supported = TRUE;

  ok = TRUE;

out:
  if (fence != VK_NULL_HANDLE)
    vkDestroyFence (device->device, fence, NULL);
  if (cmd != VK_NULL_HANDLE && cmd_pool != VK_NULL_HANDLE)
    vkFreeCommandBuffers (device->device, cmd_pool, 1, &cmd);
  if (staging != VK_NULL_HANDLE)
    vkDestroyBuffer (device->device, staging, NULL);
  if (staging_mem != VK_NULL_HANDLE)
    vkFreeMemory (device->device, staging_mem, NULL);
  g_clear_pointer (&dst, cogl_vk_image_free);

  if (!ok && error && !*error)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Vulkan mip level upload failed");
  return ok;
}

static gboolean
cogl_texture_2d_vk_copy_from_bitmap (CoglTexture2D  *tex_2d,
                                     int             src_x,
                                     int             src_y,
                                     int             width,
                                     int             height,
                                     CoglBitmap     *bitmap,
                                     int             dst_x,
                                     int             dst_y,
                                     int             level,
                                     GError        **error)
{
  CoglTexture2DVk *tex_vk = COGL_TEXTURE_2D_VK (tex_2d);
  CoglTexture *tex = COGL_TEXTURE (tex_2d);
  CoglVkDevice *device = peek_vk_device_from_texture (tex);
  CoglDriverVk *driver_vk;
  CoglPixelFormat internal_format;
  CoglPixelFormat format;
  g_autoptr (CoglBitmap) upload_bmp = NULL;
  CoglBitmap *src_bmp;
  uint8_t *data;
  int rowstride;
  CoglVkImage *img;
  uint32_t bpp;
  gboolean use_staging;

  if (!device)
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR,
                           COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "Vulkan device missing");
      return FALSE;
    }

  driver_vk = COGL_DRIVER_VK (cogl_context_get_driver (
                                cogl_texture_get_context (tex)));

  internal_format = tex_vk->image ? tex_2d->internal_format :
                    _cogl_texture_determine_internal_format (tex, cogl_bitmap_get_format (bitmap));
  internal_format = texture_storage_format (internal_format);
  /* Vulkan buffer/image copies do not convert texel formats. */
  upload_bmp = _cogl_bitmap_convert (bitmap, internal_format, error);
  if (!upload_bmp)
    return FALSE;
  src_bmp = upload_bmp;

  format = cogl_bitmap_get_format (src_bmp);
  bpp = cogl_pixel_format_get_bytes_per_pixel (format, 0);
  data = cogl_bitmap_map (src_bmp, COGL_BUFFER_ACCESS_READ, 0, error);
  if (!data)
    return FALSE;

  rowstride = cogl_bitmap_get_rowstride (src_bmp);

  /* level≠0, or level0 into an already-optimal / multi-mip image → staging. */
  use_staging = (level != 0) ||
                !can_upload_linear (device, cogl_vk_format_from_cogl (format)) ||
                (tex_vk->image &&
                 (!tex_vk->image->host_visible ||
                  tex_vk->image->mip_levels > 1));

  if (use_staging)
    {
      gboolean ok;

      ok = upload_mip_level_via_staging (tex_vk, device, driver_vk,
                                         data, rowstride, bpp, format,
                                         cogl_vk_format_from_cogl (format),
                                         src_x, src_y, width, height,
                                         dst_x, dst_y, level, error);
      cogl_bitmap_unmap (src_bmp);
      return ok;
    }

  /* Full replace: recreate host-visible image (level 0 only). */
  if (src_x == 0 && src_y == 0 && dst_x == 0 && dst_y == 0 &&
      width == cogl_bitmap_get_width (src_bmp) &&
      height == cogl_bitmap_get_height (src_bmp) &&
      (!tex_vk->image ||
       (tex_vk->image->width == (uint32_t) width &&
        tex_vk->image->height == (uint32_t) height)))
    {
      img = cogl_vk_image_new_host_visible (device,
                                            (uint32_t) width,
                                            (uint32_t) height,
                                            cogl_vk_format_from_cogl (format),
                                            format,
                                            data,
                                            (uint32_t) rowstride,
                                            error);
      cogl_bitmap_unmap (src_bmp);
      if (!img)
        return FALSE;

      if (tex_vk->descriptor_set != VK_NULL_HANDLE)
        {
          if (driver_vk->pipeline_cache)
            cogl_vk_pipeline_free_texture_ds (driver_vk->pipeline_cache,
                                              tex_vk->image &&
                                              tex_vk->image->is_ycbcr,
                                              tex_vk->descriptor_set);
          tex_vk->descriptor_set = VK_NULL_HANDLE;
        }

      CoglVkImage *old_image = tex_vk->image;
      tex_vk->image = img;
      cogl_texture_2d_vk_retire_replaced_image (tex_vk, old_image);
      tex_2d->internal_format = format;
      return TRUE;
    }

  /* Partial update into existing host-visible linear image (mip level 0). */
  if (!tex_vk->image || !tex_vk->image->host_visible)
    {
      cogl_bitmap_unmap (src_bmp);
      g_set_error_literal (error, COGL_SYSTEM_ERROR,
                           COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "Partial upload needs host-visible Vulkan texture");
      return FALSE;
    }

  {
    VkImageSubresource sub = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .mipLevel = 0,
      .arrayLayer = 0,
    };
    VkSubresourceLayout layout;
    void *mapped = NULL;
    VkResult res;
    int y;

    /* The compositor may be sampling this texture in the pass being recorded
     * (a damaged client window is redrawn the same frame its buffer updates).
     * Writing the mapped image underneath that pass races the GPU read: a
     * scrolling terminal, which lands several damage rectangles per frame,
     * shows torn / jumbled rows. Submit the pass first so any sampling of the
     * old contents completes before the CPU overwrites them; the memory is
     * HOST_COHERENT, so the next pass sees the new bytes without a flush.
     */
    if (!flush_current_pass_for_upload (driver_vk, error))
      {
        cogl_bitmap_unmap (src_bmp);
        return FALSE;
      }
    /* Flush only submits. CPU writes must also wait for earlier sampling,
     * even if that pass was already submitted by a previous frame.
     */
    res = vkQueueWaitIdle (device->graphics_queue);
    cogl_vk_device_note_vk_result (device, res);
    if (res != VK_SUCCESS)
      {
        cogl_bitmap_unmap (src_bmp);
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Texture upload wait failed (%d)", (int) res);
        return FALSE;
      }

    vkGetImageSubresourceLayout (device->device, tex_vk->image->image, &sub,
                                 &layout);
    res = vkMapMemory (device->device, tex_vk->image->memories[0], 0,
                       VK_WHOLE_SIZE, 0, &mapped);
    if (res != VK_SUCCESS)
      {
        cogl_bitmap_unmap (src_bmp);
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "vkMapMemory failed (%d)", (int) res);
        return FALSE;
      }

    for (y = 0; y < height; y++)
      {
        memcpy ((uint8_t *) mapped + layout.offset +
                (size_t) (dst_y + y) * layout.rowPitch +
                (size_t) dst_x * bpp,
                data + (src_y + y) * rowstride + src_x * bpp,
                (size_t) width * bpp);
      }
    vkUnmapMemory (device->device, tex_vk->image->memories[0]);
  }

  cogl_bitmap_unmap (src_bmp);
  return TRUE;
}

static gboolean
allocate_with_size (CoglTexture2DVk   *tex_vk,
                    CoglTextureLoader *loader,
                    GError           **error)
{
  CoglTexture *tex = COGL_TEXTURE (tex_vk);
  CoglTexture2D *tex_2d = COGL_TEXTURE_2D (tex_vk);
  CoglVkDevice *device = peek_vk_device_from_texture (tex);
  CoglDriver *drv = cogl_context_get_driver (cogl_texture_get_context (tex));
  CoglDriverVk *driver_vk = COGL_IS_DRIVER_VK (drv) ? COGL_DRIVER_VK (drv) : NULL;
  CoglPixelFormat internal_format;
  VkFormat vk_format;

  if (!device)
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR, COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "No Vulkan device for texture allocate");
      return FALSE;
    }

  /* Match GL: sized loader often passes ANY; default is RGBA_8888_PRE so
   * get_data (…_PRE) does not wrongly straight→premult (journal 0x33→0x0a).
   */
  internal_format =
    _cogl_texture_determine_internal_format (tex, loader->src.sized.format);
  internal_format = texture_storage_format (internal_format);

  vk_format = cogl_vk_format_from_cogl (internal_format);
  if (vk_format == VK_FORMAT_UNDEFINED)
    vk_format = VK_FORMAT_R8G8B8A8_UNORM;

  /* DEVICE_LOCAL OPTIMAL for sized textures (incl. offscreen FBO targets).
   * Host-visible linear COLOR_ATTACHMENT caused Intel ANV DEVICE_LOST on
   * 512² offscreen + sample, and CPU map of host-visible after GPU render
   * returned stale init pixels (journal → #ffffffff). get_data uses staging.
   */
  if (driver_vk && driver_vk->current_pass)
    {
      g_autoptr (GError) flush_error = NULL;

      /* Submit any open CB before creating/replacing images — destroying a
       * VkImage still referenced by a recording CB invalidates it
       * (validation:
       * commandBuffer-recording / wrap-modes DEVICE_LOST).
       */
      if (!flush_current_pass_for_upload (driver_vk, &flush_error))
        {
          if (error && flush_error)
            g_propagate_error (error, g_steal_pointer (&flush_error));
          return FALSE;
        }
    }

  {
    VkImageUsageFlags usage =
      VK_IMAGE_USAGE_SAMPLED_BIT |
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
      VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    cogl_vk_debug_log (
      "tex",
      "allocate_with_size %dx%d → OPTIMAL usage=0x%x vk_fmt=%d cogl_fmt=%d "
      "(was host_visible linear) tex=%p pass=%p in_rp=%d",
      loader->src.sized.width, loader->src.sized.height,
      (unsigned) usage, (int) vk_format, (int) internal_format,
      (void *) tex_vk,
      driver_vk && driver_vk->current_pass
        ? (void *) driver_vk->current_pass : NULL,
      driver_vk && driver_vk->current_pass
        ? driver_vk->current_pass->in_render_pass : -1);

    tex_vk->image = cogl_vk_image_new_optimal (device,
                                               (uint32_t) loader->src.sized.width,
                                               (uint32_t) loader->src.sized.height,
                                               vk_format,
                                               internal_format,
                                               usage,
                                               error);
  }
  if (!tex_vk->image)
    return FALSE;

  /* Define contents out-of-band so draw CBs never Clear+LOAD from UNDEFINED
   * (that sequence DEVICE_LOST on Intel ANV; journal get_data QueueSubmit
   * -4).
   */
  if (!cogl_vk_image_oneshot_clear_to_color_attachment (tex_vk->image, error))
    {
      g_clear_pointer (&tex_vk->image, cogl_vk_image_free);
      return FALSE;
    }

  tex_2d->internal_format = internal_format;
  tex_2d->is_get_data_supported = TRUE;
  _cogl_texture_set_allocated (tex,
                               internal_format,
                               loader->src.sized.width,
                               loader->src.sized.height);
  return TRUE;
}

static gboolean
allocate_from_bitmap (CoglTexture2DVk   *tex_vk,
                      CoglTextureLoader *loader,
                      GError           **error)
{
  CoglTexture *tex = COGL_TEXTURE (tex_vk);
  CoglTexture2D *tex_2d = COGL_TEXTURE_2D (tex_vk);
  CoglVkDevice *device = peek_vk_device_from_texture (tex);
  CoglBitmap *bitmap = loader->src.bitmap.bitmap;
  g_autoptr (CoglBitmap) upload_bmp = NULL;
  CoglPixelFormat internal_format;
  CoglPixelFormat format;
  VkFormat vk_format;
  uint8_t *data;
  int width, height, rowstride;

  if (!device)
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR, COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "No Vulkan device for bitmap texture");
      return FALSE;
    }

  internal_format =
    _cogl_texture_determine_internal_format (tex, cogl_bitmap_get_format (bitmap));
  internal_format = texture_storage_format (internal_format);
  /* Vulkan buffer/image copies do not convert texel formats. */
  upload_bmp = _cogl_bitmap_convert (bitmap, internal_format, error);
  if (!upload_bmp)
    return FALSE;

  width = cogl_bitmap_get_width (upload_bmp);
  height = cogl_bitmap_get_height (upload_bmp);
  format = cogl_bitmap_get_format (upload_bmp);
  rowstride = cogl_bitmap_get_rowstride (upload_bmp);
  vk_format = cogl_vk_format_from_cogl (format);
  if (vk_format == VK_FORMAT_UNDEFINED)
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR, COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "Unsupported bitmap format for Vulkan");
      return FALSE;
    }

  data = cogl_bitmap_map (upload_bmp, COGL_BUFFER_ACCESS_READ, 0, error);
  if (!data)
    return FALSE;

  if (can_upload_linear (device, vk_format))
    {
      tex_vk->image = cogl_vk_image_new_host_visible (device,
                                                      (uint32_t) width,
                                                      (uint32_t) height,
                                                      vk_format,
                                                      format,
                                                      data,
                                                      (uint32_t) rowstride,
                                                      error);
    }
  else
    {
      CoglDriverVk *driver_vk = COGL_DRIVER_VK (
        cogl_context_get_driver (cogl_texture_get_context (tex)));

      if (!upload_mip_level_via_staging (
            tex_vk, device, driver_vk, data, rowstride,
            cogl_pixel_format_get_bytes_per_pixel (format, 0), format,
            vk_format, 0, 0, width, height, 0, 0, 0, error))
        {
          cogl_bitmap_unmap (upload_bmp);
          return FALSE;
        }
    }
  cogl_bitmap_unmap (upload_bmp);

  if (!tex_vk->image)
    return FALSE;

  tex_2d->internal_format = format;
  tex_2d->is_get_data_supported = TRUE;
  _cogl_texture_set_allocated (tex, format, width, height);
  return TRUE;
}

#if defined (HAVE_EGL) && defined (EGL_KHR_image_base)
static gboolean
allocate_from_egl_image (CoglTexture2DVk   *tex_vk,
                         CoglTextureLoader *loader,
                         GError           **error)
{
  CoglTexture *tex = COGL_TEXTURE (tex_vk);
  CoglTexture2D *tex_2d = COGL_TEXTURE_2D (tex_vk);
  CoglContext *ctx = cogl_texture_get_context (tex);
  CoglRenderer *renderer = cogl_context_get_renderer (ctx);
  CoglVkDevice *device = peek_vk_device_from_texture (tex);
  CoglVkPipelineCache *cache;
  EGLDisplay edpy = EGL_NO_DISPLAY;
  PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC query_fn = NULL;
  PFNEGLEXPORTDMABUFIMAGEMESAPROC export_fn = NULL;
  const char *exts;
  int fourcc = 0;
  int n_planes = 0;
  EGLuint64KHR modifiers[4] = { 0, 0, 0, 0 };
  int fds[4] = { -1, -1, -1, -1 };
  EGLint strides_i[4] = { 0, 0, 0, 0 };
  EGLint offsets_i[4] = { 0, 0, 0, 0 };
  uint32_t strides[4];
  uint32_t offsets[4];
  uint64_t mods[4];
  VkSamplerYcbcrConversion ycbcr = VK_NULL_HANDLE;
  int i;

  if (!device)
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR,
                           COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "No Vulkan device for EGLImage texture");
      return FALSE;
    }

  if (!COGL_IS_RENDERER_EGL (renderer))
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR,
                           COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "EGLImage import requires an EGL renderer");
      return FALSE;
    }

  edpy = cogl_renderer_egl_get_edisplay (COGL_RENDERER_EGL (renderer));
  if (edpy == EGL_NO_DISPLAY)
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR,
                           COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "No EGLDisplay for EGLImage export");
      return FALSE;
    }

  exts = eglQueryString (edpy, EGL_EXTENSIONS);
  if (!exts || !strstr (exts, "EGL_MESA_image_dma_buf_export"))
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR,
                           COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "EGL_MESA_image_dma_buf_export unavailable");
      return FALSE;
    }

  query_fn = (PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC)
             eglGetProcAddress ("eglExportDMABUFImageQueryMESA");
  export_fn = (PFNEGLEXPORTDMABUFIMAGEMESAPROC)
              eglGetProcAddress ("eglExportDMABUFImageMESA");
  if (!query_fn || !export_fn)
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR,
                           COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "eglExportDMABUFImageMESA entry points missing");
      return FALSE;
    }

  if (!query_fn (edpy, loader->src.egl_image.image,
                 &fourcc, &n_planes, modifiers) ||
      n_planes < 1 || n_planes > 4)
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR,
                           COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "eglExportDMABUFImageQueryMESA failed");
      return FALSE;
    }

  if (!export_fn (edpy, loader->src.egl_image.image,
                  fds, strides_i, offsets_i))
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR,
                           COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "eglExportDMABUFImageMESA failed");
      return FALSE;
    }

  for (i = 0; i < n_planes; i++)
    {
      if (fds[i] < 0)
        {
          int j;
          for (j = 0; j < n_planes; j++)
            {
              if (fds[j] >= 0)
                close (fds[j]);
            }
          g_set_error_literal (error, COGL_SYSTEM_ERROR,
                               COGL_SYSTEM_ERROR_UNSUPPORTED,
                               "EGLImage dmabuf export returned invalid fd");
          return FALSE;
        }
      strides[i] = (uint32_t) strides_i[i];
      offsets[i] = (uint32_t) offsets_i[i];
      mods[i] = (uint64_t) modifiers[i];
    }

  cache = cogl_driver_vk_peek_pipeline_cache (COGL_DRIVER_VK (
                                                cogl_context_get_driver (ctx)));
  if (cache && (uint32_t) fourcc == DRM_FORMAT_NV12)
    ycbcr = cogl_vk_pipeline_cache_peek_ycbcr_conversion (cache, 0);

  tex_vk->image = cogl_vk_image_import_dmabuf (device,
                                               (uint32_t) loader->src.egl_image.width,
                                               (uint32_t) loader->src.egl_image.height,
                                               (uint32_t) fourcc,
                                               n_planes,
                                               fds,
                                               strides,
                                               offsets,
                                               mods,
                                               ycbcr,
                                               error);

  for (i = 0; i < n_planes; i++)
    {
      if (fds[i] >= 0)
        close (fds[i]);
    }

  if (!tex_vk->image)
    return FALSE;

  tex_2d->internal_format = loader->src.egl_image.format;
  /* RGB dmabuf: staging. YCbCr/NV12: sample→RGBA then staging.
   * NO_GET_DATA still wins. Successful NV12 import already required
   * SamplerYcbcrConversion; keep FALSE only when that path is absent.
   */
  tex_2d->is_get_data_supported =
    !(loader->src.egl_image.flags & COGL_EGL_IMAGE_FLAG_NO_GET_DATA) &&
    (!tex_vk->image->is_ycbcr ||
     (device->has_sampler_ycbcr &&
      cache &&
      cogl_vk_pipeline_cache_peek_ycbcr_conversion (cache, 0) != VK_NULL_HANDLE));
  _cogl_texture_set_allocated (tex,
                               loader->src.egl_image.format,
                               loader->src.egl_image.width,
                               loader->src.egl_image.height);
  return TRUE;
}
#endif

static gboolean
cogl_texture_2d_vk_allocate (CoglTexture *tex,
                             GError     **error)
{
  CoglTexture2DVk *tex_vk = COGL_TEXTURE_2D_VK (tex);
  CoglTextureLoader *loader = cogl_texture_get_loader (tex);

  g_return_val_if_fail (loader, FALSE);

  switch (loader->src_type)
    {
    case COGL_TEXTURE_SOURCE_TYPE_SIZE:
      return allocate_with_size (tex_vk, loader, error);
    case COGL_TEXTURE_SOURCE_TYPE_BITMAP:
      return allocate_from_bitmap (tex_vk, loader, error);
    case COGL_TEXTURE_SOURCE_TYPE_EGL_IMAGE:
#if defined (HAVE_EGL) && defined (EGL_KHR_image_base)
      return allocate_from_egl_image (tex_vk, loader, error);
#else
      g_set_error_literal (error, COGL_SYSTEM_ERROR, COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "Vulkan EGLImage allocate requires HAVE_EGL");
      return FALSE;
#endif
    }

  g_return_val_if_reached (FALSE);
}

static void
cogl_texture_2d_vk_dispose (GObject *object)
{
  CoglTexture2DVk *tex_vk = COGL_TEXTURE_2D_VK (object);
  /* The dma-buf memo can keep textures alive until driver teardown, after
   * CoglContext has already disposed its display. The image's device stays
   * alive until the memo is cleared; its owner is NULL during teardown.
   */
  CoglDriverVk *driver_vk = tex_vk->image ? tex_vk->image->device->owner : NULL;

  if (driver_vk)
    {
      if (driver_vk->dmabuf_memo)
        cogl_vk_dmabuf_memo_remove_texture (driver_vk->dmabuf_memo,
                                            COGL_TEXTURE (tex_vk));

      if (tex_vk->descriptor_set != VK_NULL_HANDLE &&
          driver_vk->pipeline_cache)
        {
          gboolean ycbcr = tex_vk->image && tex_vk->image->is_ycbcr;
          cogl_vk_pipeline_free_texture_ds (driver_vk->pipeline_cache, ycbcr,
                                            tex_vk->descriptor_set);
        }
    }
  tex_vk->descriptor_set = VK_NULL_HANDLE;

  g_clear_pointer (&tex_vk->image, cogl_vk_image_free);
  if (tex_vk->pending_acquire_sync_fd >= 0)
    {
      close (tex_vk->pending_acquire_sync_fd);
      tex_vk->pending_acquire_sync_fd = -1;
    }

  G_OBJECT_CLASS (cogl_texture_2d_vk_parent_class)->dispose (object);
}

/* Sample NV12 through its YCbCr conversion into a temporary RGBA8 attachment.
 * Copy that attachment to a staging buffer and wait before returning pixels,
 * as for RGB texture readback.
 */
static gboolean
get_data_ycbcr_via_rgba (CoglTexture2DVk *tex_vk,
                         CoglPixelFormat  format,
                         int              rowstride,
                         uint8_t         *data)
{
  CoglTexture *tex = COGL_TEXTURE (tex_vk);
  CoglVkImage *src = tex_vk->image;
  CoglVkImage *rgba = NULL;
  CoglVkDevice *device;
  CoglDriverVk *driver_vk;
  CoglVkPipelineCache *cache;
  CoglVkRenderFormatSetup *setup;
  CoglContext *ctx;
  CoglDriver *driver;
  VkDescriptorSet src_ds;
  CoglVkPass pass;
  CoglVkVertPush vert;
  CoglPixelFormat src_format = COGL_PIXEL_FORMAT_RGBA_8888;
  uint32_t bpp;
  int width, height, y;
  size_t staging_row;
  size_t staging_size;
  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory staging_mem = VK_NULL_HANDLE;
  VkBufferCreateInfo buf_info;
  VkMemoryRequirements reqs;
  VkMemoryAllocateInfo alloc_info;
  VkCommandBufferAllocateInfo cb_alloc;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkCommandBufferBeginInfo begin_info = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  VkFenceCreateInfo fence_info = {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
  };
  VkFence fence = VK_NULL_HANDLE;
  VkSubmitInfo submit_info;
  VkBufferImageCopy region;
  void *mapped = NULL;
  int mem_type;
  unsigned int i;
  VkResult res;
  gboolean ok = FALSE;
  g_autoptr (GError) error = NULL;

  if (!src || !src->is_ycbcr)
    return FALSE;

  device = peek_vk_device_from_texture (tex);
  if (!device || !device->has_sampler_ycbcr)
    return FALSE;

  ctx = cogl_texture_get_context (tex);
  driver = cogl_context_get_driver (ctx);
  driver_vk = COGL_DRIVER_VK (driver);
  cache = cogl_driver_vk_peek_pipeline_cache (driver_vk);
  if (!cache ||
      cogl_vk_pipeline_cache_peek_ycbcr_conversion (cache, 0) == VK_NULL_HANDLE)
    return FALSE;

  if (format == COGL_PIXEL_FORMAT_ANY)
    format = src_format;

  bpp = cogl_pixel_format_get_bytes_per_pixel (src_format, 0);
  if (bpp == 0)
    return FALSE;

  width = cogl_texture_get_width (tex);
  height = cogl_texture_get_height (tex);
  staging_row = (size_t) width * bpp;
  staging_size = staging_row * (size_t) height;

  if (!flush_current_pass_for_upload (driver_vk, &error))
    return FALSE;

  src_ds = cogl_texture_2d_vk_ensure_descriptor_set (tex_vk, &error);
  if (src_ds == VK_NULL_HANDLE)
    return FALSE;

  rgba = cogl_vk_image_new_optimal (
    device, (uint32_t) width, (uint32_t) height,
    VK_FORMAT_R8G8B8A8_UNORM, src_format,
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
    VK_IMAGE_USAGE_SAMPLED_BIT,
    &error);
  if (!rgba)
    return FALSE;

  setup = cogl_vk_pipeline_cache_get_format_setup (
    cache, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED,
    VK_SAMPLE_COUNT_1_BIT, &error);
  if (!setup || setup->tex_pipe_ycbcr_opaque[src->ycbcr_index] == VK_NULL_HANDLE)
    {
      g_clear_pointer (&rgba, cogl_vk_image_free);
      return FALSE;
    }

  memset (&buf_info, 0, sizeof (buf_info));
  buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buf_info.size = staging_size;
  buf_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  res = vkCreateBuffer (device->device, &buf_info, NULL, &staging);
  if (res != VK_SUCCESS)
    {
      g_clear_pointer (&rgba, cogl_vk_image_free);
      return FALSE;
    }

  vkGetBufferMemoryRequirements (device->device, staging, &reqs);
  mem_type = find_host_visible_mem_type (device, reqs.memoryTypeBits);
  if (mem_type < 0)
    goto out;

  memset (&alloc_info, 0, sizeof (alloc_info));
  alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc_info.allocationSize = reqs.size;
  alloc_info.memoryTypeIndex = (uint32_t) mem_type;
  res = vkAllocateMemory (device->device, &alloc_info, NULL, &staging_mem);
  if (res != VK_SUCCESS)
    goto out;
  res = vkBindBufferMemory (device->device, staging, staging_mem, 0);
  if (res != VK_SUCCESS)
    goto out;

  memset (&cb_alloc, 0, sizeof (cb_alloc));
  cb_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cb_alloc.commandPool = device->command_pool;
  cb_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cb_alloc.commandBufferCount = 1;
  res = vkAllocateCommandBuffers (device->device, &cb_alloc, &cmd);
  if (res != VK_SUCCESS)
    goto out;

  res = vkBeginCommandBuffer (cmd, &begin_info);
  if (res != VK_SUCCESS)
    goto out;

  /* Lightweight pass wrapper so we can reuse cmd_draw_texture + foreign
   * acquire without going through async fence-ticket submit.
   */
  memset (&pass, 0, sizeof (pass));
  pass.device = device;
  pass.command_buffer = cmd;
  pass.recording = TRUE;
  pass.foreign_images = g_ptr_array_new ();
  pass.timestamp_begin = -1;
  pass.timestamp_end = -1;

  cogl_vk_vert_push_ortho_uv_unit (&vert, NULL, NULL);
  if (!cogl_vk_cmd_draw_texture (&pass, cache, setup, rgba, src, src_ds,
                                 1.f, TRUE, &vert, VK_NULL_HANDLE, NULL,
                                 &error))
    {
      cogl_vk_pass_end_render (&pass);
      for (i = 0; i < pass.foreign_images->len; i++)
        {
          CoglVkImage *img = g_ptr_array_index (pass.foreign_images, i);
          cogl_vk_image_cmd_foreign_release (
            img, cmd, device->graphics_queue_family, VK_IMAGE_LAYOUT_GENERAL);
        }
      g_clear_pointer (&pass.foreign_images, g_ptr_array_unref);
      goto out;
    }

  cogl_vk_pass_end_render (&pass);

  for (i = 0; i < pass.foreign_images->len; i++)
    {
      CoglVkImage *img = g_ptr_array_index (pass.foreign_images, i);
      cogl_vk_image_cmd_foreign_release (
        img, cmd, device->graphics_queue_family, VK_IMAGE_LAYOUT_GENERAL);
    }
  g_clear_pointer (&pass.foreign_images, g_ptr_array_unref);

  cogl_vk_image_cmd_barrier (rgba, cmd,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT);

  memset (&region, 0, sizeof (region));
  region.bufferOffset = 0;
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.layerCount = 1;
  region.imageExtent.width = (uint32_t) width;
  region.imageExtent.height = (uint32_t) height;
  region.imageExtent.depth = 1;

  vkCmdCopyImageToBuffer (cmd, rgba->image, rgba->layout,
                          staging, 1, &region);

  res = vkEndCommandBuffer (cmd);
  if (res != VK_SUCCESS)
    goto out;

  res = vkCreateFence (device->device, &fence_info, NULL, &fence);
  if (res != VK_SUCCESS)
    goto out;

  memset (&submit_info, 0, sizeof (submit_info));
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &cmd;
  res = vkQueueSubmit (device->graphics_queue, 1, &submit_info, fence);
  if (res != VK_SUCCESS)
    goto out;

  res = vkWaitForFences (device->device, 1, &fence, VK_TRUE, UINT64_MAX);
  if (res != VK_SUCCESS)
    goto out;

  res = vkMapMemory (device->device, staging_mem, 0, staging_size, 0, &mapped);
  if (res != VK_SUCCESS)
    goto out;

  if ((format & ~COGL_PREMULT_BIT) != (src_format & ~COGL_PREMULT_BIT) &&
      cogl_driver_has_feature (driver, COGL_FEATURE_ID_FORMAT_CONVERSION))
    {
      g_autoptr (CoglBitmap) src_bmp = NULL;
      g_autoptr (CoglBitmap) dst_bmp = NULL;
      uint8_t *tmp = g_malloc (staging_size);

      memcpy (tmp, mapped, staging_size);
      vkUnmapMemory (device->device, staging_mem);
      mapped = NULL;

      src_bmp = cogl_bitmap_new_for_data (ctx, width, height, src_format,
                                          (int) staging_row, tmp);
      dst_bmp = cogl_bitmap_new_for_data (ctx, width, height, format,
                                          rowstride, data);
      if (!_cogl_bitmap_convert_into_bitmap (src_bmp, dst_bmp, &error))
        {
          g_free (tmp);
          goto out;
        }
      g_free (tmp);
      ok = TRUE;
      goto out;
    }

  for (y = 0; y < height; y++)
    {
      memcpy (data + y * rowstride,
              (uint8_t *) mapped + (size_t) y * staging_row,
              staging_row);
    }
  vkUnmapMemory (device->device, staging_mem);
  mapped = NULL;
  ok = TRUE;

out:
  if (mapped)
    vkUnmapMemory (device->device, staging_mem);
  if (fence != VK_NULL_HANDLE)
    vkDestroyFence (device->device, fence, NULL);
  if (cmd != VK_NULL_HANDLE)
    vkFreeCommandBuffers (device->device, device->command_pool, 1, &cmd);
  if (staging != VK_NULL_HANDLE)
    vkDestroyBuffer (device->device, staging, NULL);
  if (staging_mem != VK_NULL_HANDLE)
    vkFreeMemory (device->device, staging_mem, NULL);
  g_clear_pointer (&rgba, cogl_vk_image_free);
  return ok;
}

static gboolean
get_data_via_staging (CoglTexture2DVk *tex_vk,
                      CoglPixelFormat  format,
                      int              rowstride,
                      uint8_t         *data)
{
  CoglTexture *tex = COGL_TEXTURE (tex_vk);
  CoglVkImage *img = tex_vk->image;
  CoglVkDevice *device;
  CoglDriverVk *driver_vk;
  CoglContext *ctx;
  CoglDriver *driver;
  CoglPixelFormat src_format;
  uint32_t bpp;
  int width, height, y;
  size_t staging_row;
  size_t staging_size;
  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory staging_mem = VK_NULL_HANDLE;
  VkBufferCreateInfo buf_info;
  VkMemoryRequirements reqs;
  VkMemoryAllocateInfo alloc_info;
  VkCommandBufferAllocateInfo cb_alloc;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkCommandBufferBeginInfo begin_info = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  VkFenceCreateInfo fence_info = {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
  };
  VkFence fence = VK_NULL_HANDLE;
  VkSubmitInfo submit_info;
  VkBufferImageCopy region;
  void *mapped = NULL;
  int mem_type;
  VkResult res;
  gboolean ok = FALSE;
  g_autoptr (GError) error = NULL;

  /* Cogl get_data has no level arg — always mip 0.
   * YCbCr/NV12 uses get_data_ycbcr_via_rgba; host-visible linear uses
   * the map path in cogl_texture_2d_vk_get_data.
   */
  if (!img || img->host_visible || img->is_ycbcr)
    {
      cogl_vk_debug_log (
        "tex",
        "get_data_via_staging SKIP img=%p host_vis=%d ycbcr=%d",
        (void *) img,
        img ? img->host_visible : -1,
        img ? img->is_ycbcr : -1);
      return FALSE;
    }

  device = peek_vk_device_from_texture (tex);
  if (!device)
    return FALSE;

  ctx = cogl_texture_get_context (tex);
  driver = cogl_context_get_driver (ctx);
  driver_vk = COGL_DRIVER_VK (driver);

  /* Dmabuf imports leave img->cogl_format unset; use texture format. */
  if (img->cogl_format != COGL_PIXEL_FORMAT_ANY)
    src_format = img->cogl_format;
  else
    {
      src_format = cogl_texture_get_format (tex);
      if (src_format == COGL_PIXEL_FORMAT_ANY)
        src_format = COGL_PIXEL_FORMAT_RGBA_8888;
    }

  if (format == COGL_PIXEL_FORMAT_ANY)
    format = src_format;

  bpp = cogl_pixel_format_get_bytes_per_pixel (src_format, 0);
  if (bpp == 0)
    return FALSE;

  width = cogl_texture_get_width (tex);
  height = cogl_texture_get_height (tex);
  staging_row = (size_t) width * bpp;
  staging_size = staging_row * (size_t) height;

  if (!flush_current_pass_for_upload (driver_vk, &error))
    {
      cogl_vk_debug_log (
        "tex",
        "get_data_via_staging flush FAIL: %s",
        error ? error->message : "?");
      return FALSE;
    }

  cogl_vk_debug_log (
    "tex",
    "get_data_via_staging BEGIN img=%p %dx%d layout=%d src_fmt=%d dst_fmt=%d",
    (void *) img, width, height, (int) img->layout,
    (int) src_format, (int) format);

  memset (&buf_info, 0, sizeof (buf_info));
  buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buf_info.size = staging_size;
  buf_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  res = vkCreateBuffer (device->device, &buf_info, NULL, &staging);
  if (res != VK_SUCCESS)
    return FALSE;

  vkGetBufferMemoryRequirements (device->device, staging, &reqs);
  mem_type = find_host_visible_mem_type (device, reqs.memoryTypeBits);
  if (mem_type < 0)
    goto out;

  memset (&alloc_info, 0, sizeof (alloc_info));
  alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc_info.allocationSize = reqs.size;
  alloc_info.memoryTypeIndex = (uint32_t) mem_type;
  res = vkAllocateMemory (device->device, &alloc_info, NULL, &staging_mem);
  if (res != VK_SUCCESS)
    goto out;
  res = vkBindBufferMemory (device->device, staging, staging_mem, 0);
  if (res != VK_SUCCESS)
    goto out;

  memset (&cb_alloc, 0, sizeof (cb_alloc));
  cb_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cb_alloc.commandPool = device->command_pool;
  cb_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cb_alloc.commandBufferCount = 1;
  res = vkAllocateCommandBuffers (device->device, &cb_alloc, &cmd);
  if (res != VK_SUCCESS)
    goto out;

  res = vkBeginCommandBuffer (cmd, &begin_info);
  if (res != VK_SUCCESS)
    goto out;

  if (img->is_dmabuf)
    {
      /* Own image for TRANSFER_SRC (foreign acquire if supported). */
      cogl_vk_image_cmd_foreign_acquire (
        img, cmd, device->graphics_queue_family,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
  else
    {
      cogl_vk_image_cmd_barrier (img, cmd,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_TRANSFER_BIT |
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_ACCESS_SHADER_READ_BIT |
                                 VK_ACCESS_TRANSFER_WRITE_BIT |
                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                 VK_ACCESS_TRANSFER_READ_BIT);
    }

  memset (&region, 0, sizeof (region));
  region.bufferOffset = 0;
  region.bufferRowLength = 0;
  region.bufferImageHeight = 0;
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.layerCount = 1;
  region.imageExtent.width = (uint32_t) width;
  region.imageExtent.height = (uint32_t) height;
  region.imageExtent.depth = 1;

  vkCmdCopyImageToBuffer (cmd, img->image, img->layout,
                          staging, 1, &region);

  /* Back to SHADER_READ_ONLY for later sampling. */
  cogl_vk_image_cmd_barrier (img, cmd,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT,
                             VK_ACCESS_SHADER_READ_BIT);

  if (img->is_dmabuf)
    {
      /* Match pass submit: release ownership to FOREIGN → GENERAL. */
      cogl_vk_image_cmd_foreign_release (
        img, cmd, device->graphics_queue_family,
        VK_IMAGE_LAYOUT_GENERAL);
    }

  res = vkEndCommandBuffer (cmd);
  if (res != VK_SUCCESS)
    {
      cogl_vk_debug_log ("tex", "get_data_via_staging EndCB FAIL %d", (int) res);
      goto out;
    }

  res = vkCreateFence (device->device, &fence_info, NULL, &fence);
  if (res != VK_SUCCESS)
    {
      cogl_vk_debug_log ("tex", "get_data_via_staging CreateFence FAIL %d", (int) res);
      goto out;
    }

  memset (&submit_info, 0, sizeof (submit_info));
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &cmd;
  res = vkQueueSubmit (device->graphics_queue, 1, &submit_info, fence);
  if (res != VK_SUCCESS)
    {
      cogl_vk_debug_log ("tex", "get_data_via_staging QueueSubmit FAIL %d", (int) res);
      goto out;
    }

  res = vkWaitForFences (device->device, 1, &fence, VK_TRUE, UINT64_MAX);
  if (res != VK_SUCCESS)
    {
      cogl_vk_debug_log ("tex", "get_data_via_staging WaitFence FAIL %d", (int) res);
      goto out;
    }

  res = vkMapMemory (device->device, staging_mem, 0, staging_size, 0, &mapped);
  if (res != VK_SUCCESS)
    {
      cogl_vk_debug_log ("tex", "get_data_via_staging Map FAIL %d", (int) res);
      goto out;
    }

  if ((format & ~COGL_PREMULT_BIT) != (src_format & ~COGL_PREMULT_BIT) &&
      cogl_driver_has_feature (driver, COGL_FEATURE_ID_FORMAT_CONVERSION))
    {
      g_autoptr (CoglBitmap) src_bmp = NULL;
      g_autoptr (CoglBitmap) dst_bmp = NULL;
      uint8_t *tmp = g_malloc (staging_size);

      memcpy (tmp, mapped, staging_size);
      vkUnmapMemory (device->device, staging_mem);
      mapped = NULL;

      src_bmp = cogl_bitmap_new_for_data (ctx, width, height, src_format,
                                          (int) staging_row, tmp);
      dst_bmp = cogl_bitmap_new_for_data (ctx, width, height, format,
                                          rowstride, data);
      if (!_cogl_bitmap_convert_into_bitmap (src_bmp, dst_bmp, &error))
        {
          cogl_vk_debug_log (
            "tex",
            "get_data_via_staging convert FAIL: %s",
            error ? error->message : "?");
          g_free (tmp);
          goto out;
        }
      g_free (tmp);
      ok = TRUE;
      goto out;
    }

  cogl_vk_debug_log (
    "tex",
    "get_data_via_staging COPYOUT px0=%02x%02x%02x%02x row=%zu",
    ((uint8_t *) mapped)[0], ((uint8_t *) mapped)[1],
    ((uint8_t *) mapped)[2], ((uint8_t *) mapped)[3],
    staging_row);

  for (y = 0; y < height; y++)
    {
      memcpy (data + y * rowstride,
              (uint8_t *) mapped + (size_t) y * staging_row,
              staging_row);
    }
  vkUnmapMemory (device->device, staging_mem);
  mapped = NULL;
  ok = TRUE;

out:
  cogl_vk_debug_log (
    "tex",
    "get_data_via_staging END ok=%d img=%p layout_now=%d",
    ok, (void *) img, img ? (int) img->layout : -1);
  if (mapped)
    vkUnmapMemory (device->device, staging_mem);
  if (fence != VK_NULL_HANDLE)
    vkDestroyFence (device->device, fence, NULL);
  if (cmd != VK_NULL_HANDLE)
    vkFreeCommandBuffers (device->device, device->command_pool, 1, &cmd);
  if (staging != VK_NULL_HANDLE)
    vkDestroyBuffer (device->device, staging, NULL);
  if (staging_mem != VK_NULL_HANDLE)
    vkFreeMemory (device->device, staging_mem, NULL);
  return ok;
}

static gboolean
cogl_texture_2d_vk_get_data (CoglTexture     *tex,
                             CoglPixelFormat  format,
                             int              rowstride,
                             uint8_t         *data)
{
  CoglTexture2DVk *tex_vk = COGL_TEXTURE_2D_VK (tex);
  CoglVkDevice *device;
  CoglContext *ctx;
  CoglDriver *driver;
  /* Cogl get_data has no level arg — always mip 0. */
  VkImageSubresource sub = {
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .mipLevel = 0,
    .arrayLayer = 0,
  };
  VkSubresourceLayout layout;
  void *mapped = NULL;
  VkResult res;
  int y, height, width;
  uint32_t bpp;
  CoglPixelFormat src_format;
  g_autoptr (GError) error = NULL;

  if (!tex_vk->image)
    return FALSE;

  /* YCbCr/NV12: sample→RGBA intermediate then staging. */
  if (tex_vk->image->is_ycbcr)
    return get_data_ycbcr_via_rgba (tex_vk, format, rowstride, data);

  /* Optimal / DEVICE_LOCAL / dmabuf RGB: staging Image→Buffer (R4/P2). */
  if (!tex_vk->image->host_visible || tex_vk->image->is_dmabuf)
    return get_data_via_staging (tex_vk, format, rowstride, data);

  /* Host-visible linear level-0 fast path (unchanged). */
  device = peek_vk_device_from_texture (tex);
  if (!device)
    return FALSE;

  ctx = cogl_texture_get_context (tex);
  driver = cogl_context_get_driver (ctx);

  /* Flush any in-flight render that may still write this image before map. */
  if (COGL_IS_DRIVER_VK (driver))
    {
      CoglDriverVk *driver_vk = COGL_DRIVER_VK (driver);
      gboolean had_pass = (driver_vk->current_pass != NULL);

      if (!flush_current_pass_for_upload (driver_vk, &error))
        return FALSE;
      if (had_pass)
        {
          cogl_vk_debug_log (
            "tex",
            "get_data host_visible flushed current pass before map");
        }
    }

  /* A previously flushed pass may still use the mapped storage. Flush is
   * asynchronous; direct host access is the point that requires completion.
   */
  res = vkQueueWaitIdle (device->graphics_queue);
  cogl_vk_device_note_vk_result (device, res);
  if (res != VK_SUCCESS)
    return FALSE;

  src_format = (tex_vk->image->cogl_format != COGL_PIXEL_FORMAT_ANY)
    ? tex_vk->image->cogl_format
    : COGL_PIXEL_FORMAT_RGBA_8888;

  if (format == COGL_PIXEL_FORMAT_ANY)
    format = src_format;

  bpp = cogl_pixel_format_get_bytes_per_pixel (src_format, 0);
  width = cogl_texture_get_width (tex);
  height = cogl_texture_get_height (tex);
  vkGetImageSubresourceLayout (device->device, tex_vk->image->image, &sub,
                               &layout);
  res = vkMapMemory (device->device, tex_vk->image->memories[0], 0,
                     VK_WHOLE_SIZE, 0, &mapped);
  if (res != VK_SUCCESS)
    return FALSE;

  if ((format & ~COGL_PREMULT_BIT) != (src_format & ~COGL_PREMULT_BIT) &&
      cogl_driver_has_feature (driver, COGL_FEATURE_ID_FORMAT_CONVERSION))
    {
      g_autoptr (CoglBitmap) src_bmp = NULL;
      g_autoptr (CoglBitmap) dst_bmp = NULL;
      uint8_t *tmp;
      int src_stride = (int) layout.rowPitch;

      tmp = g_malloc ((gsize) src_stride * (gsize) height);
      for (y = 0; y < height; y++)
        {
          memcpy (tmp + (gsize) y * src_stride,
                  (uint8_t *) mapped + layout.offset + (gsize) y * layout.rowPitch,
                  (gsize) width * bpp);
        }
      vkUnmapMemory (device->device, tex_vk->image->memories[0]);

      src_bmp = cogl_bitmap_new_for_data (ctx, width, height, src_format,
                                          src_stride, tmp);
      dst_bmp = cogl_bitmap_new_for_data (ctx, width, height, format,
                                          rowstride, data);
      if (!_cogl_bitmap_convert_into_bitmap (src_bmp, dst_bmp, &error))
        {
          g_free (tmp);
          return FALSE;
        }
      g_free (tmp);
      return TRUE;
    }

  for (y = 0; y < height; y++)
    {
      memcpy (data + y * rowstride,
              (uint8_t *) mapped + layout.offset + y * layout.rowPitch,
              (size_t) width * bpp);
    }
  vkUnmapMemory (device->device, tex_vk->image->memories[0]);
  return TRUE;
}

static void
cogl_texture_2d_vk_class_init (CoglTexture2DVkClass *klass)
{
  GObjectClass *object_klass = G_OBJECT_CLASS (klass);
  CoglTextureClass *tex_klass = COGL_TEXTURE_CLASS (klass);
  CoglTexture2DClass *tex_2d_klass = COGL_TEXTURE_2D_CLASS (klass);

  object_klass->dispose = cogl_texture_2d_vk_dispose;
  tex_klass->allocate = cogl_texture_2d_vk_allocate;
  tex_klass->get_data = cogl_texture_2d_vk_get_data;
  tex_klass->pre_paint = cogl_texture_2d_vk_pre_paint;
  tex_2d_klass->generate_mipmap = cogl_texture_2d_vk_generate_mipmap;
  tex_2d_klass->copy_from_framebuffer = cogl_texture_2d_vk_copy_from_framebuffer;
  tex_2d_klass->copy_from_bitmap = cogl_texture_2d_vk_copy_from_bitmap;
}

static void
cogl_texture_2d_vk_init (CoglTexture2DVk *tex)
{
  tex->image = NULL;
  tex->descriptor_set = VK_NULL_HANDLE;
  tex->pending_acquire_sync_fd = -1;
}

CoglTexture *
cogl_texture_2d_vk_new_from_dmabuf (CoglContext    *ctx,
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
                                    GError        **error)
{
  CoglDriver *driver;
  CoglVkDevice *device;
  CoglTexture2DVk *tex_vk;
  CoglTextureLoader *loader;
  CoglVkPipelineCache *cache;
  VkSamplerYcbcrConversion ycbcr = VK_NULL_HANDLE;

  const CoglVkDmabufImport import = {
    width, height, format, drm_fourcc, n_planes,
    fds, strides, offsets, modifiers, matrix * 2 + range
  };

  g_return_val_if_fail (ctx != NULL, NULL);
  if ((unsigned) matrix > COGL_YUV_MATRIX_BT2020 ||
      (unsigned) range > COGL_YUV_RANGE_FULL)
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR, COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "Unsupported YUV matrix or range");
      return NULL;
    }

  driver = cogl_context_get_driver (ctx);
  if (!COGL_IS_DRIVER_VK (driver))
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR, COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "dmabuf import requires COGL_DRIVER=vulkan");
      return NULL;
    }

  device = cogl_driver_vk_peek_device (COGL_DRIVER_VK (driver));
  if (!device)
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR, COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "Vulkan device not initialized");
      return NULL;
    }

  /* Reuse an import only when every plane and its color interpretation match.
   */
  if (n_planes > 0 && fds &&
      COGL_DRIVER_VK (driver)->dmabuf_memo)
    {
      CoglTexture *cached =
        cogl_vk_dmabuf_memo_lookup (COGL_DRIVER_VK (driver)->dmabuf_memo,
                                    &import);
      if (cached)
        return cached;
    }

  cache = cogl_driver_vk_peek_pipeline_cache (COGL_DRIVER_VK (driver));
  if (cache && drm_fourcc == DRM_FORMAT_NV12)
    ycbcr = cogl_vk_pipeline_cache_peek_ycbcr_conversion (cache, import.ycbcr_index);

  loader = cogl_texture_loader_new (COGL_TEXTURE_SOURCE_TYPE_SIZE);
  loader->src.sized.width = width;
  loader->src.sized.height = height;
  loader->src.sized.format = format;

  tex_vk = COGL_TEXTURE_2D_VK (_cogl_texture_2d_create_base (ctx,
                                                             width,
                                                             height,
                                                             format,
                                                             loader));
  tex_vk->image = cogl_vk_image_import_dmabuf (device,
                                               (uint32_t) width,
                                               (uint32_t) height,
                                               drm_fourcc,
                                               n_planes,
                                               fds,
                                               strides,
                                               offsets,
                                               modifiers,
                                               ycbcr,
                                               error);
  if (!tex_vk->image)
    {
      g_object_unref (tex_vk);
      return NULL;
    }

  tex_vk->image->ycbcr_index = import.ycbcr_index;
  COGL_TEXTURE_2D (tex_vk)->internal_format = format;
  /* RGB: staging. YCbCr/NV12: resolve→RGBA then staging when ICD has
   * SamplerYcbcrConversion (required for NV12 import already).
   */
  COGL_TEXTURE_2D (tex_vk)->is_get_data_supported =
    !tex_vk->image->is_ycbcr ||
    (device->has_sampler_ycbcr &&
     cache &&
     cogl_vk_pipeline_cache_peek_ycbcr_conversion (cache, import.ycbcr_index) != VK_NULL_HANDLE);
  _cogl_texture_set_allocated (COGL_TEXTURE (tex_vk), format, width, height);

  if (n_planes > 0 && fds && COGL_DRIVER_VK (driver)->dmabuf_memo)
    cogl_vk_dmabuf_memo_store (COGL_DRIVER_VK (driver)->dmabuf_memo,
                               &import, COGL_TEXTURE (tex_vk));

  return COGL_TEXTURE (tex_vk);
}

CoglTexture *
cogl_texture_2d_vk_new_from_image (CoglContext *ctx,
                                   CoglVkImage *image)
{
  CoglTextureLoader *loader = cogl_texture_loader_new (COGL_TEXTURE_SOURCE_TYPE_SIZE);
  CoglTexture2DVk *texture;
  CoglPixelFormat format = COGL_PIXEL_FORMAT_RGBA_8888_PRE;

  loader->src.sized.width = image->width;
  loader->src.sized.height = image->height;
  loader->src.sized.format = format;
  texture = COGL_TEXTURE_2D_VK (_cogl_texture_2d_create_base (ctx, image->width,
                                                              image->height, format,
                                                              loader));
  texture->image = image;
  COGL_TEXTURE_2D (texture)->internal_format = format;
  COGL_TEXTURE_2D (texture)->is_get_data_supported = TRUE;
  _cogl_texture_set_allocated (COGL_TEXTURE (texture), format,
                               image->width, image->height);
  return COGL_TEXTURE (texture);
}

gboolean
cogl_texture_2d_vk_set_yuv_colorimetry (CoglTexture2DVk *texture,
                                        CoglYuvMatrix    matrix,
                                        CoglYuvRange     range,
                                        GError         **error)
{
  CoglDriverVk *driver;
  CoglVkPipelineCache *cache;
  CoglVkImage *image;
  VkSamplerYcbcrConversion conversion;
  unsigned int index;
  VkResult result;

  if ((unsigned) matrix > COGL_YUV_MATRIX_BT2020 ||
      (unsigned) range > COGL_YUV_RANGE_FULL)
    goto unsupported;
  if (!cogl_texture_allocate (COGL_TEXTURE (texture), error))
    return FALSE;
  image = texture->image;
  if (!image || !image->is_ycbcr)
    goto unsupported;
  index = matrix * 2 + range;
  if (image->ycbcr_index == index)
    return TRUE;
  driver = COGL_DRIVER_VK (cogl_context_get_driver (
                             cogl_texture_get_context (COGL_TEXTURE (texture))));
  cache = cogl_driver_vk_peek_pipeline_cache (driver);
  conversion = cogl_vk_pipeline_cache_peek_ycbcr_conversion (cache, index);
  if (!conversion)
    goto unsupported;
  cogl_context_flush (cogl_texture_get_context (COGL_TEXTURE (texture)));
  if (!flush_current_pass_for_upload (driver, error))
    return FALSE;
  result = vkQueueWaitIdle (image->device->graphics_queue);
  if (result != VK_SUCCESS)
    {
      cogl_vk_device_note_vk_result (image->device, result);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Waiting before changing YUV colorimetry failed (%d)", result);
      return FALSE;
    }
  if (!cogl_vk_image_set_ycbcr_conversion (image, conversion, error))
    return FALSE;
  cogl_vk_pipeline_free_texture_ds (cache, image->ycbcr_index + 1,
                                    texture->descriptor_set);
  texture->descriptor_set = VK_NULL_HANDLE;
  image->ycbcr_index = index;
  /* The original import key no longer describes this mutable texture. */
  cogl_vk_dmabuf_memo_remove_texture (driver->dmabuf_memo, COGL_TEXTURE (texture));
  return TRUE;

unsupported:
  g_set_error_literal (error, COGL_SYSTEM_ERROR, COGL_SYSTEM_ERROR_UNSUPPORTED,
                       "Colorimetry requires NV12 and a supported YUV matrix/range");
  return FALSE;
}

static GType
cogl_texture_driver_vk_texture_2d_get_type (CoglTextureDriver *driver)
{
  (void) driver;
  return COGL_TYPE_TEXTURE_2D_VK;
}

static gboolean
cogl_texture_driver_vk_texture_2d_can_create (CoglTextureDriver *driver,
                                              CoglContext       *ctx,
                                              int                width,
                                              int                height,
                                              CoglPixelFormat    internal_format)
{
  (void) driver;
  (void) ctx;
  (void) width;
  (void) height;

  return cogl_vk_format_from_cogl (internal_format) != VK_FORMAT_UNDEFINED ||
         internal_format == COGL_PIXEL_FORMAT_RGBA_8888_PRE ||
         internal_format == COGL_PIXEL_FORMAT_RGBA_8888;
}

static CoglPixelFormat
cogl_texture_driver_vk_find_best_get_data_format (CoglTextureDriver *driver,
                                                  CoglContext       *context,
                                                  CoglPixelFormat    format)
{
  (void) driver;
  (void) context;
  return format;
}

CoglTexture2DVk *
cogl_texture_2d_vk_resolve (CoglTexture *tex)
{
  CoglTexture2D *leaf;

  if (!tex)
    return NULL;

  if (COGL_IS_TEXTURE_2D_VK (tex))
    return COGL_TEXTURE_2D_VK (tex);

  leaf = cogl_texture_get_first_leaf (tex);
  if (leaf && COGL_IS_TEXTURE_2D_VK (leaf))
    return COGL_TEXTURE_2D_VK (leaf);

  return NULL;
}

static void
cogl_texture_driver_vk_class_init (CoglTextureDriverVkClass *klass)
{
  CoglTextureDriverClass *driver_klass = COGL_TEXTURE_DRIVER_CLASS (klass);

  driver_klass->texture_2d_get_type = cogl_texture_driver_vk_texture_2d_get_type;
  driver_klass->texture_2d_can_create = cogl_texture_driver_vk_texture_2d_can_create;
  driver_klass->find_best_get_data_format =
    cogl_texture_driver_vk_find_best_get_data_format;
}

static void
cogl_texture_driver_vk_init (CoglTextureDriverVk *driver)
{
}
