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

#include "cogl/driver/vk/cogl-framebuffer-vk-private.h"
#include "cogl/driver/vk/cogl-driver-vk-private.h"
#include "cogl/driver/vk/cogl-texture-driver-vk-private.h"
#include "cogl/driver/vk/cogl-pipeline-vk-private.h"
#include "cogl/driver/vk/cogl-pipeline-vk-glsl-private.h"
#include "cogl/driver/vk/cogl-pipeline-vk-vbo-private.h"
#include "cogl/driver/vk/cogl-pass-vk-private.h"
#include "cogl/driver/vk/cogl-vk-debug-private.h"

#include "cogl/cogl-private.h"
#include "cogl/cogl-context-private.h"
#include "cogl/cogl-framebuffer-private.h"
#include "cogl/cogl-offscreen.h"
#include "cogl/cogl-offscreen-private.h"
#include "cogl/cogl-pipeline.h"
#include "cogl/cogl-pipeline-state.h"
#include "cogl/cogl-pipeline-layer-private.h"
#include "cogl/cogl-renderer-private.h"
#include "cogl/cogl-texture.h"
#include "cogl/cogl-pixel-format.h"
#include "cogl/cogl-bitmap-private.h"
#include "cogl/cogl-bitmap.h"
#include "cogl/cogl-attribute-private.h"
#include "cogl/cogl-attribute-buffer.h"
#include "cogl/cogl-buffer-private.h"
#include "cogl/cogl-indices-private.h"
#include "cogl/cogl-index-buffer.h"
#include "cogl/cogl-color.h"

#include <graphene.h>
#include <string.h>
#include <unistd.h>

#ifdef __GLIBC__
#include <execinfo.h>
#endif

/* VK-local CoglRenderbuffers (opaque to Meta). Shares DS RBs across
 * GBM flip FBOs like GL. Prefer packed when both depth+stencil needed;
 * depth-only / stencil-only when allocate flags say so. Same-driver
 * reattach only — cross-driver Meta/GL handles out of scope.
 */
struct _CoglRenderbuffers
{
  grefcount refcount;
  CoglVkImage *depth_stencil; /* packed; owned */
  CoglVkImage *depth;         /* depth-only; owned unless aliases packed */
  CoglVkImage *stencil;       /* stencil-only; owned unless aliases packed */
  VkImageView depth_view;
  VkImageView stencil_view;
  CoglOffscreenAllocateFlags flags;
  VkSampleCountFlagBits samples;
  uint32_t width;
  uint32_t height;
};

struct _CoglFramebufferVk
{
  CoglFramebufferDriver parent_instance;
  /* Borrowed from texture when offscreen; owned when created here. */
  CoglVkImage *color;
  gboolean owns_color;
  /* Packed depth/stencil attachment, or a separate depth or stencil image. */
  CoglVkImage *depth_stencil;
  /* Optional multisample color image, resolved when rendering is flushed. */
  CoglVkImage *ms_color;
  gboolean owns_ms_color;
  /* Shared DS RBs (own the images). */
  CoglRenderbuffers *renderbuffers;
  /* From CoglFramebufferDriverConfig (COGL_OFFSCREEN_DISABLE_DEPTH…). */
  gboolean disable_depth_and_stencil;
};

G_DEFINE_FINAL_TYPE (CoglFramebufferVk,
                     cogl_framebuffer_vk,
                     COGL_TYPE_FRAMEBUFFER_DRIVER)

CoglVkImage *
cogl_framebuffer_vk_peek_color_image (CoglFramebufferVk * fb)
{
  g_return_val_if_fail (COGL_IS_FRAMEBUFFER_DRIVER_VK (fb), NULL);
  return fb->color;
}

static CoglDriverVk *
peek_driver_vk (CoglFramebufferDriver *driver)
{
  CoglFramebuffer *fb = cogl_framebuffer_driver_get_framebuffer (driver);
  CoglContext *ctx = cogl_framebuffer_get_context (fb);
  CoglDriver *cogl_driver = cogl_context_get_driver (ctx);

  if (!COGL_IS_DRIVER_VK (cogl_driver))
    return NULL;
  return COGL_DRIVER_VK (cogl_driver);
}

static gboolean
ensure_color_image (CoglFramebufferVk *fb_vk)
{
  CoglFramebuffer *fb;
  CoglOffscreen *offscreen;

  if (fb_vk->color)
    return TRUE;

  fb = cogl_framebuffer_driver_get_framebuffer (COGL_FRAMEBUFFER_DRIVER (fb_vk));
  if (!COGL_IS_OFFSCREEN (fb))
    return FALSE;

  offscreen = COGL_OFFSCREEN (fb);
  if (COGL_IS_TEXTURE_2D_VK (cogl_offscreen_get_texture (offscreen)))
    {
      fb_vk->color =
        cogl_texture_2d_vk_peek_image (COGL_TEXTURE_2D_VK (
                                         cogl_offscreen_get_texture (offscreen)));
      fb_vk->owns_color = FALSE;
      return fb_vk->color != NULL;
    }

  return FALSE;
}

static void destroy_color_framebuffer (CoglVkImage *color);

static void
attach_ds_to_color_targets (CoglFramebufferVk *fb_vk)
{
  if (fb_vk->color)
    {
      if (fb_vk->color->framebuffer_ds != fb_vk->depth_stencil &&
          fb_vk->color->framebuffer != VK_NULL_HANDLE)
        destroy_color_framebuffer (fb_vk->color);
      fb_vk->color->attached_depth_stencil = fb_vk->depth_stencil;
    }
  if (fb_vk->ms_color)
    {
      if (fb_vk->ms_color->framebuffer_ds != fb_vk->depth_stencil &&
          fb_vk->ms_color->framebuffer != VK_NULL_HANDLE)
        destroy_color_framebuffer (fb_vk->ms_color);
      fb_vk->ms_color->attached_depth_stencil = fb_vk->depth_stencil;
    }
}

void
cogl_framebuffer_vk_replace_color_image (CoglFramebufferVk   *fb_vk,
                                         CoglVkImage         *old_image,
                                         CoglVkImage         *new_image)
{
  if (fb_vk->owns_color || fb_vk->color != old_image)
    return;

  /* Texture storage can change when generating or uploading mip levels.
   * Rebind borrowed attachments before the old image is retired.
   */
  fb_vk->color = new_image;
  if (old_image)
    old_image->attached_depth_stencil = NULL;
  attach_ds_to_color_targets (fb_vk);
}

void
cogl_framebuffer_vk_adopt_targets_from (CoglFramebufferVk *dst,
                                        CoglFramebufferVk *src)
{
  g_return_if_fail (COGL_IS_FRAMEBUFFER_DRIVER_VK (dst));
  g_return_if_fail (COGL_IS_FRAMEBUFFER_DRIVER_VK (src));

  /* Populate src color from Texture2DVk when still unset (BIND is a no-op). */
  if (!ensure_color_image (src))
    return;

  /* Idempotent if already pointing at the same targets. */
  if (dst->color == src->color &&
      dst->ms_color == src->ms_color &&
      dst->depth_stencil == src->depth_stencil)
    return;

  if (dst->color)
    dst->color->attached_depth_stencil = NULL;
  if (dst->ms_color)
    dst->ms_color->attached_depth_stencil = NULL;

  if (dst->owns_ms_color)
    g_clear_pointer (&dst->ms_color, cogl_vk_image_free);
  else
    dst->ms_color = NULL;

  if (dst->owns_color)
    g_clear_pointer (&dst->color, cogl_vk_image_free);
  else
    dst->color = NULL;

  /* Borrow src's images; do not steal ownership from src / Texture2DVk. */
  dst->color = src->color;
  dst->owns_color = FALSE;
  dst->ms_color = src->ms_color;
  dst->owns_ms_color = FALSE;
  dst->depth_stencil = src->depth_stencil;

  attach_ds_to_color_targets (dst);
}

static void
cogl_framebuffer_vk_adopt_targets (CoglFramebufferDriver *driver,
                                   CoglFramebufferDriver *source)
{
  g_return_if_fail (COGL_IS_FRAMEBUFFER_DRIVER_VK (source));

  cogl_framebuffer_vk_adopt_targets_from (COGL_FRAMEBUFFER_DRIVER_VK (driver),
                                          COGL_FRAMEBUFFER_DRIVER_VK (source));
}

static void
cogl_vk_renderbuffers_free (CoglRenderbuffers *rb)
{
  if (!rb)
    return;
  if (rb->depth_stencil)
    {
      CoglVkDevice *device = rb->depth_stencil->device;
      cogl_driver_vk_flush_current_pass (device->owner);
      vkQueueWaitIdle (device->graphics_queue);
      if (rb->depth_view)
        vkDestroyImageView (device->device, rb->depth_view, NULL);
      if (rb->stencil_view)
        vkDestroyImageView (device->device, rb->stencil_view, NULL);
    }
  if (rb->depth != rb->depth_stencil)
    g_clear_pointer (&rb->depth, cogl_vk_image_free);
  if (rb->stencil != rb->depth_stencil)
    g_clear_pointer (&rb->stencil, cogl_vk_image_free);
  g_clear_pointer (&rb->depth_stencil, cogl_vk_image_free);
  g_free (rb);
}

static CoglRenderbuffers *
cogl_vk_renderbuffers_ref (CoglRenderbuffers *rb)
{
  if (rb)
    g_ref_count_inc (&rb->refcount);
  return rb;
}

static void
cogl_vk_renderbuffers_unref (CoglRenderbuffers *rb)
{
  if (rb && g_ref_count_dec (&rb->refcount))
    cogl_vk_renderbuffers_free (rb);
}

/* Cogl shares renderbuffer sets as a whole. Independent depth/stencil
 * handles use disjoint aspect views of one Vulkan attachment, preserving
 * independent aspect contents without copying or discarding either one.
 */
static CoglVkImage *
renderbuffers_draw_ds (CoglRenderbuffers *rb)
{
  if (!rb)
    return NULL;
  if (rb->depth_stencil)
    return rb->depth_stencil;
  return rb->depth ? rb->depth : rb->stencil;
}

static CoglRenderbuffers *
cogl_vk_renderbuffers_new (CoglVkDevice               *device,
                           uint32_t                    width,
                           uint32_t                    height,
                           VkSampleCountFlagBits       samples,
                           CoglOffscreenAllocateFlags  flags,
                           GError                    **error)
{
  CoglRenderbuffers *rb;
  gboolean want_packed;
  gboolean want_depth;
  gboolean want_stencil;

  if (samples == 0)
    samples = VK_SAMPLE_COUNT_1_BIT;

  if (flags == 0)
    {
      rb = g_new0 (CoglRenderbuffers, 1);
      g_ref_count_init (&rb->refcount);
      rb->flags = 0;
      rb->samples = samples;
      rb->width = width;
      rb->height = height;
      return rb;
    }

  want_packed = !!(flags & COGL_OFFSCREEN_ALLOCATE_FLAG_DEPTH_STENCIL);
  want_depth = !want_packed &&
               !!(flags & COGL_OFFSCREEN_ALLOCATE_FLAG_DEPTH);
  want_stencil = !want_packed &&
                 !!(flags & COGL_OFFSCREEN_ALLOCATE_FLAG_STENCIL);

  /* Explicit packed and independent aspect storage are distinct choices. */
  g_assert (!(want_packed && (want_depth || want_stencil)));

  rb = g_new0 (CoglRenderbuffers, 1);
  g_ref_count_init (&rb->refcount);
  rb->flags = flags;
  rb->samples = samples;
  rb->width = width;
  rb->height = height;

  if (want_depth && want_stencil)
    {
      VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .subresourceRange = { .levelCount = 1, .layerCount = 1 },
      };
      VkResult result;

      rb->depth_stencil = cogl_vk_image_new_depth_stencil (device, width, height,
                                                           samples, error);
      if (!rb->depth_stencil)
        goto fail;
      rb->depth = rb->stencil = rb->depth_stencil;
      view_info.image = rb->depth_stencil->image;
      view_info.format = rb->depth_stencil->format;
      view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
      result = vkCreateImageView (device->device, &view_info, NULL, &rb->depth_view);
      if (result != VK_SUCCESS)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Creating independent depth view failed (%d)", result);
          goto fail;
        }
      view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
      result = vkCreateImageView (device->device, &view_info, NULL, &rb->stencil_view);
      if (result != VK_SUCCESS)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Creating independent stencil view failed (%d)", result);
          goto fail;
        }
    }
  else if (want_packed)
    {
      rb->depth_stencil =
        cogl_vk_image_new_depth_stencil (device, width, height, samples, error);
      if (!rb->depth_stencil)
        {
          cogl_vk_renderbuffers_free (rb);
          return NULL;
        }
    }
  else if (want_depth)
    {
      rb->depth =
        cogl_vk_image_new_depth (device, width, height, samples, error);
      if (!rb->depth)
        {
          cogl_vk_renderbuffers_free (rb);
          return NULL;
        }
    }
  else if (want_stencil)
    {
      g_autoptr (GError) stencil_error = NULL;

      rb->stencil =
        cogl_vk_image_new_stencil (device, width, height, samples,
                                   error ? error : &stencil_error);
      if (!rb->stencil)
        {
          cogl_vk_renderbuffers_free (rb);
          return NULL;
        }
    }

  return rb;

