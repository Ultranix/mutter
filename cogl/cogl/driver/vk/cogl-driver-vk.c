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

/* Cogl driver implementation selected by COGL_DRIVER=vulkan.
 * Draws are recorded into command batches. Submission completion is exposed
 * through Cogl fences and SYNC_FD exports for Mutter native backend.
 */

#include "config.h"

#include "cogl/driver/vk/cogl-driver-vk-private.h"

#include "cogl/cogl-render-device.h"
#include "cogl/driver/vk/cogl-buffer-impl-vk-private.h"
#include "cogl/driver/vk/cogl-framebuffer-vk-private.h"
#include "cogl/driver/vk/cogl-texture-driver-vk-private.h"
#include "cogl/driver/vk/cogl-pass-vk-private.h"
#include "cogl/driver/vk/cogl-fence-vk-private.h"
#include "cogl/driver/vk/cogl-vk-debug-private.h"

#include "cogl/cogl-private.h"
#include "cogl/cogl-context-private.h"
#include "cogl/cogl-renderer-private.h"
#include "cogl/cogl-driver.h"
#include "cogl/cogl-clip-stack.h"
#include "cogl/cogl-framebuffer.h"
#include "cogl/cogl-framebuffer-private.h"
#include "cogl/cogl-graphene.h"
#include "cogl/cogl-matrix-stack.h"
#include "mtk/mtk.h"
#include "cogl/cogl-boxed-value.h"
#include "cogl/cogl-attribute-private.h"
#include "cogl/cogl-sampler-cache-private.h"
#include "cogl/cogl-pipeline.h"
#include "cogl/cogl-pipeline-private.h"
#include "cogl/cogl-pipeline-state.h"
#include "cogl/driver/vk/cogl-image-vk-private.h"

#include <string.h>

#ifndef GL_MIN
#define GL_MIN 0x8007
#endif
#ifndef GL_MAX
#define GL_MAX 0x8008
#endif
#ifndef GL_FUNC_ADD
#define GL_FUNC_ADD 0x8006
#endif
#ifndef GL_FUNC_SUBTRACT
#define GL_FUNC_SUBTRACT 0x800A
#endif
#ifndef GL_FUNC_REVERSE_SUBTRACT
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#endif

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

G_DEFINE_FINAL_TYPE (CoglDriverVk, cogl_driver_vk, COGL_TYPE_DRIVER)

CoglVkDevice *
cogl_driver_vk_peek_device (CoglDriverVk * driver)
{
  g_return_val_if_fail (COGL_IS_DRIVER_VK (driver), NULL);
  return driver->vk_device;
}

CoglVkPipelineCache *
cogl_driver_vk_peek_pipeline_cache (CoglDriverVk *driver)
{
  g_return_val_if_fail (COGL_IS_DRIVER_VK (driver), NULL);
  return driver->pipeline_cache;
}

void
cogl_driver_vk_store_sync_fd_from_pass (CoglDriverVk *driver)
{
  g_autoptr (GError) error = NULL;
  int fd;

  g_return_if_fail (COGL_IS_DRIVER_VK (driver));

  if (!driver->current_pass || !driver->current_pass->fence)
    {
      cogl_vk_debug_log ("draw", "store_sync_fd: no pass/fence");
      return;
    }

  /* Offscreen flush has no external presenter and, like GL flush, must not
   * wait for GPU completion. The pass ticket retains in-flight resources;
   * readback, finish and resource destruction perform their own waits.
   */
  if (driver->pending_drm_fd < 0)
    {
      if (driver->latest_sync_fd >= 0)
        close (driver->latest_sync_fd);
      driver->latest_sync_fd = -1;
      return;
    }

  /* An external presenter must receive a current-frame fd, or wait for this
   * submission on the CPU before presenting without one.
   */
  if (!driver->vk_device ||
      !driver->vk_device->has_external_semaphore_fd ||
      !driver->vk_device->vkGetSemaphoreFdKHR)
    goto wait_for_completion;

  cogl_vk_debug_log (
    "draw",
    "store_sync_fd: export begin fence=%p binary=%p",
    (void *) driver->current_pass->fence,
    (void *) (uintptr_t) driver->current_pass->fence->binary);

  fd = cogl_vk_fence_ticket_export_sync_fd (driver->current_pass->fence,
                                            &error);
  if (error)
    {
      cogl_vk_debug_log ("draw", "store_sync_fd: export FAIL: %s",
                         error ? error->message : "unknown");
      g_debug ("Cogl Vulkan: SYNC_FD export failed: %s",
               error ? error->message : "unknown");
      goto wait_for_completion;
    }

  cogl_vk_debug_log ("draw", "store_sync_fd: export OK fd=%d", fd);

  if (driver->latest_sync_fd >= 0)
    close (driver->latest_sync_fd);
  driver->latest_sync_fd = fd;
  return;

wait_for_completion:
  {
    VkResult res;

    if (!driver->vk_device ||
        driver->vk_device->graphics_queue == VK_NULL_HANDLE)
      g_error ("Cogl Vulkan: no queue to complete presentation");
    res = vkQueueWaitIdle (driver->vk_device->graphics_queue);
    cogl_vk_device_note_vk_result (driver->vk_device, res);
    if (res != VK_SUCCESS)
      g_error ("Cogl Vulkan: presentation wait failed (%d)", (int) res);
    driver->current_pass->fence->signaled_cached = TRUE;
    if (driver->latest_sync_fd >= 0)
      close (driver->latest_sync_fd);
    driver->latest_sync_fd = -1;
  }
}

void
cogl_driver_vk_flush_current_pass (CoglDriverVk *driver)
{
  g_autoptr (GError) error = NULL;

  if (!driver || !driver->current_pass || driver->flushing_current_pass)
    return;

  driver->flushing_current_pass = TRUE;

  cogl_vk_debug_log (
    "tex",
    "flush_current_pass: submit+free pass=%p in_rp=%d (before GPU object destroy)",
    (void *) driver->current_pass,
    driver->current_pass->in_render_pass);

  if (!cogl_vk_pass_submit (driver->current_pass, &error))
    {
      cogl_vk_debug_log ("tex", "flush_current_pass: submit FAIL: %s",
                         error ? error->message : "unknown");
      g_clear_pointer (&driver->current_pass, cogl_vk_pass_free);
      driver->flushing_current_pass = FALSE;
      return;
    }

  cogl_driver_vk_store_sync_fd_from_pass (driver);
  g_clear_pointer (&driver->current_pass, cogl_vk_pass_free);
  driver->flushing_current_pass = FALSE;
}