fail:
  cogl_vk_renderbuffers_free (rb);
  return NULL;
}

static gboolean
rb_matches (CoglRenderbuffers         *rb,
            uint32_t                   width,
            uint32_t                   height,
            VkSampleCountFlagBits      samples,
            CoglOffscreenAllocateFlags flags)
{
  return rb &&
         rb->samples == samples &&
         rb->width == width &&
         rb->height == height &&
         rb->flags == flags;
}

/* Try GL-ordered allocate flags; explicit independent storage is also
 * supported through the remembered allocation flags.
 */
static CoglRenderbuffers *
try_allocate_renderbuffers (CoglDriverVk          *driver_vk,
                            uint32_t               width,
                            uint32_t               height,
                            VkSampleCountFlagBits  samples,
                            GError               **error)
{
  static const CoglOffscreenAllocateFlags try_order[] = {
    COGL_OFFSCREEN_ALLOCATE_FLAG_DEPTH_STENCIL,
    COGL_OFFSCREEN_ALLOCATE_FLAG_STENCIL,
    COGL_OFFSCREEN_ALLOCATE_FLAG_DEPTH,
    0,
  };
  unsigned int i;

  if (driver_vk->have_last_offscreen_allocate_flags)
    {
      CoglRenderbuffers *rb;
      g_autoptr (GError) local = NULL;
      CoglOffscreenAllocateFlags last =
        (CoglOffscreenAllocateFlags) driver_vk->last_offscreen_allocate_flags;

      rb = cogl_vk_renderbuffers_new (driver_vk->vk_device, width, height,
                                      samples, last, &local);
      if (rb)
        {
          /* Retain the successful logical attachment allocation. */
          driver_vk->last_offscreen_allocate_flags = (unsigned int) rb->flags;
          return rb;
        }
    }

  for (i = 0; i < G_N_ELEMENTS (try_order); i++)
    {
      CoglRenderbuffers *rb;
      g_autoptr (GError) local = NULL;
      GError **errp = (i + 1 == G_N_ELEMENTS (try_order)) ? error : &local;

      rb = cogl_vk_renderbuffers_new (driver_vk->vk_device, width, height,
                                      samples, try_order[i], errp);
      if (rb)
        {
          driver_vk->last_offscreen_allocate_flags = (unsigned int) rb->flags;
          driver_vk->have_last_offscreen_allocate_flags = TRUE;
          return rb;
        }
    }

  return NULL;
}

static gboolean
ensure_depth_stencil (CoglFramebufferVk    *fb_vk,
                      CoglDriverVk         *driver_vk,
                      VkSampleCountFlagBits samples)
{
  CoglFramebuffer *fb;
  g_autoptr (GError) error = NULL;
  uint32_t width, height;
  CoglOffscreenAllocateFlags want_flags;

  if (fb_vk->disable_depth_and_stencil)
    {
      fb_vk->depth_stencil = NULL;
      attach_ds_to_color_targets (fb_vk);
      return FALSE;
    }

  if (samples == 0)
    samples = VK_SAMPLE_COUNT_1_BIT;

  if (!ensure_color_image (fb_vk) || !driver_vk || !driver_vk->vk_device)
    return FALSE;

  fb = cogl_framebuffer_driver_get_framebuffer (COGL_FRAMEBUFFER_DRIVER (fb_vk));
  if (!COGL_IS_OFFSCREEN (fb))
    {
      /* Onscreen may already hold a GBM-borrowed DS; attach it, but never
       * allocate new depth/stencil renderbuffers for onscreen.
       */
      if (fb_vk->depth_stencil != NULL)
        {
          attach_ds_to_color_targets (fb_vk);
          return TRUE;
        }
      return FALSE;
    }

  width = (uint32_t) cogl_framebuffer_get_width (fb);
  height = (uint32_t) cogl_framebuffer_get_height (fb);

  want_flags = driver_vk->have_last_offscreen_allocate_flags
    ? (CoglOffscreenAllocateFlags) driver_vk->last_offscreen_allocate_flags
    : COGL_OFFSCREEN_ALLOCATE_FLAG_DEPTH_STENCIL;

  if (fb_vk->renderbuffers)
    want_flags = fb_vk->renderbuffers->flags;

  if (rb_matches (fb_vk->renderbuffers, width, height, samples, want_flags) &&
      renderbuffers_draw_ds (fb_vk->renderbuffers))
    {
      fb_vk->depth_stencil = renderbuffers_draw_ds (fb_vk->renderbuffers);
      attach_ds_to_color_targets (fb_vk);
      return TRUE;
    }

  /* Drop mismatched RB (sample count / size / flags) before realloc. */
  if (fb_vk->renderbuffers)
    {
      destroy_color_framebuffer (fb_vk->color);
      destroy_color_framebuffer (fb_vk->ms_color);
      fb_vk->depth_stencil = NULL;
      attach_ds_to_color_targets (fb_vk);
      g_clear_pointer (&fb_vk->renderbuffers, cogl_vk_renderbuffers_unref);
    }

  fb_vk->renderbuffers =
    try_allocate_renderbuffers (driver_vk, width, height, samples, &error);
  if (!fb_vk->renderbuffers)
    {
      g_warning ("Cogl Vulkan: depth/stencil alloc failed: %s",
                 error ? error->message : "unknown");
      return FALSE;
    }

  fb_vk->depth_stencil = renderbuffers_draw_ds (fb_vk->renderbuffers);
  attach_ds_to_color_targets (fb_vk);
  return fb_vk->depth_stencil != NULL;
}

CoglVkImage *
cogl_framebuffer_vk_peek_depth_stencil (CoglFramebufferVk *fb)
{
  g_return_val_if_fail (COGL_IS_FRAMEBUFFER_DRIVER_VK (fb), NULL);
  return fb->depth_stencil;
}

static VkSampleCountFlagBits framebuffer_msaa_samples (CoglFramebufferVk *fb_vk);

gboolean
cogl_framebuffer_vk_ensure_depth_stencil (CoglFramebufferVk *fb)
{
  CoglDriverVk *driver_vk;
  VkSampleCountFlagBits samples;

  g_return_val_if_fail (COGL_IS_FRAMEBUFFER_DRIVER_VK (fb), FALSE);
  driver_vk = peek_driver_vk (COGL_FRAMEBUFFER_DRIVER (fb));
  samples = framebuffer_msaa_samples (fb);
  return ensure_depth_stencil (fb, driver_vk, samples);
}

void
cogl_framebuffer_vk_apply_driver_config (CoglFramebufferVk                 *fb,
                                         const CoglFramebufferDriverConfig *cfg)
{
  g_return_if_fail (COGL_IS_FRAMEBUFFER_DRIVER_VK (fb));
  fb->disable_depth_and_stencil =
    cfg && cfg->disable_depth_and_stencil;
}

static CoglVkPass * ensure_pass (CoglDriverVk *driver_vk);

/* MSAA samples source (no public Cogl getter/setter exists — GL offscreen
 * allocate never sets sample count; only disable_depth_and_stencil is in
 * CoglFramebufferDriverConfig):
 * 1) Color / ms_color image samples when already multisampled
 * 2) COGL_VK_MSAA env as fallback only (debug override when API is 1×)
 */
static VkSampleCountFlagBits
env_msaa_samples (void)
{
  const char *env = g_getenv ("COGL_VK_MSAA");
  int n;

  if (!env || !*env)
    return VK_SAMPLE_COUNT_1_BIT;
  n = (int) g_ascii_strtoll (env, NULL, 10);
  if (n >= 8)
    return VK_SAMPLE_COUNT_8_BIT;
  if (n >= 4)
    return VK_SAMPLE_COUNT_4_BIT;
  if (n >= 2)
    return VK_SAMPLE_COUNT_2_BIT;
  return VK_SAMPLE_COUNT_1_BIT;
}

static VkSampleCountFlagBits
framebuffer_msaa_samples (CoglFramebufferVk *fb_vk)
{
  VkSampleCountFlagBits api = VK_SAMPLE_COUNT_1_BIT;

  if (fb_vk->color && fb_vk->color->samples > VK_SAMPLE_COUNT_1_BIT)
    api = fb_vk->color->samples;
  else if (fb_vk->ms_color && fb_vk->ms_color->samples > VK_SAMPLE_COUNT_1_BIT)
    api = fb_vk->ms_color->samples;

  if (api > VK_SAMPLE_COUNT_1_BIT)
    return api;

  return env_msaa_samples ();
}

static CoglVkImage *
draw_color_target (CoglFramebufferVk *fb_vk,
                   CoglDriverVk      *driver_vk)
{
  VkSampleCountFlagBits samples = framebuffer_msaa_samples (fb_vk);
  CoglFramebuffer *fb;
  g_autoptr (GError) error = NULL;

  if (samples <= VK_SAMPLE_COUNT_1_BIT || !fb_vk->color)
    return fb_vk->color;

  if (fb_vk->ms_color &&
      fb_vk->ms_color->width == fb_vk->color->width &&
      fb_vk->ms_color->height == fb_vk->color->height &&
      fb_vk->ms_color->samples == samples)
    {
      fb_vk->ms_color->attached_depth_stencil = fb_vk->depth_stencil;
      return fb_vk->ms_color;
    }

  if (fb_vk->owns_ms_color)
    g_clear_pointer (&fb_vk->ms_color, cogl_vk_image_free);
  else
    fb_vk->ms_color = NULL;
  fb_vk->owns_ms_color = FALSE;

  fb = cogl_framebuffer_driver_get_framebuffer (COGL_FRAMEBUFFER_DRIVER (fb_vk));
  if (!COGL_IS_OFFSCREEN (fb) || !driver_vk || !driver_vk->vk_device)
    return fb_vk->color;

  fb_vk->ms_color =
    cogl_vk_image_new_optimal_samples (driver_vk->vk_device,
                                       fb_vk->color->width,
                                       fb_vk->color->height,
                                       fb_vk->color->format,
                                       fb_vk->color->cogl_format,
                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                       samples, &error);
  if (!fb_vk->ms_color)
    {
      g_warning ("Cogl Vulkan: MSAA alloc failed: %s",
                 error ? error->message : "unknown");
      return fb_vk->color;
    }
  fb_vk->owns_ms_color = TRUE;
  fb_vk->ms_color->attached_depth_stencil = fb_vk->depth_stencil;
  return fb_vk->ms_color;
}

/* Ensure MS color (if any) and matching-sample packed DS before draw/clip. */
static CoglVkImage *
prepare_draw_targets (CoglFramebufferVk *fb_vk,
                      CoglDriverVk      *driver_vk)
{
  CoglVkImage *color_target;
  VkSampleCountFlagBits samples;

  color_target = draw_color_target (fb_vk, driver_vk);
  samples = color_target && color_target->samples > VK_SAMPLE_COUNT_1_BIT
    ? color_target->samples
    : VK_SAMPLE_COUNT_1_BIT;
  ensure_depth_stencil (fb_vk, driver_vk, samples);
  if (color_target)
    color_target->attached_depth_stencil = fb_vk->depth_stencil;
  return color_target;
}

CoglVkImage *
cogl_framebuffer_vk_prepare_draw_targets (CoglFramebufferVk *fb)
{
  CoglDriverVk *driver_vk;

  g_return_val_if_fail (COGL_IS_FRAMEBUFFER_DRIVER_VK (fb), NULL);
  if (!ensure_color_image (fb))
    return NULL;
  driver_vk = peek_driver_vk (COGL_FRAMEBUFFER_DRIVER (fb));
  if (!driver_vk)
    return fb->color;
  return prepare_draw_targets (fb, driver_vk);
}