int
cogl_driver_vk_get_latest_sync_fd (CoglDriverVk *driver)
{
  int fd;

  g_return_val_if_fail (COGL_IS_DRIVER_VK (driver), -1);

  fd = driver->latest_sync_fd;
  /* Dup so Meta can take ownership while we keep a copy for later polls. */
  if (fd >= 0)
    {
      int dup_fd = fcntl (fd, F_DUPFD_CLOEXEC, 0);
      if (dup_fd < 0)
        dup_fd = dup (fd);
      if (dup_fd < 0)
        {
          VkResult res;

          /* Meta interprets -1 as no wait needed, not as a duplication error.
           */
          if (!driver->vk_device ||
              driver->vk_device->graphics_queue == VK_NULL_HANDLE)
            g_error ("Cogl Vulkan: no queue after sync fd duplication failure");
          res = vkQueueWaitIdle (driver->vk_device->graphics_queue);
          cogl_vk_device_note_vk_result (driver->vk_device, res);
          if (res != VK_SUCCESS)
            g_error ("Cogl Vulkan: sync fd duplication fallback failed (%d)",
                     (int) res);
        }
      return dup_fd;
    }
  return -1;
}

VkSampler
cogl_driver_vk_peek_sampler (CoglDriverVk                *driver,
                             const CoglSamplerCacheEntry *entry)
{
  gpointer samp;

  g_return_val_if_fail (COGL_IS_DRIVER_VK (driver), VK_NULL_HANDLE);

  if (entry && driver->samplers)
    {
      samp = g_hash_table_lookup (driver->samplers, entry);
      if (samp)
        return (VkSampler) (gsize) samp;
    }

  /* Sampler cache keeps separate cogl-key vs GL-canonical entry pointers.
   * sampler_init() is only invoked for the GL-canonical entry, so a layer's
   * cogl entry pointer misses the hash and used to fall back to the default
   * CLAMP sampler — wrap-modes saw wrap_s=GL_REPEAT but samp==default.
   * Lazily create a VkSampler keyed by this entry pointer.
   */
  if (entry && driver->vk_device)
    {
      CoglDriverClass *klass = COGL_DRIVER_GET_CLASS (driver);

      if (klass->sampler_init)
        klass->sampler_init (COGL_DRIVER (driver),
                             (CoglSamplerCacheEntry *) entry);
      if (driver->samplers)
        {
          samp = g_hash_table_lookup (driver->samplers, entry);
          if (samp)
            {
              cogl_vk_debug_log (
                "tex",
                "peek_sampler lazy-init entry=%p wrap_s=%d wrap_t=%d → samp=%p",
                (void *) entry,
                (int) entry->wrap_mode_s, (int) entry->wrap_mode_t,
                samp);
              return (VkSampler) (gsize) samp;
            }
        }
    }

  cogl_vk_debug_log (
    "tex",
    "peek_sampler FALLBACK default entry=%p wrap_s=%d wrap_t=%d",
    (void *) entry,
    entry ? (int) entry->wrap_mode_s : -1,
    entry ? (int) entry->wrap_mode_t : -1);

  if (driver->pipeline_cache)
    return driver->pipeline_cache->sampler;

  return VK_NULL_HANDLE;
}

const CoglBoxedValue *
cogl_driver_vk_peek_pending_uniform (CoglDriverVk *driver,
                                     GLint         location)
{
  g_return_val_if_fail (COGL_IS_DRIVER_VK (driver), NULL);

  if (!driver->pending_uniforms || location < 0)
    return NULL;

  return g_hash_table_lookup (driver->pending_uniforms,
                              GINT_TO_POINTER (location));
}

gboolean
cogl_driver_vk_initialize (CoglDriverVk  *driver_vk,
                           CoglRenderer  *renderer,
                           GError       **error)
{
  CoglDriver *driver = COGL_DRIVER (driver_vk);
  g_autoptr (GError) local_error = NULL;
  CoglRenderDevice *render_device = cogl_renderer_get_render_device (renderer);
  int drm_fd = render_device ? cogl_render_device_get_drm_fd (render_device) : -1;

  if (driver_vk->vk_device)
    return TRUE;

  if (drm_fd < 0)
    {
      const char *env = g_getenv ("COGL_VULKAN_DRM_FD");
      if (env && *env)
        drm_fd = atoi (env);
    }

  driver_vk->pending_drm_fd = drm_fd;
  driver_vk->vk_device = cogl_vk_device_new (drm_fd, &local_error);
  if (!driver_vk->vk_device)
    {
      g_propagate_prefixed_error (error, g_steal_pointer (&local_error),
                                  "Failed to initialize Vulkan device: ");
      return FALSE;
    }

  driver_vk->vk_device->owner = driver_vk;
#if defined (HAVE_EGL) && defined (EGL_KHR_image_base)
  cogl_driver_set_feature (driver, COGL_FEATURE_ID_TEXTURE_2D_FROM_EGL_IMAGE,
                           driver_vk->vk_device->has_dma_buf);
#endif

  g_clear_error (&local_error);
  driver_vk->pipeline_cache =
    cogl_vk_pipeline_cache_new (driver_vk->vk_device, &local_error);
  if (!driver_vk->pipeline_cache)
    {
      g_propagate_prefixed_error (error, g_steal_pointer (&local_error),
                                  "Failed to initialize Vulkan pipeline cache: ");
      g_clear_pointer (&driver_vk->vk_device, cogl_vk_device_free);
      return FALSE;
    }

  driver_vk->dmabuf_memo = cogl_vk_dmabuf_memo_new ();

  /* TIMESTAMP_QUERY: feature bit after device knows timestampPeriod.
   * update_features runs before context_init, so set it here.
   */
  if (driver_vk->vk_device->has_timestamp_query)
    cogl_driver_set_feature (driver, COGL_FEATURE_ID_TIMESTAMP_QUERY, TRUE);

  return TRUE;
}

static const char *
cogl_driver_vk_get_vendor (CoglDriver *driver)
{
  (void) driver;
  return "Vulkan";
}