static void
maybe_resolve_msaa (CoglFramebufferVk *fb_vk,
                    CoglDriverVk      *driver_vk)
{
  CoglVkPass *pass;
  g_autoptr (GError) error = NULL;

  if (!fb_vk->ms_color || !fb_vk->color || !driver_vk)
    return;
  if (fb_vk->ms_color->samples <= VK_SAMPLE_COUNT_1_BIT)
    return;

  pass = ensure_pass (driver_vk);
  if (!pass)
    return;
  if (!cogl_vk_cmd_resolve_image (pass, fb_vk->ms_color, fb_vk->color, &error))
    {
      g_warning ("Cogl Vulkan: MSAA resolve failed: %s",
                 error ? error->message : "unknown");
    }
}

static CoglVkPass *
ensure_pass (CoglDriverVk *driver_vk)
{
  g_autoptr (GError) error = NULL;

  if (driver_vk->current_pass)
    {
      if (driver_vk->pending_scissor_valid)
        {
          driver_vk->current_pass->scissor = driver_vk->pending_scissor;
          driver_vk->current_pass->scissor_valid = TRUE;
        }
      if (driver_vk->pending_viewport_valid)
        {
          driver_vk->current_pass->viewport = driver_vk->pending_viewport;
          driver_vk->current_pass->viewport_valid = TRUE;
        }
      driver_vk->current_pass->stencil_clip_active =
        driver_vk->pending_stencil_clip;
      return driver_vk->current_pass;
    }

  driver_vk->current_pass = cogl_vk_pass_begin (driver_vk->vk_device, &error);
  if (!driver_vk->current_pass)
    {
      g_warning ("Cogl Vulkan: pass begin failed: %s",
                 error ? error->message : "unknown");
      return NULL;
    }

  if (driver_vk->pending_scissor_valid)
    {
      driver_vk->current_pass->scissor = driver_vk->pending_scissor;
      driver_vk->current_pass->scissor_valid = TRUE;
    }
  if (driver_vk->pending_viewport_valid)
    {
      driver_vk->current_pass->viewport = driver_vk->pending_viewport;
      driver_vk->current_pass->viewport_valid = TRUE;
    }
  driver_vk->current_pass->stencil_clip_active =
    driver_vk->pending_stencil_clip;

  return driver_vk->current_pass;
}

static void
cogl_framebuffer_vk_query_bits (CoglFramebufferDriver *driver,
                                CoglFramebufferBits   *bits)
{
  CoglFramebufferVk *fb_vk = COGL_FRAMEBUFFER_DRIVER_VK (driver);
  CoglDriverVk *driver_vk = peek_driver_vk (driver);

  bits->red = 8;
  bits->green = 8;
  bits->blue = 8;
  bits->alpha = 8;
  bits->depth = 0;
  bits->stencil = 0;

  if (!ensure_color_image (fb_vk))
    return;

  cogl_vk_format_color_bits (fb_vk->color->format,
                             &bits->red, &bits->green,
                             &bits->blue, &bits->alpha);

  if (driver_vk &&
      !fb_vk->disable_depth_and_stencil &&
      ensure_depth_stencil (fb_vk, driver_vk,
                            framebuffer_msaa_samples (fb_vk)))
    {
      cogl_vk_format_depth_stencil_bits (fb_vk->depth_stencil->format,
                                         &bits->depth, &bits->stencil);
    }
}


static void
cogl_framebuffer_vk_clear (CoglFramebufferDriver *driver,
                           unsigned long          buffers,
                           float                  red,
                           float                  green,
                           float                  blue,
                           float                  alpha)
{
  CoglFramebufferVk *fb_vk = COGL_FRAMEBUFFER_DRIVER_VK (driver);
  CoglDriverVk *driver_vk;
  CoglVkPass *pass;
  VkClearColorValue color;
  CoglVkImage *color_target;
  CoglVkRenderFormatSetup *setup;
  VkClearAttachment attachments[2] = { 0 };
  VkClearRect rect = { .layerCount = 1 };
  uint32_t n_attachments = 0;
  g_autoptr (GError) error = NULL;

  if (!(buffers & (COGL_BUFFER_BIT_COLOR |
                   COGL_BUFFER_BIT_DEPTH |
                   COGL_BUFFER_BIT_STENCIL)))
    return;

  if (!ensure_color_image (fb_vk))
    {
      static int n_skip;

      if (cogl_vk_debug_enabled ("draw") && n_skip < 8)
        {
          n_skip++;
          g_printerr ("VKCLEAR skip-no-color buffers=0x%lx fb=%p\n",
                      buffers, driver);
        }
      return;
    }

  driver_vk = peek_driver_vk (driver);
  if (!driver_vk || !driver_vk->vk_device)
    return;

  if ((buffers & (COGL_BUFFER_BIT_DEPTH | COGL_BUFFER_BIT_STENCIL)) &&
      !fb_vk->disable_depth_and_stencil &&
      !ensure_depth_stencil (fb_vk, driver_vk,
                             framebuffer_msaa_samples (fb_vk)))
    {
      /* DS ensure failed (e.g. onscreen without borrowed DS): still clear
       * COLOR if requested rather than aborting the whole clear.
       */
      buffers &= ~(COGL_BUFFER_BIT_DEPTH | COGL_BUFFER_BIT_STENCIL);
      if (!(buffers & COGL_BUFFER_BIT_COLOR))
        return;
    }

  /* Target allocation/rebinding can submit and retire the previous pass. */
  color_target = prepare_draw_targets (fb_vk, driver_vk);
  pass = ensure_pass (driver_vk);
  if (!pass)
    return;
  if (!color_target)
    return;

  rect.rect.extent.width = color_target->width;
  rect.rect.extent.height = color_target->height;
  if (pass->scissor_valid)
    {
      int64_t x1 = MAX ((int64_t) pass->scissor.offset.x, 0);
      int64_t y1 = MAX ((int64_t) pass->scissor.offset.y, 0);
      int64_t x2 = MIN ((int64_t) pass->scissor.offset.x +
                        pass->scissor.extent.width, color_target->width);
      int64_t y2 = MIN ((int64_t) pass->scissor.offset.y +
                        pass->scissor.extent.height, color_target->height);

      if (x2 <= x1 || y2 <= y1)
        return;
      rect.rect.offset.x = x1;
      rect.rect.offset.y = y1;
      rect.rect.extent.width = x2 - x1;
      rect.rect.extent.height = y2 - y1;
    }

  if (buffers & COGL_BUFFER_BIT_COLOR)
    {
      static int n_clear;
      CoglPixelFormat fmt = fb_vk->color->cogl_format;
      VkFormat vk_fmt = fb_vk->color->format;
      float cr = red;
      float cg = green;
      float cb = blue;
      float ca = alpha;

      if (cogl_vk_debug_enabled ("draw") && n_clear < 8)
        {
          n_clear++;
          g_printerr ("VKCLEAR color=%p dmabuf=%d owned=%d layout=%d vkfmt=%d "
                      "rgba=%.3f,%.3f,%.3f,%.3f\n",
                      fb_vk->color, fb_vk->color->is_dmabuf,
                      fb_vk->color->dmabuf_graphics_owned,
                      (int) fb_vk->color->layout, (int) vk_fmt,
                      red, green, blue, alpha);
        }
      /* These VkFormats store R,G,B,A in address order, so a Cogl
       * BGR/AFIRST name has to be swizzled into the clear. Packed
       * formats (A2R10G10B10, B8G8R8A8) already name components;
       * float32[0] is always Red there.
       */
      gboolean rgba_memory =
        vk_fmt == VK_FORMAT_R8G8B8A8_UNORM ||
        vk_fmt == VK_FORMAT_R8G8B8A8_SRGB ||
        vk_fmt == VK_FORMAT_R16G16B16A16_UNORM ||
        vk_fmt == VK_FORMAT_R16G16B16A16_SFLOAT ||
        vk_fmt == VK_FORMAT_R32G32B32A32_SFLOAT;

      if (fmt != COGL_PIXEL_FORMAT_ANY && !(fmt & COGL_A_BIT))
        ca = 1.f;

      /* AFIRST 8888 is stored in Cogl address order (A,R,G,B or
       * A,B,G,R). The VkFormat names those bytes differently; the
       * view swizzle undoes that for sampling. Clear must write the
       * Cogl order, or readback sees B,G,R,A (rgb8 red came back 31).
       */
      if ((vk_fmt == VK_FORMAT_B8G8R8A8_UNORM ||
           vk_fmt == VK_FORMAT_B8G8R8A8_SRGB) &&
          (fmt & COGL_AFIRST_BIT))
        {
          color.float32[0] = cg;
          color.float32[1] = cr;
          color.float32[2] = ca;
          color.float32[3] = cb;
        }
      else if ((vk_fmt == VK_FORMAT_R8G8B8A8_UNORM ||
                vk_fmt == VK_FORMAT_R8G8B8A8_SRGB) &&
               (fmt & COGL_AFIRST_BIT))
        {
          color.float32[0] = ca;
          color.float32[1] = cb;
          color.float32[2] = cg;
          color.float32[3] = cr;
        }
      else
        {
          if (rgba_memory && (fmt & COGL_BGR_BIT))
            {
              float tmp = cr;
              cr = cb;
              cb = tmp;
            }

          if (rgba_memory && (fmt & COGL_AFIRST_BIT))
            {
              color.float32[0] = ca;
              color.float32[1] = cr;
              color.float32[2] = cg;
              color.float32[3] = cb;
            }
          else
            {
              color.float32[0] = cr;
              color.float32[1] = cg;
              color.float32[2] = cb;
              color.float32[3] = ca;
            }
        }

      attachments[n_attachments++] = (VkClearAttachment) {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .colorAttachment = 0,
        .clearValue.color = color,
      };
    }

  if ((buffers & (COGL_BUFFER_BIT_DEPTH | COGL_BUFFER_BIT_STENCIL)) &&
      fb_vk->depth_stencil)
    {
      int d = 0, s = 0;
      VkImageAspectFlags fmt_aspect = 0;
      VkImageAspectFlags clear_aspect = 0;

      cogl_vk_format_depth_stencil_bits (fb_vk->depth_stencil->format, &d, &s);
      if (d > 0)
        fmt_aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
      if (s > 0)
        fmt_aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;

      if (buffers & COGL_BUFFER_BIT_DEPTH)
        clear_aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
      if (buffers & COGL_BUFFER_BIT_STENCIL)
        clear_aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
      clear_aspect &= fmt_aspect;

      if (clear_aspect != 0)
        {
          attachments[n_attachments++] = (VkClearAttachment) {
            .aspectMask = clear_aspect,
            .clearValue.depthStencil = { .depth = 1.f, .stencil = 0 },
          };
        }
    }

  if (n_attachments == 0)
    return;

  setup = cogl_vk_pipeline_cache_get_format_setup (
    driver_vk->pipeline_cache, color_target->format,
    fb_vk->depth_stencil ? fb_vk->depth_stencil->format : VK_FORMAT_UNDEFINED,
    color_target->samples > VK_SAMPLE_COUNT_1_BIT
      ? color_target->samples : VK_SAMPLE_COUNT_1_BIT,
    &error);
  if (!setup ||
      !cogl_vk_pass_begin_render (pass, driver_vk->pipeline_cache,
                                  color_target, setup,
                                  color_target->width, color_target->height,
                                  &error))
    {
      g_warning ("Cogl Vulkan: clear failed: %s",
                 error ? error->message : "unknown");
      return;
    }

  /* Attachment clears ignore dynamic scissor state; supply its bounds. */
  vkCmdClearAttachments (pass->command_buffer, n_attachments, attachments,
                         1, &rect);
}