static gboolean
cogl_driver_vk_update_features (CoglDriver   *driver,
                                CoglRenderer *renderer,
                                GError      **error)
{
  (void) renderer;
  (void) error;

  cogl_driver_set_feature (driver, COGL_FEATURE_ID_UNSIGNED_INT_INDICES, TRUE);
  cogl_driver_set_feature (driver, COGL_FEATURE_ID_BLIT_FRAMEBUFFER, TRUE);
  cogl_driver_set_feature (driver, COGL_FEATURE_ID_TEXTURE_FORMAT_BGRA8888, TRUE);
  cogl_driver_set_feature (driver, COGL_FEATURE_ID_ALPHA_TEXTURES, TRUE);
  cogl_driver_set_feature (driver, COGL_FEATURE_ID_TEXTURE_FORMAT_SIZED_RGBA, TRUE);
  cogl_driver_set_feature (driver, COGL_FEATURE_ID_PBOS, TRUE);
  cogl_driver_set_feature (driver, COGL_FEATURE_ID_MAP_BUFFER_FOR_READ, TRUE);
  cogl_driver_set_feature (driver, COGL_FEATURE_ID_MAP_BUFFER_FOR_WRITE, TRUE);
  cogl_driver_set_feature (driver, COGL_FEATURE_ID_READ_PIXELS_ANY_STRIDE, TRUE);
  cogl_driver_set_feature (driver, COGL_FEATURE_ID_TEXTURE_RG, TRUE);
  cogl_driver_set_feature (driver, COGL_FEATURE_ID_QUERY_FRAMEBUFFER_BITS, TRUE);
  /* Vulkan immutable/sampler objects cover GL sampler-object feature. */
  cogl_driver_set_feature (driver, COGL_FEATURE_ID_SAMPLER_OBJECTS, TRUE);
  /* Partial texture upload implemented in texture-driver-vk. */
  cogl_driver_set_feature (driver, COGL_FEATURE_ID_UNPACK_SUBIMAGE, TRUE);

  /* Use image-view component mapping to preserve Cogl pixel format swizzles. */
  cogl_driver_set_feature (driver, COGL_FEATURE_ID_TEXTURE_HALF_FLOAT, TRUE);
  cogl_driver_set_feature (driver, COGL_FEATURE_ID_TEXTURE_RGBA1010102, TRUE);
  cogl_driver_set_feature (driver, COGL_FEATURE_ID_TEXTURE_NORM16, TRUE);
  cogl_driver_set_feature (driver, COGL_FEATURE_ID_TEXTURE_SWIZZLE, TRUE);
  cogl_driver_set_feature (driver, COGL_FEATURE_ID_FORMAT_CONVERSION, TRUE);
  /* Host-visible linear upload can invert Y in CPU path when needed. */
  cogl_driver_set_feature (driver, COGL_FEATURE_ID_MESA_PACK_INVERT, TRUE);
  cogl_driver_set_feature (driver, COGL_FEATURE_ID_TEXTURE_MAX_LEVEL, TRUE);
  cogl_driver_set_feature (driver, COGL_FEATURE_ID_TEXTURE_LOD_BIAS, TRUE);
  cogl_driver_set_feature (driver, COGL_FEATURE_ID_EXT_PACKED_DEPTH_STENCIL, TRUE);
  cogl_driver_set_feature (driver, COGL_FEATURE_ID_OES_PACKED_DEPTH_STENCIL, TRUE);
#if defined (HAVE_EGL) && defined (EGL_KHR_image_base)
  /* allocate_from_egl_image exports via EGL_MESA_image_dma_buf_export. */
  cogl_driver_set_feature (driver, COGL_FEATURE_ID_TEXTURE_2D_FROM_EGL_IMAGE, TRUE);
#endif
  /* TIMESTAMP_QUERY enabled in context_init once the query pool exists. */

  return TRUE;
}

static gboolean
cogl_driver_vk_format_supports_upload (CoglDriver      *driver,
                                       CoglPixelFormat  format)
{
  (void) driver;
  return cogl_vk_format_from_cogl (format) != VK_FORMAT_UNDEFINED;
}

static CoglGraphicsResetStatus
cogl_driver_vk_get_graphics_reset_status (CoglDriver *driver)
{
  CoglDriverVk *driver_vk = COGL_DRIVER_VK (driver);

  if (!driver_vk->vk_device)
    return COGL_GRAPHICS_RESET_STATUS_NO_ERROR;

  return cogl_vk_device_get_graphics_reset_status (driver_vk->vk_device);
}

static CoglFramebufferDriver *
cogl_driver_vk_create_framebuffer_driver (CoglDriver                         *driver,
                                          CoglFramebuffer                    *framebuffer,
                                          const CoglFramebufferDriverConfig  *driver_config,
                                          GError                            **error)
{
  CoglFramebufferDriver *fb_driver;

  (void) driver;
  (void) error;

  fb_driver = g_object_new (COGL_TYPE_FRAMEBUFFER_DRIVER_VK,
                            "framebuffer", framebuffer,
                            NULL);
  cogl_framebuffer_vk_apply_driver_config (COGL_FRAMEBUFFER_DRIVER_VK (fb_driver),
                                           driver_config);
  return fb_driver;
}