static void
submit_and_sync (CoglFramebufferDriver *driver)
{
  CoglDriverVk *driver_vk = peek_driver_vk (driver);
  CoglFramebufferVk *fb_vk;
  CoglFramebuffer *fb;
  CoglContext *ctx;
  CoglRenderer *renderer;
  g_autoptr (GError) error = NULL;

  if (!driver_vk)
    return;

  fb_vk = COGL_FRAMEBUFFER_DRIVER_VK (driver);
  maybe_resolve_msaa (fb_vk, driver_vk);

  if (!driver_vk->current_pass)
    {
      static int n_nosubmit;

      if (cogl_vk_debug_enabled ("draw") && n_nosubmit < 4)
        {
          n_nosubmit++;
          g_printerr ("VKSUBMIT none\n");
        }
      cogl_vk_debug_log ("draw", "submit_and_sync: no current_pass");
      return;
    }

  {
    static int n_submit;

    if (cogl_vk_debug_enabled ("draw") && n_submit < 8)
      {
        n_submit++;
        g_printerr ("VKSUBMIT color=%p ino=%" G_GUINT64_FORMAT
                    " dmabuf=%d owned=%d layout=%d\n",
                    fb_vk->color,
                    fb_vk->color ? fb_vk->color->debug_dmabuf_ino : 0,
                    fb_vk->color ? fb_vk->color->is_dmabuf : -1,
                    fb_vk->color ? fb_vk->color->dmabuf_graphics_owned : -1,
                    fb_vk->color ? (int) fb_vk->color->layout : -1);
      }
  }

  cogl_vk_debug_log (
    "draw",
    "submit_and_sync begin pass=%p color=%p layout=%d",
    (void *) driver_vk->current_pass,
    fb_vk->color ? (void *) fb_vk->color : NULL,
    fb_vk->color ? (int) fb_vk->color->layout : -1);

  /* Diagnostic: prove whether this queue can store into the scanout
   * dma-buf at all. A standalone vkCmdClearColorImage of the same BO
   * paints it; render-pass draws currently leave it zero.
   */
  if ((g_strcmp0 (g_getenv ("COGL_VK_FORCE_CLEAR"), "1") == 0 ||
       g_strcmp0 (g_getenv ("COGL_VK_FORCE_CLEAR"), "xfer") == 0) &&
      fb_vk->color && fb_vk->color->is_dmabuf &&
      fb_vk->color->width >= 64 && fb_vk->color->height >= 64)
    {
      CoglVkPass *pass = driver_vk->current_pass;
      VkClearColorValue green = {{ 0.f, 1.f, 0.f, 1.f }};
      VkImageSubresourceRange range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .levelCount = 1,
        .layerCount = 1,
      };
      static int n_force;

      cogl_vk_pass_end_render (pass);
      cogl_vk_image_cmd_barrier (fb_vk->color,
                                 pass->command_buffer,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_ACCESS_MEMORY_WRITE_BIT,
                                 VK_ACCESS_TRANSFER_WRITE_BIT);
      vkCmdClearColorImage (pass->command_buffer,
                            fb_vk->color->image,
                            fb_vk->color->layout,
                            &green, 1, &range);
      if (n_force < 4)
        {
          n_force++;
          g_printerr ("VKFORCECLEAR ino=%" G_GUINT64_FORMAT
                      " owned=%d layout=%d\n",
                      fb_vk->color->debug_dmabuf_ino,
                      fb_vk->color->dmabuf_graphics_owned,
                      (int) fb_vk->color->layout);
        }
    }

  if (!cogl_vk_pass_submit (driver_vk->current_pass, &error))
    {
      g_warning ("Cogl Vulkan: submit failed: %s",
                 error ? error->message : "unknown");
      cogl_vk_debug_log ("draw", "submit_and_sync FAIL: %s",
                         error ? error->message : "unknown");
      g_clear_pointer (&driver_vk->current_pass, cogl_vk_pass_free);
      return;
    }

  cogl_driver_vk_store_sync_fd_from_pass (driver_vk);

  g_clear_pointer (&driver_vk->current_pass, cogl_vk_pass_free);
  cogl_vk_debug_log ("draw", "submit_and_sync OK color_layout=%d",
                     fb_vk->color ? (int) fb_vk->color->layout : -1);

  fb = cogl_framebuffer_driver_get_framebuffer (driver);
  ctx = cogl_framebuffer_get_context (fb);
  renderer = cogl_context_get_renderer (ctx);
  cogl_renderer_update_sync (renderer);
}

static void
cogl_framebuffer_vk_finish (CoglFramebufferDriver *driver)
{
  CoglDriverVk *driver_vk = peek_driver_vk (driver);

  submit_and_sync (driver);
  if (driver_vk && driver_vk->vk_device)
    {
      VkResult res = vkQueueWaitIdle (driver_vk->vk_device->graphics_queue);

      cogl_vk_device_note_vk_result (driver_vk->vk_device, res);
      if (res != VK_SUCCESS)
        g_error ("Cogl Vulkan: finish failed (%d)", (int) res);
    }
}

static void
cogl_framebuffer_vk_flush (CoglFramebufferDriver *driver)
{
  submit_and_sync (driver);
}

static void
cogl_framebuffer_vk_discard_buffers (CoglFramebufferDriver *driver,
                                     unsigned long          buffers)
{
  CoglDriverVk *driver_vk = peek_driver_vk (driver);

  (void) buffers;

  /* GL discard is a hint; for VK end any open render pass so subsequent
   * clears/draws see a clean subpass boundary (LOAD still used).
   */
  if (driver_vk && driver_vk->current_pass)
    cogl_vk_pass_end_render (driver_vk->current_pass);
}

static gboolean
vert_push_from_fan_quad (CoglAttribute   **attributes,
                         int               n_attributes,
                         int               first_vertex,
                         CoglFramebuffer  *fb,
                         CoglPipeline     *pipeline,
                         CoglVkVertPush   *out)
{
  CoglAttribute *pos_attr = NULL;
  CoglAttribute *tex_attr = NULL;
  CoglAttributeBuffer *buf;
  const uint8_t *data;
  float x0, y0, x1, y1;
  float u0, v0, u1, v1;
  graphene_matrix_t mvp, local, combined;
  int i;

  for (i = 0; i < n_attributes; i++)
    {
      if (!attributes[i] || !attributes[i]->name_state)
        continue;
      if (attributes[i]->name_state->name_id ==
          COGL_ATTRIBUTE_NAME_ID_POSITION_ARRAY)
        pos_attr = attributes[i];
      else if (attributes[i]->name_state->name_id ==
               COGL_ATTRIBUTE_NAME_ID_TEXTURE_COORD_ARRAY &&
               attributes[i]->name_state->layer_number == 0)
        tex_attr = attributes[i];
    }

  if (!pos_attr || pos_attr->n_components < 2)
    return FALSE;

  buf = cogl_attribute_get_buffer (pos_attr);
  data = _cogl_buffer_map (COGL_BUFFER (buf), COGL_BUFFER_ACCESS_READ, 0, NULL);
  if (!data)
    return FALSE;

  {
    const float *p0 = (const float *)(data + pos_attr->offset +
                                      (size_t) first_vertex * pos_attr->stride);
    const float *p2 = (const float *)((const uint8_t *) p0 + 2 * pos_attr->stride);

    x0 = p0[0];
    y0 = p0[1];
    x1 = p2[0];
    y1 = p2[1];
  }
  cogl_buffer_unmap (COGL_BUFFER (buf));

  u0 = 0.f;
  v0 = 0.f;
  u1 = 1.f;
  v1 = 1.f;
  if (tex_attr && tex_attr->n_components >= 2)
    {
      buf = cogl_attribute_get_buffer (tex_attr);
      data = _cogl_buffer_map (COGL_BUFFER (buf), COGL_BUFFER_ACCESS_READ, 0,
                               NULL);
      if (data)
        {
          const float *t0 = (const float *)(data + tex_attr->offset +
                                            (size_t) first_vertex *
                                            tex_attr->stride);
          const float *t2 = (const float *)((const uint8_t *) t0 +
                                            2 * tex_attr->stride);

          u0 = t0[0];
          v0 = t0[1];
          u1 = t2[0];
          v1 = t2[1];
          cogl_buffer_unmap (COGL_BUFFER (buf));
        }
    }

  /* Journal fan: local maps unit→object; MVP applies projection (+ optional
   * y_flip bake when no vertex snippets — GL flip_uniform==-1).
   */
  graphene_matrix_init_identity (&mvp);
  if (fb)
    {
      graphene_matrix_t mv, proj;
      CoglContext *ctx = cogl_framebuffer_get_context (fb);
      CoglMatrixEntry *mv_entry = ctx
        ? cogl_context_get_current_modelview_entry (ctx) : NULL;
      CoglMatrixEntry *proj_entry = ctx
        ? cogl_context_get_current_projection_entry (ctx) : NULL;

      graphene_matrix_init_identity (&mv);
      graphene_matrix_init_identity (&proj);
      if (mv_entry)
        cogl_matrix_entry_get (mv_entry, &mv);
      else
        cogl_framebuffer_get_modelview_matrix (fb, &mv);
      if (proj_entry)
        cogl_matrix_entry_get (proj_entry, &proj);
      else
        cogl_framebuffer_get_projection_matrix (fb, &proj);
      cogl_vk_maybe_bake_y_flip_projection (fb, pipeline, &proj);
      /* Graphene uses row vectors (p' = p × M). multiply(a,b)=a×b applies
       * a then b — so MVP = MV × Proj, not Proj × MV.
       */
      graphene_matrix_multiply (&mv, &proj, &mvp);
    }
  graphene_matrix_init_translate (&local,
                                  &GRAPHENE_POINT3D_INIT (x0, y0, 0.f));
  graphene_matrix_scale (&local, x1 - x0, y1 - y0, 1.f);
  /* Unit → object (local) then MVP. Row-vector: Local × MVP. */
  graphene_matrix_multiply (&local, &mvp, &combined);
  cogl_vk_vert_push_set_matrix (out, &combined);
  out->uv_off[0] = u0;
  out->uv_off[1] = v0;
  out->uv_size[0] = u1 - u0;
  out->uv_size[1] = v1 - v0;
  cogl_vk_debug_log (
    "draw",
    "fan_quad first=%d stride=%u off=%u xy=(%.1f,%.1f)-(%.1f,%.1f) "
    "uv=(%.2f,%.2f)+(%.2f,%.2f)",
    first_vertex,
    (unsigned) pos_attr->stride,
    (unsigned) pos_attr->offset,
    x0, y0, x1, y1, u0, v0, u1 - u0, v1 - v0);
  return TRUE;
}

/* Wait on each layer's foreign acquire fd before sampling (dma-buf / EGL). */
static gboolean
pass_wait_pipeline_acquire_fds (CoglVkPass   *pass,
                                CoglPipeline *pipeline)
{
  g_autofree CoglPipelineLayer **layers = NULL;
  int n_layers = 0;
  int li;

  if (!pass || !pipeline)
    return FALSE;

  /* Unit-order layers only — never get_layer_texture(pipeline, unit). */
  cogl_vk_pipeline_collect_layers (pipeline, &layers, &n_layers);
  cogl_vk_debug_log (
    "draw",
    "pass_wait acquire n_layers=%d (unit-order, no get_layer_texture(unit))",
    n_layers);

  for (li = 0; li < n_layers; li++)
    {
      CoglTexture *layer_tex;
      CoglTexture2DVk *layer_tex_vk;
      int acquire_fd;

      if (!layers[li])
        {
          cogl_vk_debug_log ("draw", "  wait unit=%d layer=NULL", li);
          continue;
        }
      layer_tex = _cogl_pipeline_layer_get_texture (layers[li]);
      cogl_vk_debug_log (
        "draw",
        "  wait unit=%d layer_index=%d tex=%p",
        li, layers[li]->index, (void *) layer_tex);
      layer_tex_vk = cogl_texture_2d_vk_resolve (layer_tex);
      if (!layer_tex_vk)
        continue;

      acquire_fd = cogl_texture_2d_vk_take_acquire_sync_fd (layer_tex_vk);
      if (acquire_fd >= 0)
        {
          g_autoptr (GError) wait_error = NULL;

          if (!cogl_vk_pass_add_wait_sync_fd (pass, acquire_fd, &wait_error))
            {
              g_warning ("Cogl Vulkan: acquire sync wait failed: %s",
                         wait_error ? wait_error->message : "unknown");
              cogl_texture_2d_vk_set_acquire_sync_fd (layer_tex_vk, acquire_fd);
              return FALSE;
            }
        }
    }
  return TRUE;
}

/* GSK records a GskGpuOp list, then gsk_vulkan_frame_submit_ops walks it
 * once. Cogl re-enters draw from journal/clip flush. Dump the caller when
 * that nesting starts so we can see which flush created the loop.
 */
static void
vk_draw_dump_reentry (const char      *tag,
                      int              depth,
                      CoglDrawFlags    flags,
                      CoglPipeline    *pipeline,
                      CoglVerticesMode mode,
                      int              n_vertices)
{
  g_autofree CoglPipelineLayer **layers = NULL;
  int n_layers = 0;
  g_autoptr (GString) indices = NULL;
  int i;

  if (!cogl_vk_debug_enabled ("draw"))
    return;

  indices = g_string_new (NULL);
  if (pipeline)
    cogl_vk_pipeline_collect_layers (pipeline, &layers, &n_layers);
  for (i = 0; i < n_layers; i++)
    g_string_append_printf (indices, "%s%d",
                            i ? "," : "",
                            layers[i] ? layers[i]->index : -1);

  fprintf (stderr,
           "Cogl Vulkan: %s depth=%d flags=0x%x mode=%d n=%d "
           "pipe=%p n_layers=%d indices=[%s]\n",
           tag, depth, (unsigned) flags, (int) mode, n_vertices,
           (void *) pipeline,
           pipeline ? cogl_pipeline_get_n_layers (pipeline) : -1,
           indices->str);

#ifdef __GLIBC__
  {
    void *frames[16];
    int n = backtrace (frames, G_N_ELEMENTS (frames));
    char **syms = backtrace_symbols (frames, n);

    for (i = 0; syms && i < n; i++)
      fprintf (stderr, "  #%d %s\n", i, syms[i]);
    free (syms);
  }
#endif
}

static CoglTexture *
pipeline_first_layer_texture (CoglPipeline *pipeline)
{
  g_autofree CoglPipelineLayer **layers = NULL;
  int n_layers = 0;

  if (!pipeline || cogl_pipeline_get_n_layers (pipeline) < 1)
    return NULL;

  cogl_vk_pipeline_collect_layers (pipeline, &layers, &n_layers);
  if (n_layers < 1 || !layers[0])
    {
      cogl_vk_debug_log ("draw", "first_layer_texture n=%d → NULL", n_layers);
      return NULL;
    }
  cogl_vk_debug_log (
    "draw",
    "first_layer_texture n=%d index=%d tex=%p",
    n_layers, layers[0]->index,
    (void *) _cogl_pipeline_layer_get_texture (layers[0]));
  return _cogl_pipeline_layer_get_texture (layers[0]);
}

static void
cogl_framebuffer_vk_draw_attributes (CoglFramebufferDriver  *driver,
                                     CoglPipeline           *pipeline,
                                     CoglVerticesMode        mode,
                                     int                     first_vertex,
                                     int                     n_vertices,
                                     CoglAttribute         **attributes,
                                     int                     n_attributes,
                                     CoglDrawFlags           flags)
{
  CoglFramebufferVk *fb_vk = COGL_FRAMEBUFFER_DRIVER_VK (driver);
  CoglFramebuffer *fb;
  CoglDriverVk *driver_vk;
  CoglVkPass *pass;
  CoglTexture *src_tex;
  CoglTexture2DVk *src_tex_vk;
  CoglVkImage *src_image = NULL;
  CoglVkImage *color_target;
  CoglVkPipelineCache *cache;
  CoglVkRenderFormatSetup *setup;
  VkDescriptorSet ds = VK_NULL_HANDLE;
  g_autoptr (GError) error = NULL;
  CoglColor pipe_color;
  gboolean opaque;
  CoglVkVertPush vert_push;
  const CoglVkVertPush *vert_ptr = NULL;
  gboolean use_push;
  gboolean free_pipeline = FALSE;
  static int draw_depth;

  driver_vk = peek_driver_vk (driver);
  if (!driver_vk || !driver_vk->pipeline_cache)
    return;

  draw_depth++;
  cogl_vk_debug_log (
    "draw",
    "ENTER depth=%d mode=%d first=%d n=%d nattr=%d flags=0x%x "
    "n_layers=%d pass=%p color_layout=%d in_rp=%d",
    draw_depth, (int) mode, first_vertex, n_vertices, n_attributes,
    (unsigned) flags,
    pipeline ? cogl_pipeline_get_n_layers (pipeline) : -1,
    (void *) driver_vk->current_pass,
    (fb_vk && fb_vk->color) ? (int) fb_vk->color->layout : -1,
    driver_vk->current_pass ? driver_vk->current_pass->in_render_pass : -1);
  /* depth 2 is the normal journal flush inside a user draw. Deeper
   * means that flush drew something which flushed again.
   */
  if (draw_depth >= 3)
    vk_draw_dump_reentry (draw_depth > 8 ? "ABORT" : "reenter",
                          draw_depth, flags, pipeline, mode, n_vertices);

  if (draw_depth > 8)
    {
      cogl_vk_debug_log ("draw",
                         "ABORT recursion depth=%d (likely get_layer/flush loop)",
                         draw_depth);
      g_warning ("Cogl Vulkan: draw_attributes recursion depth=%d — abort",
                 draw_depth);
      draw_depth--;
      return;
    }

  fb = cogl_framebuffer_driver_get_framebuffer (driver);

  /* Match GL: journal flush, layer validate/fallback, FB flush, then
   * driver flush_attributes_state (may stash pending_pipeline).
   */
  _cogl_flush_attributes_state (fb, pipeline, flags,
                                attributes, n_attributes);

  if (driver_vk->pending_pipeline)
    {
      pipeline = driver_vk->pending_pipeline;
      free_pipeline = driver_vk->pending_pipeline_owned;
      driver_vk->pending_pipeline = NULL;
      driver_vk->pending_pipeline_owned = FALSE;
    }

  if (!ensure_color_image (fb_vk))
    {
      static int n_draw_skip;

      if (cogl_vk_debug_enabled ("draw") && n_draw_skip < 8)
        {
          n_draw_skip++;
          g_printerr ("VKDRAW skip-no-color nvert=%d\n", n_vertices);
        }
      goto out;
    }

  cache = driver_vk->pipeline_cache;
  /* Target allocation/rebinding can submit and retire the previous pass. */
  color_target = prepare_draw_targets (fb_vk, driver_vk);
  pass = ensure_pass (driver_vk);
  if (!pass)
    goto out;
  cogl_pipeline_get_color (pipeline, &pipe_color);
  {
    static int n_draw;

    if (cogl_vk_debug_enabled ("draw") && n_draw < 8 && color_target)
      {
        n_draw++;
        g_printerr ("VKDRAW color=%p ino=%" G_GUINT64_FORMAT
                    " dmabuf=%d owned=%d layout=%d vkfmt=%d nvert=%d "
                    "rgba=%.2f,%.2f,%.2f,%.2f layers=%d\n",
                    color_target, color_target->debug_dmabuf_ino,
                    color_target->is_dmabuf,
                    color_target->dmabuf_graphics_owned,
                    (int) color_target->layout, (int) color_target->format,
                    n_vertices,
                    cogl_color_get_red (&pipe_color),
                    cogl_color_get_green (&pipe_color),
                    cogl_color_get_blue (&pipe_color),
                    cogl_color_get_alpha (&pipe_color),
                    pipeline ? cogl_pipeline_get_n_layers (pipeline) : -1);
      }
  }

  setup = cogl_vk_pipeline_cache_get_format_setup (
    cache,
    color_target->format,
    fb_vk->depth_stencil ? fb_vk->depth_stencil->format
                         : VK_FORMAT_UNDEFINED,
    color_target->samples > VK_SAMPLE_COUNT_1_BIT
      ? color_target->samples
      : VK_SAMPLE_COUNT_1_BIT,
    &error);
  if (!setup)
    {
      g_warning ("Cogl Vulkan: format setup failed: %s",
                 error ? error->message : "unknown");
      goto out;
    }

  cogl_pipeline_get_color (pipeline, &pipe_color);
  src_tex = pipeline_first_layer_texture (pipeline);
  /* Use the same derived blend state as GL, including vertex alpha,
   * snippets and every texture layer. Pipeline constant color alone
   * cannot establish opacity for journal batches.
   */
  opaque = !pipeline->real_blend_enable;

  use_push = cogl_vk_vbo_should_use_push_constant_fast_path (mode, n_vertices,
                                                             attributes,
                                                             n_attributes,
                                                             pipeline);
  cogl_vk_debug_log (
    "draw",
    "  path use_push=%d src_tex=%p opaque=%d color=(%.3f %.3f %.3f %.3f)",
    use_push, (void *) src_tex, opaque,
    cogl_color_get_red (&pipe_color), cogl_color_get_green (&pipe_color),
    cogl_color_get_blue (&pipe_color), cogl_color_get_alpha (&pipe_color));

  /* Draw general geometry using Vulkan vertex-buffer bindings. */
  if (!use_push)
    {
      VkPipeline cold_pipe = VK_NULL_HANDLE;

      src_tex_vk = cogl_texture_2d_vk_resolve (src_tex);
      if (src_tex_vk)
        {
          src_image = cogl_texture_2d_vk_peek_image (src_tex_vk);
          ds = cogl_texture_2d_vk_ensure_descriptor_set (src_tex_vk, &error);
          if (ds == VK_NULL_HANDLE)
            {
              g_warning ("Cogl Vulkan: VBO texture descriptor failed: %s",
                         error ? error->message : "unknown");
              goto out;
            }
        }

      if (!cogl_vk_pipeline_is_hot_path (pipeline))
        {
          if (!cache->spirv_cache)
            {
              g_warning ("Cogl Vulkan: cold VBO draw needs spirv cache");
              goto out;
            }
          /* Do NOT call get_pipeline_for_cogl here: it assembles PUSH_QUAD
           * (strips `attribute` without layout in) and aborts the draw on
           * shaderc fail — custom-attributes never reached VERT_VBO.
           * cmd_draw_vbo builds VERT_VBO with custom_names when cold_pipe
           * is NULL.
           */
          cogl_vk_debug_log (
            "draw",
            "VBO cold: skip PUSH_QUAD probe; VERT_VBO via cmd_draw_vbo "
            "n_attr=%d",
            n_attributes);
          cold_pipe = VK_NULL_HANDLE;
        }

      if (!pass_wait_pipeline_acquire_fds (pass, pipeline))
        goto out;

      if (!cogl_vk_cmd_draw_vbo (pass, cache, setup, color_target,
                                 src_image, ds, pipeline, fb, mode,
                                 first_vertex, n_vertices, attributes,
                                 n_attributes, opaque, cogl_color_get_alpha (&pipe_color),
                                 cold_pipe, &error))
        {
          g_warning ("Cogl Vulkan: VBO draw failed: %s",
                     error ? error->message : "unknown");
        }
      goto out;
    }

  /* Hot push-constant journal quad path. */
  if (!src_tex)
    {
      if (n_vertices == 4 && attributes && n_attributes > 0 &&
          vert_push_from_fan_quad (attributes, n_attributes, first_vertex,
                                   fb, pipeline, &vert_push))
        vert_ptr = &vert_push;
      if (!cogl_vk_cmd_draw_quad (pass, cache, setup, color_target,
                                  cogl_color_get_red (&pipe_color),
                                  cogl_color_get_green (&pipe_color),
                                  cogl_color_get_blue (&pipe_color),
                                  cogl_color_get_alpha (&pipe_color),
                                  vert_ptr, fb, pipeline, &error))
        {
          g_warning ("Cogl Vulkan: solid draw failed: %s",
                     error ? error->message : "unknown");
        }
      goto out;
    }

  src_tex_vk = cogl_texture_2d_vk_resolve (src_tex);
  if (!src_tex_vk)
    goto out;

  src_image = cogl_texture_2d_vk_peek_image (src_tex_vk);
  if (!src_image)
    goto out;

  ds = cogl_texture_2d_vk_ensure_descriptor_set (src_tex_vk, &error);
  if (ds == VK_NULL_HANDLE)
    {
      g_warning ("Cogl Vulkan: descriptor set failed: %s",
                 error ? error->message : "unknown");
      goto out;
    }

  if (!pass_wait_pipeline_acquire_fds (pass, pipeline))
    goto out;

  if (n_vertices == 4 && attributes && n_attributes > 0 &&
      vert_push_from_fan_quad (attributes, n_attributes, first_vertex,
                               fb, pipeline, &vert_push))
    vert_ptr = &vert_push;

  {
    VkPipeline cold_pipe = VK_NULL_HANDLE;

    if (!cogl_vk_pipeline_is_hot_path (pipeline))
      {
        g_autoptr (GError) cold_error = NULL;

        cogl_vk_debug_log ("draw", "textured COLD assemble n_verts=%d",
                           n_vertices);

        if (!cache->spirv_cache)
          {
            g_warning ("Cogl Vulkan: cold textured draw needs spirv cache");
            goto out;
          }
        cold_pipe =
          cogl_vk_spirv_cache_get_pipeline_for_cogl (cache->spirv_cache, cache,
                                                     setup, pipeline, opaque,
                                                     &cold_error);
        if (cold_pipe == VK_NULL_HANDLE)
          {
            g_warning ("Cogl Vulkan: cold textured pipeline failed: %s",
                       cold_error ? cold_error->message : "unknown");
            goto out;
          }
      }

    if (!cogl_vk_cmd_draw_texture (pass, cache, setup,
                                   color_target, src_image, ds,
                                   cogl_color_get_alpha (&pipe_color), opaque, vert_ptr,
                                   cold_pipe, pipeline, &error))
      {
        g_warning ("Cogl Vulkan: textured draw failed: %s",
                   error ? error->message : "unknown");
      }
  }

out:
  cogl_vk_debug_log ("draw", "LEAVE depth=%d free_pipeline=%d",
                     draw_depth, free_pipeline);
  draw_depth--;
  if (free_pipeline)
    g_object_unref (pipeline);
}