static void
cogl_driver_vk_flush_framebuffer_state (CoglDriver           *driver,
                                        CoglContext          *context,
                                        CoglFramebuffer      *draw_buffer,
                                        CoglFramebuffer      *read_buffer,
                                        CoglFramebufferState  state)
{
  CoglDriverVk *driver_vk = COGL_DRIVER_VK (driver);
  CoglFramebuffer *current_draw_buffer =
    cogl_context_get_current_draw_buffer (context);
  unsigned long differences;

  differences = cogl_context_get_current_draw_buffer_changes (context);
  differences |= ~cogl_context_get_current_draw_buffer_state_flushed (context);
  differences &= state;

  if (current_draw_buffer != draw_buffer)
    {
      if (current_draw_buffer == NULL)
        differences |= state;
      else
        differences |= _cogl_framebuffer_compare (current_draw_buffer,
                                                  draw_buffer,
                                                  state & ~differences);

      cogl_context_set_current_draw_buffer (context, draw_buffer);
      cogl_context_set_current_draw_buffer_state_flushed (context, 0);
    }

  if (cogl_context_get_current_read_buffer (context) != read_buffer &&
      (state & COGL_FRAMEBUFFER_STATE_BIND))
    {
      differences |= COGL_FRAMEBUFFER_STATE_BIND;
      cogl_context_set_current_read_buffer (context, read_buffer);
    }

  if (!differences)
    return;

  /* BIND: no FBO bind in Vulkan. */
  if (differences & COGL_FRAMEBUFFER_STATE_BIND)
    differences &= ~COGL_FRAMEBUFFER_STATE_BIND;

  if (differences & COGL_FRAMEBUFFER_STATE_VIEWPORT)
    {
      float vx, vy, vw, vh;
      CoglPipeline *pipeline;
      CoglDepthState depth_state;

      cogl_framebuffer_get_viewport4f (draw_buffer, &vx, &vy, &vw, &vh);
      /* Vulkan viewport origin is top-left (same as Cogl). */
      driver_vk->pending_viewport.x = vx;
      driver_vk->pending_viewport.y = vy;
      driver_vk->pending_viewport.width = vw;
      driver_vk->pending_viewport.height = vh;
      driver_vk->pending_viewport.minDepth = 0.f;
      driver_vk->pending_viewport.maxDepth = 1.f;

      pipeline = cogl_context_get_current_pipeline (context);
      if (pipeline)
        {
          cogl_pipeline_get_depth_state (pipeline, &depth_state);
          driver_vk->pending_viewport.minDepth = depth_state.range_near;
          driver_vk->pending_viewport.maxDepth = depth_state.range_far;
        }
      driver_vk->pending_viewport_valid = TRUE;

      if (driver_vk->current_pass)
        {
          driver_vk->current_pass->viewport = driver_vk->pending_viewport;
          driver_vk->current_pass->viewport_valid = TRUE;
          if (driver_vk->current_pass->in_render_pass)
            {
              vkCmdSetViewport (driver_vk->current_pass->command_buffer, 0, 1,
                                &driver_vk->pending_viewport);
            }
        }
    }

  if (differences & COGL_FRAMEBUFFER_STATE_CLIP)
    {
      _cogl_clip_stack_flush (_cogl_framebuffer_get_clip_stack (draw_buffer),
                              draw_buffer);
    }

  if (differences & COGL_FRAMEBUFFER_STATE_FRONT_FACE_WINDING)
    {
      CoglPipeline *pipeline = cogl_context_get_current_pipeline (context);
      gboolean invert = cogl_framebuffer_is_y_flipped (draw_buffer);
      CoglWinding winding = COGL_WINDING_COUNTER_CLOCKWISE;

      if (pipeline)
        winding = cogl_pipeline_get_front_face_winding (pipeline);

      if (winding == COGL_WINDING_CLOCKWISE)
        driver_vk->pending_front_face = invert
          ? VK_FRONT_FACE_CLOCKWISE
          : VK_FRONT_FACE_COUNTER_CLOCKWISE;
      else
        driver_vk->pending_front_face = invert
          ? VK_FRONT_FACE_COUNTER_CLOCKWISE
          : VK_FRONT_FACE_CLOCKWISE;

      if (pipeline)
        {
          CoglPipelineCullFaceMode mode =
            cogl_pipeline_get_cull_face_mode (pipeline);
          if (mode != COGL_PIPELINE_CULL_FACE_MODE_NONE &&
              mode != COGL_PIPELINE_CULL_FACE_MODE_BOTH)
            {
              cogl_context_add_current_pipeline_changes_since_flush
                (context, COGL_PIPELINE_STATE_CULL_FACE);
              cogl_context_decrement_current_pipeline_age (context);
            }
        }
    }

  if (differences & COGL_FRAMEBUFFER_STATE_DEPTH_WRITE)
    {
      driver_vk->pending_depth_write =
        cogl_framebuffer_get_depth_write_enabled (draw_buffer);
    }

  if (differences & COGL_FRAMEBUFFER_STATE_MODELVIEW)
    {
      CoglMatrixEntry *entry =
        _cogl_framebuffer_get_modelview_entry (draw_buffer);
      _cogl_context_set_current_modelview_entry (context, entry);
    }

  if (differences & COGL_FRAMEBUFFER_STATE_PROJECTION)
    {
      CoglMatrixEntry *entry =
        _cogl_framebuffer_get_projection_entry (draw_buffer);
      _cogl_context_set_current_projection_entry (context, entry);
    }

  if (differences & COGL_FRAMEBUFFER_STATE_DITHER)
    {
      /* Vulkan has no global dither enable; acknowledged as no-op. */
      (void) cogl_framebuffer_get_dither_enabled (draw_buffer);
    }

  cogl_context_add_current_draw_buffer_state_flushed (context, state);
  cogl_context_clear_current_draw_buffer_changes (context, state);
}

static void
cogl_driver_vk_flush_attributes_state (CoglDriver          *driver,
                                       CoglFramebuffer     *framebuffer,
                                       CoglPipeline        *pipeline,
                                       CoglFlushLayerState *layer_state,
                                       CoglDrawFlags        flags,
                                       CoglAttribute      **attributes,
                                       int                  n_attributes)
{
  CoglDriverVk *driver_vk = COGL_DRIVER_VK (driver);
  gboolean unknown_color_alpha = FALSE;

  (void) framebuffer;

  /* Drop any leftover owned pipeline from a prior flush that draw
   * never consumed (should be rare; avoid leaking across draws).
   */
  if (driver_vk->pending_pipeline_owned)
    g_clear_object (&driver_vk->pending_pipeline);
  else
    driver_vk->pending_pipeline = NULL;
  driver_vk->pending_pipeline_owned = FALSE;

  /* Match GL _cogl_gl_flush_attributes_state: honor layer overrides
   * (esp. COGL_PIPELINE_FLUSH_FALLBACK_MASK) via a pipeline copy so
   * draw uses the effective pipeline.
   */
  if (layer_state && G_UNLIKELY (layer_state->options.flags))
    {
      CoglPipeline *copy = cogl_pipeline_copy (pipeline);

      _cogl_pipeline_apply_overrides (copy, &layer_state->options);
      driver_vk->pending_pipeline = copy;
      driver_vk->pending_pipeline_owned = TRUE;
    }

  for (int i = 0; i < n_attributes; i++)
    if (attributes[i]->name_state->name_id == COGL_ATTRIBUTE_NAME_ID_COLOR_ARRAY &&
        !(flags & COGL_DRAW_COLOR_ATTRIBUTE_IS_OPAQUE) &&
        _cogl_attribute_get_n_components (attributes[i]) == 4)
      unknown_color_alpha = TRUE;

  _cogl_pipeline_update_real_blend_enable (driver_vk->pending_pipeline
                                           ? driver_vk->pending_pipeline
                                           : pipeline,
                                           unknown_color_alpha);

  /* The journal flushes attribute state before drawing. Retain the borrowed
   * attributes so the subsequent draw can read their buffer geometry.
   */
  driver_vk->pending_attributes = attributes;
  driver_vk->pending_n_attributes = n_attributes;
}