static void
cogl_framebuffer_vk_draw_indexed_attributes (CoglFramebufferDriver  *driver,
                                             CoglPipeline           *pipeline,
                                             CoglVerticesMode        mode,
                                             int                     first_vertex,
                                             int                     n_vertices,
                                             CoglIndices            *indices,
                                             CoglAttribute         **attributes,
                                             int                     n_attributes,
                                             CoglDrawFlags           flags)
{
  CoglFramebufferVk *fb_vk = COGL_FRAMEBUFFER_DRIVER_VK (driver);
  CoglFramebuffer *fb;
  CoglDriverVk *driver_vk;
  CoglVkPass *pass;
  CoglVkPipelineCache *cache;
  CoglVkRenderFormatSetup *setup;
  CoglVkImage *color_target;
  CoglTexture *src_tex;
  CoglTexture2DVk *src_tex_vk;
  CoglVkImage *src_image = NULL;
  VkDescriptorSet ds = VK_NULL_HANDLE;
  CoglColor pipe_color;
  gboolean opaque;
  g_autoptr (GError) error = NULL;
  gboolean free_pipeline = FALSE;
  CoglBuffer *mapped_indices_buffer = NULL;
  int n_quads;
  int q;

  driver_vk = peek_driver_vk (driver);
  if (!driver_vk || !driver_vk->pipeline_cache)
    return;

  fb = cogl_framebuffer_driver_get_framebuffer (driver);

  /* Match GL draw_indexed_attributes: flush before geometry work. */
  _cogl_flush_attributes_state (fb, pipeline, flags,
                                attributes, n_attributes);

  if (driver_vk->pending_pipeline)
    {
      pipeline = driver_vk->pending_pipeline;
      free_pipeline = driver_vk->pending_pipeline_owned;
      driver_vk->pending_pipeline = NULL;
      driver_vk->pending_pipeline_owned = FALSE;
    }

  /* Journal fast-path: 6 indices per quad → push-constant fans.
   * Overrides already applied; skip re-validate/re-flush in nested draws.
   */
  if (n_vertices >= 6 && (n_vertices % 6) == 0 && attributes &&
      cogl_vk_vbo_should_use_push_constant_fast_path (
        COGL_VERTICES_MODE_TRIANGLE_FAN, 4, attributes, n_attributes,
        pipeline))
    {
      CoglDrawFlags nested_flags =
        flags | COGL_DRAW_SKIP_JOURNAL_FLUSH |
        COGL_DRAW_SKIP_PIPELINE_VALIDATION |
        COGL_DRAW_SKIP_FRAMEBUFFER_FLUSH;

      n_quads = n_vertices / 6;
      for (q = 0; q < n_quads; q++)
        {
          cogl_framebuffer_vk_draw_attributes (driver, pipeline,
                                               COGL_VERTICES_MODE_TRIANGLE_FAN,
                                               (first_vertex / 6 + q) * 4, 4,
                                               attributes, n_attributes,
                                               nested_flags);
        }
      goto out;
    }

  if (!ensure_color_image (fb_vk) || !indices)
    goto out;

  /* Reading DEVICE_LOCAL indices submits the previous pass for a staging
   * copy. Do this before obtaining the pass used by this draw; otherwise
   * conversion can free that pass while draw_common still holds it.
   */
  if (mode == COGL_VERTICES_MODE_LINE_LOOP ||
      cogl_indices_get_indices_type (indices) == COGL_INDICES_TYPE_UNSIGNED_BYTE)
    {
      CoglBuffer *buffer = COGL_BUFFER (cogl_indices_get_buffer (indices));

      if (first_vertex < 0 || n_vertices <= 0)
        goto out;
      if (!_cogl_buffer_map (buffer, COGL_BUFFER_ACCESS_READ, 0, &error))
        {
          g_warning ("Cogl Vulkan: index read failed: %s",
                     error ? error->message : "unknown");
          goto out;
        }
      mapped_indices_buffer = buffer;
    }

  cache = driver_vk->pipeline_cache;
  /* Target allocation/rebinding can submit and retire the previous pass. */
  color_target = prepare_draw_targets (fb_vk, driver_vk);
  pass = ensure_pass (driver_vk);
  if (!pass)
    goto out;
  setup = cogl_vk_pipeline_cache_get_format_setup (
    cache,
    color_target->format,
    fb_vk->depth_stencil ? fb_vk->depth_stencil->format
                         : VK_FORMAT_UNDEFINED,
    color_target->samples > VK_SAMPLE_COUNT_1_BIT
      ? color_target->samples
      : VK_SAMPLE_COUNT_1_BIT,
    &error);
  if (!setup)
    goto out;

  cogl_pipeline_get_color (pipeline, &pipe_color);
  src_tex = pipeline_first_layer_texture (pipeline);
  /* Use the same derived blend state as GL, including vertex alpha,
   * snippets and every texture layer. Pipeline constant color alone
   * cannot establish opacity for journal batches.
   */
  opaque = !pipeline->real_blend_enable;

  src_tex_vk = cogl_texture_2d_vk_resolve (src_tex);
  if (src_tex_vk)
    {
      src_image = cogl_texture_2d_vk_peek_image (src_tex_vk);
      ds = cogl_texture_2d_vk_ensure_descriptor_set (src_tex_vk, &error);
      if (ds == VK_NULL_HANDLE)
        {
          g_warning ("Cogl Vulkan: indexed VBO texture descriptor failed: %s",
                     error ? error->message : "unknown");
          goto out;
        }
    }

  if (!pass_wait_pipeline_acquire_fds (pass, pipeline))
    goto out;

  if (!cogl_vk_cmd_draw_indexed_vbo (pass, cache, setup, color_target,
                                     src_image, ds, pipeline, fb, mode,
                                     first_vertex, n_vertices, indices,
                                     attributes, n_attributes, opaque,
                                     cogl_color_get_alpha (&pipe_color), VK_NULL_HANDLE,
                                     &error))
    {
      g_warning ("Cogl Vulkan: indexed VBO draw failed: %s",
                 error ? error->message : "unknown");
    }

out:
  if (mapped_indices_buffer)
    cogl_buffer_unmap (mapped_indices_buffer);
  if (free_pipeline)
    g_object_unref (pipeline);
}

/* VK_FORMAT_A2B10G10R10 / A2R10G10B10 put alpha in bits 31–30.
 * Cogl RGBA_1010102 / BGRA_1010102 put alpha in bits 1–0 and the
 * first color in bits 31–22. XRGB/XBGR already match the Vk word.
 */
static void
swizzle_vk_1010102_to_cogl (CoglPixelFormat format,
                            uint8_t        *data,
                            int             width,
                            int             height,
                            int             rowstride)
{
  CoglPixelFormat base = format & ~COGL_PREMULT_BIT;
  int y, x;

  if (base != COGL_PIXEL_FORMAT_RGBA_1010102 &&
      base != COGL_PIXEL_FORMAT_BGRA_1010102)
    return;

  for (y = 0; y < height; y++)
    {
      uint8_t *row = data + (size_t) y * rowstride;

      for (x = 0; x < width; x++)
        {
          uint32_t vk, out;
          uint32_t a, g, hi, lo;

          memcpy (&vk, row + (size_t) x * 4, sizeof (vk));
          a = (vk >> 30) & 3u;
          hi = (vk >> 20) & 0x3ffu;
          g = (vk >> 10) & 0x3ffu;
          lo = vk & 0x3ffu;
          /* lo is R for A2B10G10R10 and B for A2R10G10B10 — both
           * belong in Cogl bits 31–22. hi is the other color.
           */
          out = (lo << 22) | (g << 12) | (hi << 2) | a;
          memcpy (row + (size_t) x * 4, &out, sizeof (out));
        }
    }
}

/* RGBX / XRGB images are stored as 4-component VkFormats, so a clear's
 * alpha lands in the pad slot. GL reads that pad as 1. Same-format
 * copies skip the packer that would rewrite it.
 */
static void
rewrite_missing_alpha_as_one (CoglBitmap *bitmap)
{
  CoglPixelFormat format = cogl_bitmap_get_format (bitmap);
  int base = format & 0x0f;
  int bpp;
  int comp_bytes;
  int pad;
  uint16_t one16;
  int width, height, rowstride, y, x;
  uint8_t *data;
  g_autoptr (GError) error = NULL;

  if (format & (COGL_A_BIT | COGL_DEPTH_BIT | COGL_STENCIL_BIT))
    return;

  /* 3 = *8888, 10 = *16161616 unorm, 11 = *FP_16161616. Not 1010102. */
  if (base != 3 && base != 10 && base != 11)
    return;

  bpp = cogl_pixel_format_get_bytes_per_pixel (format, 0);
  if (base == 3)
    {
      if (bpp != 4)
        return;
      comp_bytes = 1;
      one16 = 0xff;
    }
  else if (base == 10)
    {
      if (bpp != 8)
        return;
      comp_bytes = 2;
      one16 = 0xffff;
    }
  else
    {
      if (bpp != 8)
        return;
      comp_bytes = 2;
      one16 = 0x3c00; /* half-float 1.0 */
    }

  pad = (format & COGL_AFIRST_BIT) ? 0 : 3;
  width = cogl_bitmap_get_width (bitmap);
  height = cogl_bitmap_get_height (bitmap);
  rowstride = cogl_bitmap_get_rowstride (bitmap);
  data = cogl_bitmap_map (bitmap,
                          COGL_BUFFER_ACCESS_READ | COGL_BUFFER_ACCESS_WRITE,
                          0, &error);
  if (!data)
    return;

  for (y = 0; y < height; y++)
    {
      uint8_t *row = data + (size_t) y * rowstride;

      for (x = 0; x < width; x++)
        {
          uint8_t *p = row + (size_t) x * bpp + (size_t) pad * comp_bytes;

          if (comp_bytes == 1)
            p[0] = (uint8_t) one16;
          else
            memcpy (p, &one16, sizeof (one16));
        }
    }

  cogl_bitmap_unmap (bitmap);
}

static gboolean
cogl_framebuffer_vk_read_pixels_into_bitmap (CoglFramebufferDriver  *driver,
                                             int                     x,
                                             int                     y,
                                             CoglReadPixelsFlags     source,
                                             CoglBitmap             *bitmap,
                                             GError                **error)
{
  CoglFramebufferVk *fb_vk = COGL_FRAMEBUFFER_DRIVER_VK (driver);
  CoglDriverVk *driver_vk;
  CoglVkDevice *device;
  CoglVkImage *color;
  int width, height, rowstride;
  CoglPixelFormat format;
  uint8_t *dst;
  VkBufferCreateInfo buf_info;
  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory staging_mem = VK_NULL_HANDLE;
  VkMemoryRequirements reqs;
  VkMemoryAllocateInfo alloc_info;
  VkCommandBufferAllocateInfo cb_alloc;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkCommandPool cmd_pool = VK_NULL_HANDLE;
  VkCommandBufferBeginInfo begin_info = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  VkBufferImageCopy region;
  VkSubmitInfo submit_info;
  VkFenceCreateInfo fence_info = {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
  };
  VkFence fence = VK_NULL_HANDLE;
  void *mapped = NULL;
  int mem_type = -1;
  uint32_t i;
  VkResult res;
  gboolean ok = FALSE;
  CoglPixelFormat image_format;
  uint32_t image_bpp = 4;
  int copy_rowstride;
  gboolean convert_readback = FALSE;
  CoglBitmap *destination_bitmap = bitmap;
  g_autoptr (CoglBitmap) readback_bitmap = NULL;

  /* Readback waits for the staging copy before returning pixels to Cogl. */
  if (!ensure_color_image (fb_vk))
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR,
                           COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "Vulkan framebuffer has no color image");
      return FALSE;
    }

  driver_vk = peek_driver_vk (driver);
  if (!driver_vk || !driver_vk->vk_device)
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR,
                           COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "Vulkan device missing");
      return FALSE;
    }

  device = driver_vk->vk_device;
  color = fb_vk->color;
  width = cogl_bitmap_get_width (bitmap);
  height = cogl_bitmap_get_height (bitmap);
  rowstride = cogl_bitmap_get_rowstride (bitmap);
  format = cogl_bitmap_get_format (bitmap);

  /* Convert, swizzle and flip in readable CPU storage. The caller may
   * provide a write-only bitmap, and its row padding must remain intact.
   */
  readback_bitmap = cogl_bitmap_new_with_size (
    cogl_framebuffer_get_context (
      cogl_framebuffer_driver_get_framebuffer (driver)), width, height, format);
  bitmap = readback_bitmap;
  rowstride = cogl_bitmap_get_rowstride (bitmap);

  /* vkCmdCopyImageToBuffer copies image texels, not the requested
   * Cogl format. Size the staging row for the image, then CPU-convert
   * (RG16 / RGB888 reads were interpreting the next component as the
   * next pixel: rgba16 red came back as blue 2).
   */
  image_format = color->cogl_format;
  if (image_format == COGL_PIXEL_FORMAT_ANY)
    image_format = format;
  /* Atlas backing images are RGBA_8888 with the premult bit clear, but
   * the texels are already premultiplied. get_data overrides the
   * offscreen internal format to the meta texture (RGBA_8888_PRE) so a
   * GL read returns those bytes unchanged. Use that logical format when
   * only the premult flag differs; otherwise straight texels get
   * premultiplied again (0xef → 0xe0).
   */
  {
    CoglFramebuffer *fb = cogl_framebuffer_driver_get_framebuffer (driver);
    CoglPixelFormat fb_format = cogl_framebuffer_get_internal_format (fb);

    if (fb_format != COGL_PIXEL_FORMAT_ANY &&
        (fb_format & ~COGL_PREMULT_BIT) ==
        (image_format & ~COGL_PREMULT_BIT))
      image_format = (image_format & ~COGL_PREMULT_BIT) |
                     (fb_format & COGL_PREMULT_BIT);
  }
  image_bpp = (uint32_t) cogl_pixel_format_get_bytes_per_pixel (image_format, 0);
  if (image_bpp == 0)
    image_bpp = 4;
  convert_readback =
    ((image_format & ~COGL_PREMULT_BIT) != (format & ~COGL_PREMULT_BIT)) ||
    (((image_format ^ format) & COGL_PREMULT_BIT) != 0 &&
     (image_format & format & COGL_A_BIT) != 0);
  copy_rowstride = width * (int) image_bpp;

  if (cogl_vk_format_from_cogl (format) == VK_FORMAT_UNDEFINED &&
      cogl_vk_format_from_cogl (image_format) == VK_FORMAT_UNDEFINED)
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR,
                           COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "Unsupported readback pixel format");
      return FALSE;
    }

  /* Flush outstanding draws so color image is complete. */
  cogl_vk_debug_log (
    "readback",
    "START fb_color=%p layout=%d pass=%p rect=(%d,%d %dx%d)",
    (void *) color, color ? (int) color->layout : -1,
    driver_vk->current_pass ? (void *) driver_vk->current_pass : NULL,
    x, y, width, height);
  if (driver_vk->current_pass)
    submit_and_sync (driver);
  cogl_vk_debug_log (
    "readback",
    "after flush layout=%d pass=%p",
    color ? (int) color->layout : -1,
    driver_vk->current_pass ? (void *) driver_vk->current_pass : NULL);

  memset (&buf_info, 0, sizeof (buf_info));
  buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buf_info.size = (VkDeviceSize) copy_rowstride * (VkDeviceSize) height;
  buf_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  res = vkCreateBuffer (device->device, &buf_info, NULL, &staging);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkCreateBuffer (readback) failed (%d)", (int) res);
      return FALSE;
    }

  vkGetBufferMemoryRequirements (device->device, staging, &reqs);
  {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties (device->physical_device, &mem_props);
    for (i = 0; i < mem_props.memoryTypeCount; i++)
      {
        if ((reqs.memoryTypeBits & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
          {
            mem_type = (int) i;
            break;
          }
      }
  }
  if (mem_type < 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "No HOST_VISIBLE memory for readback");
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

  /* Prefer oneshot pool — never share the frame command_pool with a
   * just-submitted pass CB (ANV has lost EndCB/QueueSubmit here).
   */
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
    {
      cogl_vk_debug_log ("readback", "AllocateCB failed (%d)", (int) res);
      goto out;
    }

  /* Ensure prior color writes are idle before TRANSFER_SRC barrier+copy.
   * SYNC_FD export alone does not CPU-wait; wrap-modes saw EndCB/Submit
   * abort with no pixels when racing the frame CB.
   */
  if (device->graphics_queue != VK_NULL_HANDLE)
    {
      VkResult wi = vkQueueWaitIdle (device->graphics_queue);
      cogl_vk_debug_log ("readback", "pre-copy WaitIdle res=%d layout=%d",
                         (int) wi, (int) color->layout);
      if (wi != VK_SUCCESS)
        {
          cogl_vk_device_note_vk_result (device, wi);
          goto out;
        }
    }

  res = vkBeginCommandBuffer (cmd, &begin_info);
  if (res != VK_SUCCESS)
    {
      cogl_vk_debug_log ("readback", "BeginCB failed (%d)", (int) res);
      goto out;
    }

  cogl_vk_debug_log (
    "readback",
    "barrier→TRANSFER_SRC layout_before=%d",
    (int) color->layout);
  cogl_vk_image_cmd_barrier (color, cmd,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT);
  cogl_vk_debug_log (
    "readback",
    "copyImageToBuffer layout_now=%d",
    (int) color->layout);

  memset (&region, 0, sizeof (region));
  region.bufferOffset = 0;
  if (convert_readback)
    {
      /* Tightly packed image texels. 0 means bufferRowLength = width. */
      region.bufferRowLength = 0;
      cogl_vk_debug_log (
        "readback",
        "copy region image_fmt=%d image_bpp=%u copy_stride=%d "
        "dst_fmt=%d dst_stride=%d convert=1",
        (int) image_format, (unsigned) image_bpp, copy_rowstride,
        (int) format, rowstride);
    }
  else
    {
      uint32_t bpp = cogl_pixel_format_get_bytes_per_pixel (format, 0);

      if (bpp == 0)
        bpp = 4;
      region.bufferRowLength = (uint32_t) (rowstride / bpp);
      cogl_vk_debug_log (
        "readback",
        "copy region format=%d bpp=%u rowstride=%d bufferRowLength=%u",
        (int) format, (unsigned) bpp, rowstride,
        (unsigned) region.bufferRowLength);
    }
  region.bufferImageHeight = 0;
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageOffset.x = x;
  region.imageOffset.y = y;
  region.imageExtent.width = (uint32_t) width;
  region.imageExtent.height = (uint32_t) height;
  region.imageExtent.depth = 1;

  vkCmdCopyImageToBuffer (cmd, color->image, color->layout,
                          staging, 1, &region);

  /* Restore COLOR_ATTACHMENT so the next begin_render LOAD matches the
   * render-pass initialLayout without relying solely on a late barrier.
   */
  cogl_vk_debug_log (
    "readback",
    "restore→COLOR_ATTACHMENT layout_before=%d",
    (int) color->layout);
  cogl_vk_image_cmd_barrier (color, cmd,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT,
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
  cogl_vk_debug_log (
    "readback",
    "restore done layout_now=%d",
    (int) color->layout);

  res = vkEndCommandBuffer (cmd);
  if (res != VK_SUCCESS)
    {
      cogl_vk_debug_log ("readback", "EndCB failed (%d)", (int) res);
      cogl_vk_device_note_vk_result (device, res);
      goto out;
    }

  res = vkCreateFence (device->device, &fence_info, NULL, &fence);
  if (res != VK_SUCCESS)
    {
      cogl_vk_debug_log ("readback", "CreateFence failed (%d)", (int) res);
      goto out;
    }

  memset (&submit_info, 0, sizeof (submit_info));
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &cmd;
  res = vkQueueSubmit (device->graphics_queue, 1, &submit_info, fence);
  if (res != VK_SUCCESS)
    {
      cogl_vk_debug_log ("readback", "QueueSubmit failed (%d)", (int) res);
      cogl_vk_device_note_vk_result (device, res);
      goto out;
    }

  res = vkWaitForFences (device->device, 1, &fence, VK_TRUE, UINT64_MAX);
  if (res != VK_SUCCESS)
    {
      cogl_vk_debug_log ("readback", "WaitFence failed (%d)", (int) res);
      cogl_vk_device_note_vk_result (device, res);
      goto out;
    }

  cogl_vk_debug_log (
    "readback",
    "GPU wait done layout=%d (restored COLOR_ATTACHMENT)",
    (int) color->layout);

  res = vkMapMemory (device->device, staging_mem, 0, reqs.size, 0, &mapped);
  if (res != VK_SUCCESS)
    goto out;

  if (convert_readback)
    {
      CoglFramebuffer *fb =
        cogl_framebuffer_driver_get_framebuffer (driver);
      CoglContext *ctx = cogl_framebuffer_get_context (fb);
      g_autofree uint8_t *raw =
        g_malloc ((gsize) copy_rowstride * (gsize) height);
      g_autoptr (CoglBitmap) src_bmp = NULL;

      memcpy (raw, mapped, (size_t) copy_rowstride * (size_t) height);
      vkUnmapMemory (device->device, staging_mem);
      mapped = NULL;

      swizzle_vk_1010102_to_cogl (image_format, raw, width, height,
                                  copy_rowstride);

      src_bmp = cogl_bitmap_new_for_data (ctx, width, height, image_format,
                                          copy_rowstride, raw);
      if (!_cogl_bitmap_convert_into_bitmap (src_bmp, bitmap, error))
        goto out;
    }
  else
    {
      dst = cogl_bitmap_map (bitmap, COGL_BUFFER_ACCESS_WRITE, 0, error);
      if (!dst)
        {
          vkUnmapMemory (device->device, staging_mem);
          mapped = NULL;
          goto out;
        }

      for (i = 0; i < height; i++)
        memcpy (dst + (size_t) i * rowstride,
                (uint8_t *) mapped + (size_t) i * copy_rowstride,
                (size_t) width * image_bpp);
      swizzle_vk_1010102_to_cogl (format, dst, width, height, rowstride);

      {
        CoglFramebuffer *fb_dbg =
          cogl_framebuffer_driver_get_framebuffer (driver);
        cogl_vk_debug_readback (fb_dbg, x, y, width, height, dst, rowstride);
      }

      cogl_bitmap_unmap (bitmap);
      vkUnmapMemory (device->device, staging_mem);
      mapped = NULL;
    }

  rewrite_missing_alpha_as_one (bitmap);

  /* Match GL read_pixels: CPU/PACK invert only when the FB is *not*
   * y-flipped. Offscreen FBs are always y-flipped; VkImage is top-left
   * and bake already places Cogl y=0 where callers expect, so inverting
   * here double-flips (wrap-modes blue assert).
   */
  {
    CoglFramebuffer *fb =
      cogl_framebuffer_driver_get_framebuffer (driver);
    CoglDriver *cogl_driver = cogl_context_get_driver (
      cogl_framebuffer_get_context (fb));

    if (cogl_driver_has_feature (cogl_driver, COGL_FEATURE_ID_MESA_PACK_INVERT) &&
        (source & COGL_READ_PIXELS_NO_FLIP) == 0 &&
        !cogl_framebuffer_is_y_flipped (fb))
      {
        uint8_t *pixels;
        uint8_t *temprow;
        int yy;

        pixels = cogl_bitmap_map (bitmap,
                                  COGL_BUFFER_ACCESS_READ |
                                  COGL_BUFFER_ACCESS_WRITE,
                                  0, error);
        if (!pixels)
          goto out;

        temprow = g_alloca ((gsize) rowstride);
        for (yy = 0; yy < height / 2; yy++)
          {
            memcpy (temprow, pixels + yy * rowstride, (gsize) rowstride);
            memcpy (pixels + yy * rowstride,
                    pixels + (height - 1 - yy) * rowstride,
                    (gsize) rowstride);
            memcpy (pixels + (height - 1 - yy) * rowstride,
                    temprow, (gsize) rowstride);
          }
        cogl_bitmap_unmap (bitmap);
      }
  }

  if (cogl_vk_debug_enabled ("readback") &&
      (((format & 0x0f) == 13) || ((image_format & 0x0f) == 13)))
    {
      uint8_t *px;
      g_autoptr (GError) px_error = NULL;

      px = cogl_bitmap_map (bitmap,
                            COGL_BUFFER_ACCESS_READ, 0, &px_error);
      if (px)
        {
          uint32_t word = 0;

          memcpy (&word, px, sizeof (word));
          g_printerr ("rgb10 img_fmt=%d dst_fmt=%d vk_fmt=%d convert=%d "
                      "word=0x%08x ch10=%u a2=%u\n",
                      (int) image_format, (int) format,
                      color ? (int) color->format : -1,
                      convert_readback, word,
                      (word >> 20) & 0x3ffu, (word >> 30) & 0x3u);
          cogl_bitmap_unmap (bitmap);
        }
    }

  ok = _cogl_bitmap_copy_subregion (bitmap, destination_bitmap,
                                    0, 0, 0, 0, width, height, error);

out:
  if (fence != VK_NULL_HANDLE)
    vkDestroyFence (device->device, fence, NULL);
  if (cmd != VK_NULL_HANDLE && cmd_pool != VK_NULL_HANDLE)
    vkFreeCommandBuffers (device->device, cmd_pool, 1, &cmd);
  if (staging != VK_NULL_HANDLE)
    vkDestroyBuffer (device->device, staging, NULL);
  if (staging_mem != VK_NULL_HANDLE)
    vkFreeMemory (device->device, staging_mem, NULL);

  if (!ok && error && !*error)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Vulkan read_pixels failed");
  return ok;
}

static gboolean
cogl_framebuffer_vk_blit (CoglFramebufferDriver *driver,
                          CoglFramebufferDriver *destination,
                          int                    src_x1,
                          int                    src_y1,
                          int                    src_x2,
                          int                    src_y2,
                          int                    dst_x1,
                          int                    dst_y1,
                          int                    dst_x2,
                          int                    dst_y2,
                          GError               **error)
{
  CoglFramebufferVk *src_vk = COGL_FRAMEBUFFER_DRIVER_VK (driver);
  CoglFramebufferVk *dst_vk = COGL_FRAMEBUFFER_DRIVER_VK (destination);
  CoglDriverVk *driver_vk;
  CoglVkPass *pass;

  if (!ensure_color_image (src_vk) || !ensure_color_image (dst_vk))
    return FALSE;

  driver_vk = peek_driver_vk (destination);
  if (!driver_vk)
    return FALSE;

  pass = ensure_pass (driver_vk);
  if (!pass)
    return FALSE;

  return cogl_vk_cmd_blit_images (pass, src_vk->color, dst_vk->color,
                                  src_x1, src_y1, src_x2, src_y2,
                                  dst_x1, dst_y1, dst_x2, dst_y2,
                                  0, 0, error);
}

static void
destroy_color_framebuffer (CoglVkImage *color)
{
  VkResult res;

  if (!color || color->framebuffer == VK_NULL_HANDLE || !color->device)
    return;

  /* Flush is asynchronous, including for offscreen targets. Retire the
   * framebuffer only after recorded and submitted uses have completed.
   */
  if (color->device->owner)
    cogl_driver_vk_flush_current_pass (color->device->owner);
  res = vkQueueWaitIdle (color->device->graphics_queue);
  cogl_vk_device_note_vk_result (color->device, res);
  if (res != VK_SUCCESS && res != VK_ERROR_DEVICE_LOST)
    g_error ("Cogl Vulkan: cannot retire framebuffer (%d)", (int) res);

  vkDestroyFramebuffer (color->device->device, color->framebuffer, NULL);
  color->framebuffer = VK_NULL_HANDLE;
  color->framebuffer_render_pass = VK_NULL_HANDLE;
  color->framebuffer_ds = NULL;
}

static void
cogl_framebuffer_vk_dispose (GObject *object)
{
  CoglFramebufferVk *fb_vk = COGL_FRAMEBUFFER_DRIVER_VK (object);
  CoglDriverVk *driver_vk = peek_driver_vk (COGL_FRAMEBUFFER_DRIVER (object));

  /* test-journal: unref offscreen before get_data while the draw pass may
   * still have an open RP on this color+DS. Freeing DS (renderbuffers) then
   * EndRenderPass is a UAF — latent without MALLOC_PERTURB, SIGSEGV with it.
   */
  if (driver_vk && driver_vk->current_pass)
    {
      CoglVkPass *pass = driver_vk->current_pass;

      if (pass->in_render_pass &&
          (pass->render_color == fb_vk->color ||
           pass->render_color == fb_vk->ms_color ||
           (fb_vk->depth_stencil &&
            pass->render_depth_stencil == fb_vk->depth_stencil)))
        {
          g_autoptr (GError) error = NULL;

          cogl_vk_debug_log (
            "draw",
            "fb_dispose: flush open RP before destroying DS/FB pass=%p",
            (void *) pass);
          if (!cogl_vk_pass_submit (pass, &error))
            {
              cogl_vk_debug_log (
                "draw",
                "fb_dispose: submit failed: %s",
                error ? error->message : "?");
            }
          /* Pass tickets defer command buffer retirement. The framebuffer
           * and attachment teardown below waits before destroying objects.
           */
          cogl_driver_vk_store_sync_fd_from_pass (driver_vk);
          g_clear_pointer (&driver_vk->current_pass, cogl_vk_pass_free);
        }
    }

  /* VkFramebuffer references the DS image view — destroy before DS free. */
  destroy_color_framebuffer (fb_vk->color);
  destroy_color_framebuffer (fb_vk->ms_color);

  if (fb_vk->color)
    fb_vk->color->attached_depth_stencil = NULL;
  if (fb_vk->ms_color)
    fb_vk->ms_color->attached_depth_stencil = NULL;

  fb_vk->depth_stencil = NULL;
  g_clear_pointer (&fb_vk->renderbuffers, cogl_vk_renderbuffers_unref);

  if (fb_vk->owns_ms_color)
    g_clear_pointer (&fb_vk->ms_color, cogl_vk_image_free);
  else
    fb_vk->ms_color = NULL;

  if (fb_vk->owns_color)
    g_clear_pointer (&fb_vk->color, cogl_vk_image_free);
  else
    fb_vk->color = NULL;

  G_OBJECT_CLASS (cogl_framebuffer_vk_parent_class)->dispose (object);
}

static CoglRenderbuffers *
cogl_framebuffer_vk_peek_renderbuffers (CoglFramebufferDriver *driver)
{
  CoglFramebufferVk *fb_vk = COGL_FRAMEBUFFER_DRIVER_VK (driver);

  /* Lazily allocate shared DS RBs so Meta GBM flip can peek/bind. */
  if (!fb_vk->renderbuffers && !fb_vk->disable_depth_and_stencil)
    {
      CoglDriverVk *driver_vk = peek_driver_vk (driver);

      if (driver_vk)
        ensure_depth_stencil (fb_vk, driver_vk,
                              framebuffer_msaa_samples (fb_vk));
    }
  return fb_vk->renderbuffers;
}

static void
cogl_framebuffer_vk_bind_renderbuffers (CoglFramebufferDriver *driver,
                                        CoglRenderbuffers     *renderbuffers)
{
  CoglFramebufferVk *fb_vk = COGL_FRAMEBUFFER_DRIVER_VK (driver);
  VkSampleCountFlagBits need_samples;
  CoglVkImage *draw_ds;

  if (fb_vk->disable_depth_and_stencil)
    return;

  need_samples = framebuffer_msaa_samples (fb_vk);
  if (need_samples == 0)
    need_samples = VK_SAMPLE_COUNT_1_BIT;

  if (fb_vk->renderbuffers == renderbuffers)
    {
      fb_vk->depth_stencil = renderbuffers_draw_ds (renderbuffers);
      attach_ds_to_color_targets (fb_vk);
      return;
    }

  /* Same-driver reattach: share packed / depth-only / stencil-only images
   * across same-size FBOs. Sample or size mismatch → leave unbound and let
   * prepare_draw_targets realloc.
   */
  if (renderbuffers &&
      (renderbuffers->samples != need_samples ||
       (fb_vk->color &&
        (renderbuffers->width != fb_vk->color->width ||
         renderbuffers->height != fb_vk->color->height))))
    {
      fb_vk->depth_stencil = NULL;
      attach_ds_to_color_targets (fb_vk);
      return;
    }

  draw_ds = renderbuffers_draw_ds (renderbuffers);
  destroy_color_framebuffer (fb_vk->color);
  destroy_color_framebuffer (fb_vk->ms_color);
  cogl_vk_renderbuffers_ref (renderbuffers);
  g_clear_pointer (&fb_vk->renderbuffers, cogl_vk_renderbuffers_unref);
  fb_vk->renderbuffers = renderbuffers;
  fb_vk->depth_stencil = draw_ds;
  attach_ds_to_color_targets (fb_vk);
}

void
cogl_framebuffer_vk_peek_depth_stencil_images (CoglFramebufferVk *fb,
                                               CoglVkImage      **depth,
                                               CoglVkImage      **stencil,
                                               VkImageView       *depth_view,
                                               VkImageView       *stencil_view)
{
  CoglRenderbuffers *rb = fb->renderbuffers;
  *depth_view = rb ? rb->depth_view : VK_NULL_HANDLE;
  *stencil_view = rb ? rb->stencil_view : VK_NULL_HANDLE;
  *depth = rb ? (rb->depth ? rb->depth : rb->depth_stencil) : NULL;
  *stencil = rb ? (rb->stencil ? rb->stencil : rb->depth_stencil) : NULL;
}

static void
cogl_framebuffer_vk_class_init (CoglFramebufferVkClass *klass)
{
  GObjectClass *object_klass = G_OBJECT_CLASS (klass);
  CoglFramebufferDriverClass *driver_klass =
    COGL_FRAMEBUFFER_DRIVER_CLASS (klass);

  object_klass->dispose = cogl_framebuffer_vk_dispose;

  driver_klass->blit = cogl_framebuffer_vk_blit;
  driver_klass->adopt_targets = cogl_framebuffer_vk_adopt_targets;
  driver_klass->query_bits = cogl_framebuffer_vk_query_bits;
  driver_klass->clear = cogl_framebuffer_vk_clear;
  driver_klass->finish = cogl_framebuffer_vk_finish;
  driver_klass->flush = cogl_framebuffer_vk_flush;
  driver_klass->discard_buffers = cogl_framebuffer_vk_discard_buffers;
  driver_klass->draw_attributes = cogl_framebuffer_vk_draw_attributes;
  driver_klass->draw_indexed_attributes =
    cogl_framebuffer_vk_draw_indexed_attributes;
  driver_klass->read_pixels_into_bitmap =
    cogl_framebuffer_vk_read_pixels_into_bitmap;
  driver_klass->bind_renderbuffers = cogl_framebuffer_vk_bind_renderbuffers;
  driver_klass->peek_renderbuffers = cogl_framebuffer_vk_peek_renderbuffers;
}

static void
cogl_framebuffer_vk_init (CoglFramebufferVk *fb)
{
  fb->color = NULL;
  fb->owns_color = FALSE;
  fb->depth_stencil = NULL;
  fb->ms_color = NULL;
  fb->owns_ms_color = FALSE;
  fb->renderbuffers = NULL;
  fb->disable_depth_and_stencil = FALSE;
}