static VkSamplerAddressMode
map_wrap_mode (CoglSamplerCacheWrapMode wrap)
{
  switch ((int) wrap)
    {
    case COGL_SAMPLER_CACHE_WRAP_MODE_AUTOMATIC:
    case COGL_SAMPLER_CACHE_WRAP_MODE_CLAMP_TO_EDGE:
      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case COGL_SAMPLER_CACHE_WRAP_MODE_CLAMP_TO_BORDER:
      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case GL_REPEAT:
      return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case GL_MIRRORED_REPEAT:
      return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default:
      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

static void
map_filters (GLenum               min_filter,
             GLenum               mag_filter,
             VkFilter            *out_min,
             VkFilter            *out_mag,
             VkSamplerMipmapMode *out_mip,
             float               *out_max_lod,
             float               *out_lod_bias)
{
  *out_mag = (mag_filter == GL_NEAREST) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  /* Match GL_EXT_texture_lod_bias / GL3: NEAREST mip filters bias -0.5. */
  *out_lod_bias = (min_filter == GL_NEAREST_MIPMAP_NEAREST ||
                   min_filter == GL_LINEAR_MIPMAP_NEAREST) ? -0.5f : 0.0f;

  switch (min_filter)
    {
    case GL_NEAREST:
      *out_min = VK_FILTER_NEAREST;
      *out_mip = VK_SAMPLER_MIPMAP_MODE_NEAREST;
      *out_max_lod = 0.f;
      break;
    case GL_LINEAR:
      *out_min = VK_FILTER_LINEAR;
      *out_mip = VK_SAMPLER_MIPMAP_MODE_NEAREST;
      *out_max_lod = 0.f;
      break;
    case GL_NEAREST_MIPMAP_NEAREST:
      *out_min = VK_FILTER_NEAREST;
      *out_mip = VK_SAMPLER_MIPMAP_MODE_NEAREST;
      *out_max_lod = 1000.f;
      break;
    case GL_LINEAR_MIPMAP_NEAREST:
      *out_min = VK_FILTER_LINEAR;
      *out_mip = VK_SAMPLER_MIPMAP_MODE_NEAREST;
      *out_max_lod = 1000.f;
      break;
    case GL_NEAREST_MIPMAP_LINEAR:
      *out_min = VK_FILTER_NEAREST;
      *out_mip = VK_SAMPLER_MIPMAP_MODE_LINEAR;
      *out_max_lod = 1000.f;
      break;
    case GL_LINEAR_MIPMAP_LINEAR:
      *out_min = VK_FILTER_LINEAR;
      *out_mip = VK_SAMPLER_MIPMAP_MODE_LINEAR;
      *out_max_lod = 1000.f;
      break;
    default:
      *out_min = VK_FILTER_LINEAR;
      *out_mip = VK_SAMPLER_MIPMAP_MODE_NEAREST;
      *out_max_lod = 1000.f;
      break;
    }
}

static void
cogl_driver_vk_sampler_init (CoglDriver            *driver,
                             CoglSamplerCacheEntry *entry)
{
  CoglDriverVk *driver_vk = COGL_DRIVER_VK (driver);
  VkSamplerCreateInfo info = {
    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .maxAnisotropy = 1.f,
    .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
  };
  VkSampler sampler = VK_NULL_HANDLE;
  VkResult res;

  if (!driver_vk->vk_device || !entry)
    return;

  if (!driver_vk->samplers)
    driver_vk->samplers = g_hash_table_new (g_direct_hash, g_direct_equal);

  map_filters (entry->min_filter, entry->mag_filter,
               &info.minFilter, &info.magFilter,
               &info.mipmapMode, &info.maxLod, &info.mipLodBias);
  info.minLod = 0.f;
  info.addressModeU = map_wrap_mode (entry->wrap_mode_s);
  info.addressModeV = map_wrap_mode (entry->wrap_mode_t);

  res = vkCreateSampler (driver_vk->vk_device->device, &info, NULL, &sampler);
  if (res != VK_SUCCESS)
    {
      g_warning ("Cogl Vulkan: vkCreateSampler failed (%d)", (int) res);
      return;
    }

  cogl_vk_debug_log (
    "tex",
    "sampler_init entry=%p wrap_s=%d wrap_t=%d → addrMode U=%d V=%d "
    "min=%d mag=%d samp=%p",
    (void *) entry,
    (int) entry->wrap_mode_s, (int) entry->wrap_mode_t,
    (int) info.addressModeU, (int) info.addressModeV,
    (int) info.minFilter, (int) info.magFilter,
    (void *) (uintptr_t) sampler);

  g_hash_table_insert (driver_vk->samplers, entry,
                       (gpointer) (gsize) sampler);
}

static void
cogl_driver_vk_sampler_free (CoglDriver            *driver,
                             CoglSamplerCacheEntry *entry)
{
  CoglDriverVk *driver_vk = COGL_DRIVER_VK (driver);
  gpointer samp;

  if (!driver_vk->samplers || !entry)
    return;

  samp = g_hash_table_lookup (driver_vk->samplers, entry);
  if (!samp)
    return;

  if (driver_vk->vk_device)
    vkDestroySampler (driver_vk->vk_device->device,
                      (VkSampler) (gsize) samp, NULL);
  g_hash_table_remove (driver_vk->samplers, entry);
}

static void
pending_uniform_free (gpointer data)
{
  CoglBoxedValue *bv = data;
  _cogl_boxed_value_destroy (bv);
  g_free (bv);
}

static void
cogl_driver_vk_set_uniform (CoglDriver           *driver,
                            GLint                 location,
                            const CoglBoxedValue *value)
{
  CoglDriverVk *driver_vk = COGL_DRIVER_VK (driver);
  CoglBoxedValue *copy;

  /* pack_uniforms at draw is authoritative for UBO layout; keep a
   * location→value table that pack_uniforms merges over overrides.
   */
  if (location < 0 || !value)
    return;

  if (!driver_vk->pending_uniforms)
    {
      driver_vk->pending_uniforms =
        g_hash_table_new_full (g_direct_hash, g_direct_equal,
                               NULL, pending_uniform_free);
    }

  copy = g_new0 (CoglBoxedValue, 1);
  _cogl_boxed_value_copy (copy, value);
  g_hash_table_insert (driver_vk->pending_uniforms,
                       GINT_TO_POINTER (location), copy);
}

static void
cogl_driver_vk_pipeline_pre_change_notify (CoglDriver        *driver,
                                           CoglPipeline      *pipeline,
                                           CoglPipelineState  change,
                                           const CoglColor   *new_color)
{
  /* State changes affect the cache key used by the next draw. Existing
   * Vulkan pipelines remain valid for draws already recorded.
   */
  (void) driver;
  (void) pipeline;
  (void) change;
  (void) new_color;
}

static void
cogl_driver_vk_pipeline_layer_pre_change_notify (CoglDriver             *driver,
                                                 CoglPipeline           *owner,
                                                 CoglPipelineLayer      *layer,
                                                 CoglPipelineLayerState  change)
{
  (void) driver;
  (void) owner;
  (void) layer;
  (void) change;
}

static void
vert_push_ndc_rect (CoglVkVertPush *out,
                    float           x1,
                    float           y1,
                    float           x2,
                    float           y2)
{
  memset (out, 0, sizeof (*out));
  out->mat4[0][0] = x2 - x1;
  out->mat4[1][1] = y2 - y1;
  out->mat4[2][2] = 1.f;
  out->mat4[3][3] = 1.f;
  out->mat4[0][3] = x1;
  out->mat4[1][3] = y1;
  out->uv_off[0] = 0.f;
  out->uv_off[1] = 0.f;
  out->uv_size[0] = 1.f;
  out->uv_size[1] = 1.f;
}

static void
vert_push_mvp_rect (CoglVkVertPush          *out,
                    const graphene_matrix_t *mvp,
                    float                    x1,
                    float                    y1,
                    float                    x2,
                    float                    y2)
{
  graphene_matrix_t local, combined;

  graphene_matrix_init_translate (&local,
                                  &GRAPHENE_POINT3D_INIT (x1, y1, 0.f));
  graphene_matrix_scale (&local, x2 - x1, y2 - y1, 1.f);
  graphene_matrix_multiply (mvp, &local, &combined);
  cogl_vk_vert_push_set_matrix (out, &combined);
}

static gboolean
vk_clear_stencil_attachment (CoglDriverVk            *driver_vk,
                             CoglFramebufferVk       *fb_vk,
                             CoglVkRenderFormatSetup *setup)
{
  CoglVkPass *pass;
  CoglVkImage *ds;
  VkClearDepthStencilValue value = { .depth = 1.f, .stencil = 0 };
  VkImageSubresourceRange range = {
    .aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
    .levelCount = 1,
    .layerCount = 1,
  };

  (void) setup;
  ds = cogl_framebuffer_vk_peek_depth_stencil (fb_vk);
  if (!ds)
    return FALSE;

  pass = driver_vk->current_pass;
  if (!pass)
    return FALSE;

  cogl_vk_pass_end_render (pass);
  cogl_vk_image_cmd_barrier (ds,
                             pass->command_buffer,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                             VK_ACCESS_TRANSFER_WRITE_BIT);
  vkCmdClearDepthStencilImage (pass->command_buffer, ds->image, ds->layout,
                               &value, 1, &range);
  return TRUE;
}

static void
add_stencil_clip_rectangle (CoglDriverVk      *driver_vk,
                            CoglFramebuffer   *framebuffer,
                            CoglFramebufferVk *fb_vk,
                            CoglMatrixEntry   *modelview_entry,
                            float              x_1,
                            float              y_1,
                            float              x_2,
                            float              y_2,
                            gboolean           merge)
{
  CoglMatrixStack *projection_stack =
    _cogl_framebuffer_get_projection_stack (framebuffer);
  graphene_matrix_t modelview, projection, mvp;
  CoglVkVertPush vert;
  CoglVkPass *pass;
  CoglVkPipelineCache *cache;
  CoglVkRenderFormatSetup *setup;
  CoglVkImage *color_target;
  CoglVkImage *ds_img;
  VkSampleCountFlagBits samples;
  g_autoptr (GError) error = NULL;

  color_target = cogl_framebuffer_vk_prepare_draw_targets (fb_vk);
  ds_img = cogl_framebuffer_vk_peek_depth_stencil (fb_vk);
  /* Stencil clip needs matching-sample DS; skip if DS disabled / mismatch. */
  if (!color_target || !ds_img || !driver_vk->pipeline_cache)
    return;

  samples = color_target->samples > VK_SAMPLE_COUNT_1_BIT
    ? color_target->samples
    : VK_SAMPLE_COUNT_1_BIT;
  if (ds_img->samples != samples)
    return;

  {
    int stencil_bits = 0;

    cogl_vk_format_depth_stencil_bits (ds_img->format, NULL, &stencil_bits);
    if (stencil_bits <= 0)
      return; /* depth-only RB — stencil clip needs stencil aspect */
  }

  cache = driver_vk->pipeline_cache;
  setup = cogl_vk_pipeline_cache_get_format_setup (
    cache, color_target->format, ds_img->format, samples, &error);
  if (!setup)
    {
      g_warning ("Cogl Vulkan: stencil clip setup failed: %s",
                 error ? error->message : "unknown");
      return;
    }

  if (!driver_vk->current_pass)
    {
      driver_vk->current_pass = cogl_vk_pass_begin (driver_vk->vk_device, &error);
      if (!driver_vk->current_pass)
        return;
    }
  pass = driver_vk->current_pass;
  pass->stencil_clip_active = TRUE;
  if (driver_vk->pending_scissor_valid)
    {
      pass->scissor = driver_vk->pending_scissor;
      pass->scissor_valid = TRUE;
    }

  cogl_matrix_entry_get (modelview_entry, &modelview);
  cogl_matrix_entry_get (projection_stack->last_entry, &projection);
  /* Stencil write shaders have no flip_vector; bake like GL no-snippet. */
  cogl_vk_maybe_bake_y_flip_projection (framebuffer, NULL, &projection);
  /* Match GL / VK hot path: modelview × projection. */
  graphene_matrix_multiply (&modelview, &projection, &mvp);
  vert_push_mvp_rect (&vert, &mvp, x_1, y_1, x_2, y_2);

  if (merge)
    {
      if (!cogl_vk_cmd_draw_stencil_quad (pass, cache, setup, color_target,
                                          &vert, COGL_VK_STENCIL_CLIP_OP_INCR,
                                          &error))
        g_warning ("Cogl Vulkan: stencil INCR failed: %s",
                   error ? error->message : "unknown");
      else
        {
          CoglVkVertPush full;
          vert_push_ndc_rect (&full, -1.f, -1.f, 1.f, 1.f);
          g_clear_error (&error);
          if (!cogl_vk_cmd_draw_stencil_quad (pass, cache, setup, color_target,
                                              &full, COGL_VK_STENCIL_CLIP_OP_DECR,
                                              &error))
            g_warning ("Cogl Vulkan: stencil DECR failed: %s",
                       error ? error->message : "unknown");
        }
    }
  else
    {
      vk_clear_stencil_attachment (driver_vk, fb_vk, setup);
      if (!cogl_vk_cmd_draw_stencil_quad (pass, cache, setup, color_target,
                                          &vert, COGL_VK_STENCIL_CLIP_OP_REPLACE,
                                          &error))
        g_warning ("Cogl Vulkan: stencil REPLACE failed: %s",
                   error ? error->message : "unknown");
    }

  driver_vk->pending_stencil_clip = TRUE;
  pass->stencil_clip_active = TRUE;
}

static void
add_stencil_clip_region (CoglDriverVk      *driver_vk,
                         CoglFramebuffer   *framebuffer,
                         CoglFramebufferVk *fb_vk,
                         MtkRegion         *region,
                         gboolean           merge)
{
  graphene_matrix_t matrix;
  graphene_point3d_t p;
  CoglVkPass *pass;
  CoglVkPipelineCache *cache;
  CoglVkRenderFormatSetup *setup;
  CoglVkImage *color_target;
  CoglVkImage *ds_img;
  VkSampleCountFlagBits samples;
  g_autoptr (GError) error = NULL;
  int num_rectangles;
  int i;

  color_target = cogl_framebuffer_vk_prepare_draw_targets (fb_vk);
  ds_img = cogl_framebuffer_vk_peek_depth_stencil (fb_vk);
  if (!color_target || !ds_img || !driver_vk->pipeline_cache)
    return;

  samples = color_target->samples > VK_SAMPLE_COUNT_1_BIT
    ? color_target->samples
    : VK_SAMPLE_COUNT_1_BIT;
  if (ds_img->samples != samples)
    return;

  {
    int stencil_bits = 0;

    cogl_vk_format_depth_stencil_bits (ds_img->format, NULL, &stencil_bits);
    if (stencil_bits <= 0)
      return;
  }

  cache = driver_vk->pipeline_cache;
  setup = cogl_vk_pipeline_cache_get_format_setup (
    cache, color_target->format, ds_img->format, samples, &error);
  if (!setup)
    return;

  if (!driver_vk->current_pass)
    {
      driver_vk->current_pass = cogl_vk_pass_begin (driver_vk->vk_device, &error);
      if (!driver_vk->current_pass)
        return;
    }
  pass = driver_vk->current_pass;
  pass->stencil_clip_active = TRUE;
  if (driver_vk->pending_scissor_valid)
    {
      pass->scissor = driver_vk->pending_scissor;
      pass->scissor_valid = TRUE;
    }

  graphene_point3d_init (&p,
                         -cogl_framebuffer_get_viewport_x (framebuffer),
                         -cogl_framebuffer_get_viewport_y (framebuffer),
                         0);
  graphene_matrix_init_translate (&matrix, &p);
  graphene_matrix_scale (&matrix,
                         2.0f / cogl_framebuffer_get_viewport_width (framebuffer),
                         -2.0f / cogl_framebuffer_get_viewport_height (framebuffer),
                         1.0f);
  graphene_matrix_translate (&matrix, &GRAPHENE_POINT3D_INIT (-1.f, 1.f, 0.f));

  if (!merge)
    vk_clear_stencil_attachment (driver_vk, fb_vk, setup);

  num_rectangles = mtk_region_num_rectangles (region);
  for (i = 0; i < num_rectangles; i++)
    {
      MtkRectangle rect = mtk_region_get_rectangle (region, i);
      float x1 = rect.x, y1 = rect.y, z1 = 0.f, w1 = 1.f;
      float x2 = rect.x + rect.width, y2 = rect.y + rect.height;
      float z2 = 0.f, w2 = 1.f;
      CoglVkVertPush vert;

      cogl_graphene_matrix_project_point (&matrix, &x1, &y1, &z1, &w1);
      cogl_graphene_matrix_project_point (&matrix, &x2, &y2, &z2, &w2);
      vert_push_ndc_rect (&vert, x1, y1, x2, y2);

      g_clear_error (&error);
      if (!cogl_vk_cmd_draw_stencil_quad (
            pass, cache, setup, color_target, &vert,
            merge ? COGL_VK_STENCIL_CLIP_OP_INCR
                  : COGL_VK_STENCIL_CLIP_OP_REPLACE,
            &error))
        {
          g_warning ("Cogl Vulkan: stencil region draw failed: %s",
                     error ? error->message : "unknown");
        }
    }

  if (merge)
    {
      CoglVkVertPush full;
      vert_push_ndc_rect (&full, -1.f, -1.f, 1.f, 1.f);
      g_clear_error (&error);
      if (!cogl_vk_cmd_draw_stencil_quad (pass, cache, setup, color_target,
                                          &full, COGL_VK_STENCIL_CLIP_OP_DECR,
                                          &error))
        g_warning ("Cogl Vulkan: stencil region DECR failed: %s",
                   error ? error->message : "unknown");
    }

  driver_vk->pending_stencil_clip = TRUE;
  pass->stencil_clip_active = TRUE;
}

static void
cogl_driver_vk_clip_stack_flush (CoglDriver      *driver,
                                 CoglClipStack   *stack,
                                 CoglFramebuffer *framebuffer)
{
  CoglDriverVk *driver_vk = COGL_DRIVER_VK (driver);
  CoglContext *ctx = cogl_framebuffer_get_context (framebuffer);
  CoglFramebufferDriver *fb_driver;
  CoglFramebufferVk *fb_vk = NULL;
  gboolean using_stencil_buffer = FALSE;
  CoglClipStack *entry;
  int scissor_x0, scissor_y0, scissor_x1, scissor_y1;
  int width, height;

  /* Mirror GL clip flush bookkeeping so journal skip-logic works. */
  if (cogl_context_get_current_clip_stack_valid (ctx))
    {
      if (cogl_context_get_current_clip_stack (ctx) == stack)
        return;

      _cogl_clip_stack_unref (cogl_context_get_current_clip_stack (ctx));
    }

  cogl_context_set_current_clip_stack_valid (ctx, TRUE);
  cogl_context_set_current_clip_stack (ctx, _cogl_clip_stack_ref (stack));

  driver_vk->pending_stencil_clip = FALSE;
  if (driver_vk->current_pass)
    driver_vk->current_pass->stencil_clip_active = FALSE;

  fb_driver = cogl_framebuffer_get_driver (framebuffer);
  if (COGL_IS_FRAMEBUFFER_DRIVER_VK (fb_driver))
    fb_vk = COGL_FRAMEBUFFER_DRIVER_VK (fb_driver);

  if (stack == NULL)
    {
      driver_vk->pending_scissor_valid = FALSE;
      if (driver_vk->current_pass)
        driver_vk->current_pass->scissor_valid = FALSE;
      return;
    }

  _cogl_clip_stack_get_bounds (stack,
                               &scissor_x0, &scissor_y0,
                               &scissor_x1, &scissor_y1);

  width = cogl_framebuffer_get_width (framebuffer);
  height = cogl_framebuffer_get_height (framebuffer);

  if (scissor_x0 >= scissor_x1 || scissor_y0 >= scissor_y1)
    {
      driver_vk->pending_scissor.offset.x = 0;
      driver_vk->pending_scissor.offset.y = 0;
      driver_vk->pending_scissor.extent.width = 0;
      driver_vk->pending_scissor.extent.height = 0;
    }
  else
    {
      /* A flipped Cogl projection places its top at Vulkan NDC -1,
       * which is the top of a positive-height Vulkan viewport. The
       * unflipped projection needs the same inversion as read_pixels.
       */
      int x = CLAMP (scissor_x0, 0, width);
      int y = CLAMP (scissor_y0, 0, height);
      int x2 = CLAMP (scissor_x1, 0, width);
      int y2 = CLAMP (scissor_y1, 0, height);
      gboolean y_flipped = cogl_framebuffer_is_y_flipped (framebuffer);

      driver_vk->pending_scissor.offset.x = x;
      driver_vk->pending_scissor.offset.y = y_flipped ? y : (height - y2);
      driver_vk->pending_scissor.extent.width = (uint32_t) MAX (0, x2 - x);
      driver_vk->pending_scissor.extent.height = (uint32_t) MAX (0, y2 - y);
    }

  driver_vk->pending_scissor_valid = TRUE;
  driver_vk->pending_scissor_fb_height = (uint32_t) height;
  driver_vk->pending_scissor_y_flipped = cogl_framebuffer_is_y_flipped (framebuffer);

  if (driver_vk->current_pass)
    {
      driver_vk->current_pass->scissor = driver_vk->pending_scissor;
      driver_vk->current_pass->scissor_valid = TRUE;
      if (driver_vk->current_pass->in_render_pass)
        {
          vkCmdSetScissor (driver_vk->current_pass->command_buffer, 0, 1,
                           &driver_vk->pending_scissor);
        }
    }

  if (!fb_vk)
    return;

  /* Ensure color is resolved so stencil clip can bind the offscreen. */
  if (!cogl_framebuffer_vk_peek_color_image (fb_vk))
    cogl_framebuffer_vk_ensure_depth_stencil (fb_vk);

  for (entry = stack; entry; entry = entry->parent)
    {
      switch (entry->type)
        {
        case COGL_CLIP_STACK_RECT:
          {
            CoglClipStackRect *rect = (CoglClipStackRect *) entry;

            if (!rect->can_be_scissor ||
                G_UNLIKELY (COGL_DEBUG_ENABLED (COGL_DEBUG_STENCILLING)))
              {
                add_stencil_clip_rectangle (driver_vk, framebuffer, fb_vk,
                                            rect->matrix_entry,
                                            rect->x0, rect->y0,
                                            rect->x1, rect->y1,
                                            using_stencil_buffer);
                using_stencil_buffer = TRUE;
              }
            break;
          }
        case COGL_CLIP_STACK_REGION:
          {
            CoglClipStackRegion *region = (CoglClipStackRegion *) entry;

            if (mtk_region_num_rectangles (region->region) > 1 ||
                G_UNLIKELY (COGL_DEBUG_ENABLED (COGL_DEBUG_STENCILLING)))
              {
                add_stencil_clip_region (driver_vk, framebuffer, fb_vk,
                                         region->region,
                                         using_stencil_buffer);
                using_stencil_buffer = TRUE;
              }
            break;
          }
        }
    }
}


static CoglBufferImpl *
cogl_driver_vk_create_buffer_impl (CoglDriver *driver)
{
  (void) driver;
  return g_object_new (COGL_TYPE_BUFFER_IMPL_VK, NULL);
}

static CoglTextureDriver *
cogl_driver_vk_create_texture_driver (CoglDriver *driver)
{
  return g_object_new (COGL_TYPE_TEXTURE_DRIVER_VK,
                       "driver", driver,
                       NULL);
}

static void
cogl_driver_vk_dispose (GObject *object)
{
  CoglDriverVk *driver_vk = COGL_DRIVER_VK (object);

  g_clear_pointer (&driver_vk->current_pass, cogl_vk_pass_free);
  if (driver_vk->vk_device)
    vkDeviceWaitIdle (driver_vk->vk_device->device);
  if (driver_vk->latest_sync_fd >= 0)
    {
      close (driver_vk->latest_sync_fd);
      driver_vk->latest_sync_fd = -1;
    }

  if (driver_vk->samplers)
    {
      GHashTableIter iter;
      gpointer key, value;

      g_hash_table_iter_init (&iter, driver_vk->samplers);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          if (driver_vk->vk_device && value)
            vkDestroySampler (driver_vk->vk_device->device,
                              (VkSampler) (gsize) value, NULL);
        }
      g_clear_pointer (&driver_vk->samplers, g_hash_table_unref);
    }

  g_clear_pointer (&driver_vk->pending_uniforms, g_hash_table_unref);
  if (driver_vk->pending_pipeline_owned)
    g_clear_object (&driver_vk->pending_pipeline);
  else
    driver_vk->pending_pipeline = NULL;
  driver_vk->pending_pipeline_owned = FALSE;

  if (driver_vk->vk_device)
    driver_vk->vk_device->owner = NULL;
  g_clear_pointer (&driver_vk->pipeline_cache, cogl_vk_pipeline_cache_free);
  g_clear_pointer (&driver_vk->dmabuf_memo, cogl_vk_dmabuf_memo_free);
  g_clear_pointer (&driver_vk->vk_device, cogl_vk_device_free);

  G_OBJECT_CLASS (cogl_driver_vk_parent_class)->dispose (object);
}

static gboolean cogl_driver_vk_consume_frame_error (CoglDriver *driver);

static void
cogl_driver_vk_class_init (CoglDriverVkClass *klass)
{
  GObjectClass *object_klass = G_OBJECT_CLASS (klass);
  CoglDriverClass *driver_klass = COGL_DRIVER_CLASS (klass);

  object_klass->dispose = cogl_driver_vk_dispose;

  driver_klass->consume_frame_error = cogl_driver_vk_consume_frame_error;
  driver_klass->get_vendor = cogl_driver_vk_get_vendor;
  driver_klass->get_graphics_reset_status =
    cogl_driver_vk_get_graphics_reset_status;
  driver_klass->update_features = cogl_driver_vk_update_features;
  driver_klass->format_supports_upload = cogl_driver_vk_format_supports_upload;
  driver_klass->create_framebuffer_driver = cogl_driver_vk_create_framebuffer_driver;
  driver_klass->flush_framebuffer_state = cogl_driver_vk_flush_framebuffer_state;
  driver_klass->flush_attributes_state = cogl_driver_vk_flush_attributes_state;
  driver_klass->clip_stack_flush = cogl_driver_vk_clip_stack_flush;
  driver_klass->create_buffer_impl = cogl_driver_vk_create_buffer_impl;
  driver_klass->create_texture_driver = cogl_driver_vk_create_texture_driver;
  driver_klass->sampler_init = cogl_driver_vk_sampler_init;
  driver_klass->sampler_free = cogl_driver_vk_sampler_free;
  driver_klass->set_uniform = cogl_driver_vk_set_uniform;
  driver_klass->pipeline_pre_change_notify = cogl_driver_vk_pipeline_pre_change_notify;
  driver_klass->pipeline_layer_pre_change_notify =
    cogl_driver_vk_pipeline_layer_pre_change_notify;
}

static void
cogl_driver_vk_init (CoglDriverVk *driver)
{
  driver->vk_device = NULL;
  driver->current_pass = NULL;
  driver->latest_sync_fd = -1;
  driver->pending_drm_fd = -1;
  driver->pending_front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  driver->pending_depth_write = TRUE;
  driver->pending_stencil_clip = FALSE;
}

static gboolean
cogl_driver_vk_consume_frame_error (CoglDriver *driver)
{
  CoglDriverVk *driver_vk = COGL_DRIVER_VK (driver);
  gboolean failed = driver_vk->submission_failed;
  driver_vk->submission_failed = FALSE;
  return failed || (driver_vk->vk_device && driver_vk->vk_device->device_lost);
}
