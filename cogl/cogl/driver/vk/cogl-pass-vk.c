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

#include "cogl/driver/vk/cogl-pass-vk-private.h"
#include "cogl/driver/vk/cogl-pipeline-vk-private.h"
#include "cogl/driver/vk/cogl-driver-vk-private.h"
#include "cogl/driver/vk/cogl-fence-vk-private.h"
#include "cogl/driver/vk/cogl-vk-debug-private.h"

#include <gio/gio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

typedef struct
{
  CoglVkImage *image;
  VkImageLayout layout;
  gboolean graphics_owned;
  gboolean shader_read_synced;
} ImageState;

void
cogl_vk_pass_note_image_state (CoglVkPass  *pass,
                               CoglVkImage *image)
{
  ImageState state;

  if (!pass->image_states)
    pass->image_states = g_array_new (FALSE, FALSE, sizeof (ImageState));
  for (unsigned int i = 0; i < pass->image_states->len; i++)
    if (g_array_index (pass->image_states, ImageState, i).image == image)
      return;
  state = (ImageState) { image, image->layout, image->dmabuf_graphics_owned,
                         image->shader_read_synced };
  g_array_append_val (pass->image_states, state);
}

static void
restore_image_states (CoglVkPass *pass)
{
  if (!pass->image_states)
    return;
  for (unsigned int i = 0; i < pass->image_states->len; i++)
    {
      ImageState *state = &g_array_index (pass->image_states, ImageState, i);
      state->image->layout = state->layout;
      state->image->dmabuf_graphics_owned = state->graphics_owned;
      state->image->shader_read_synced = state->shader_read_synced;
    }
}

CoglVkPass *
cogl_vk_pass_begin (CoglVkDevice *device,
                    GError      **error)
{
  CoglVkPass *pass;
  VkCommandBufferAllocateInfo alloc_info;
  VkCommandBufferBeginInfo begin_info = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  VkResult res;

  g_return_val_if_fail (device != NULL, NULL);
  g_return_val_if_fail (device->command_pool != VK_NULL_HANDLE, NULL);

  if (device->device_lost || (device->owner && device->owner->submission_failed))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Vulkan frame is aborted");
      return NULL;
    }

  pass = g_new0 (CoglVkPass, 1);
  pass->device = device;
  pass->wait_semaphores = g_array_new (FALSE, FALSE, sizeof (VkSemaphore));
  pass->foreign_images = g_ptr_array_new ();
  pass->timestamp_begin = -1;
  pass->timestamp_end = -1;

  memset (&alloc_info, 0, sizeof (alloc_info));
  alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc_info.commandPool = device->command_pool;
  alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc_info.commandBufferCount = 1;

  res = vkAllocateCommandBuffers (device->device, &alloc_info,
                                  &pass->command_buffer);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkAllocateCommandBuffers failed (%d)", (int) res);
      g_array_free (pass->wait_semaphores, TRUE);
      g_ptr_array_unref (pass->foreign_images);
      g_free (pass);
      return NULL;
    }

  pass->fence = cogl_vk_fence_ticket_acquire (device, error);
  if (!pass->fence)
    {
      vkFreeCommandBuffers (device->device, device->command_pool, 1,
                            &pass->command_buffer);
      g_array_free (pass->wait_semaphores, TRUE);
      g_ptr_array_unref (pass->foreign_images);
      g_free (pass);
      return NULL;
    }

  res = vkBeginCommandBuffer (pass->command_buffer, &begin_info);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkBeginCommandBuffer failed (%d)", (int) res);
      cogl_vk_pass_free (pass);
      return NULL;
    }

  /* TOP_OF_PIPE at cmd-buffer start — ring reset+write; -1 if unsupported. */
  pass->timestamp_begin =
    cogl_vk_device_cmd_write_timestamp (device,
                                        pass->command_buffer,
                                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

  pass->recording = TRUE;
  return pass;
}

gboolean
cogl_vk_pass_has_foreign_image (CoglVkPass  *pass,
                                CoglVkImage *image)
{
  unsigned int i;

  g_return_val_if_fail (pass != NULL, FALSE);
  if (!image || !pass->foreign_images)
    return FALSE;

  for (i = 0; i < pass->foreign_images->len; i++)
    {
      if (g_ptr_array_index (pass->foreign_images, i) == image)
        return TRUE;
    }
  return FALSE;
}

void
cogl_vk_pass_note_foreign_image (CoglVkPass  *pass,
                                 CoglVkImage *image)
{
  g_return_if_fail (pass != NULL);
  if (!image || !image->is_dmabuf)
    return;

  if (cogl_vk_pass_has_foreign_image (pass, image))
    return;
  g_ptr_array_add (pass->foreign_images, image);
}

gboolean
cogl_vk_pass_add_wait_sync_fd (CoglVkPass *pass,
                               int         sync_fd,
                               GError    **error)
{
  VkSemaphoreCreateInfo create_info = {
    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
  };
  VkImportSemaphoreFdInfoKHR import_info = {
    .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
    .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
    .fd = sync_fd,
  };
  VkSemaphore sem;
  VkResult res;

  g_return_val_if_fail (pass != NULL, FALSE);

  /* A successful temporary SYNC_FD import transfers fd ownership to Vulkan. */
  if (!pass->device->has_external_semaphore_fd)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "external semaphore fd unavailable");
      return FALSE;
    }

  res = vkCreateSemaphore (pass->device->device, &create_info, NULL, &sem);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkCreateSemaphore failed (%d)", (int) res);
      return FALSE;
    }

  import_info.semaphore = sem;
  res = pass->device->vkImportSemaphoreFdKHR (pass->device->device, &import_info);
  if (res != VK_SUCCESS)
    {
      vkDestroySemaphore (pass->device->device, sem, NULL);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkImportSemaphoreFdKHR failed (%d)", (int) res);
      return FALSE;
    }

  g_array_append_val (pass->wait_semaphores, sem);
  return TRUE;
}

gboolean
cogl_vk_pass_submit (CoglVkPass *pass,
                     GError    **error)
{
  COGL_VK_PROFILE_SCOPE ("cogl_vk_pass_submit");
  VkSubmitInfo submit_info;
  VkSubmitInfo submits[2];
  VkTimelineSemaphoreSubmitInfo timeline_info;
  uint32_t n_submits = 1;
  g_autoptr (GArray) wait_stages = NULL;
  unsigned int i;
  VkResult res;
  uint64_t next_point;

  g_return_val_if_fail (pass != NULL, FALSE);
  g_return_val_if_fail (!pass->submitted, FALSE);

  {
    CoglVkImage *submit_color = pass->render_color;
    int submit_layout = submit_color ? (int) submit_color->layout : -1;
    gboolean was_in_rp = pass->in_render_pass;

    cogl_vk_debug_log (
      "draw",
      "pass_submit PRE end_render pass=%p in_rp=%d color=%p layout=%d",
      (void *) pass, was_in_rp, (void *) submit_color, submit_layout);

    cogl_vk_pass_end_render (pass);

    cogl_vk_debug_log (
      "draw",
      "pass_submit POST end_render pass=%p was_color=%p was_layout=%d",
      (void *) pass, (void *) submit_color, submit_layout);
  }

  /* Release imported DMA-BUF images after rendering has finished. */
  for (i = 0; i < pass->foreign_images->len; i++)
    {
      CoglVkImage *img = g_ptr_array_index (pass->foreign_images, i);
      cogl_vk_image_cmd_foreign_release (img,
                                         pass->command_buffer,
                                         pass->device->graphics_queue_family,
                                         VK_IMAGE_LAYOUT_GENERAL);
    }
  g_ptr_array_set_size (pass->foreign_images, 0);

  if (pass->recording)
    {
      /* Ensure a begin stamp exists (pass_begin normally wrote it). */
      if (pass->timestamp_begin < 0)
        {
          pass->timestamp_begin =
            cogl_vk_device_cmd_write_timestamp (
              pass->device,
              pass->command_buffer,
              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
        }

      /* BOTTOM_OF_PIPE after foreign release, before EndCommandBuffer. */
      pass->timestamp_end =
        cogl_vk_device_cmd_write_timestamp (
          pass->device,
          pass->command_buffer,
          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

      res = vkEndCommandBuffer (pass->command_buffer);
      if (res != VK_SUCCESS)
        {
          cogl_vk_device_note_vk_result (pass->device, res);
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "vkEndCommandBuffer failed (%d)", (int) res);
          goto failed;
        }
      pass->recording = FALSE;
    }

  if (!cogl_vk_fence_ticket_ensure_binary (pass->fence, error))
    goto failed;

  memset (&submit_info, 0, sizeof (submit_info));
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &pass->command_buffer;

  if (pass->wait_semaphores && pass->wait_semaphores->len > 0)
    {
      wait_stages = g_array_sized_new (FALSE, FALSE,
                                       sizeof (VkPipelineStageFlags),
                                       pass->wait_semaphores->len);
      for (i = 0; i < pass->wait_semaphores->len; i++)
        {
          /* These waits also protect FOREIGN ownership acquire operations.
           * Without USE_ALL_STAGES, ownership transfers are not scoped to a
           * shader stage and require an ALL_COMMANDS semaphore wait.
           */
          VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
          g_array_append_val (wait_stages, stage);
        }
      submit_info.waitSemaphoreCount = pass->wait_semaphores->len;
      submit_info.pWaitSemaphores =
        (const VkSemaphore *) pass->wait_semaphores->data;
      submit_info.pWaitDstStageMask =
        (const VkPipelineStageFlags *) wait_stages->data;
    }

  submit_info.signalSemaphoreCount = 1;
  submit_info.pSignalSemaphores = &pass->fence->binary;
  submits[0] = submit_info;

  if (pass->fence->timeline != VK_NULL_HANDLE)
    {
      /* Signals within one batch are unordered. A following signal-only
       * batch makes timeline completion also cover the binary signal, so
       * ticket recycling may safely destroy the binary semaphore.
       */
      next_point = pass->fence->point + 1;
      timeline_info = (VkTimelineSemaphoreSubmitInfo) {
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .signalSemaphoreValueCount = 1,
        .pSignalSemaphoreValues = &next_point,
      };
      submits[1] = (VkSubmitInfo) {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = &timeline_info,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &pass->fence->timeline,
      };
      n_submits = 2;
    }

  /* Submission is asynchronous. Mutter native backend passes the exported
   * SYNC_FD to KMS, while Cogl tracks resource lifetime through the ticket.
   */
  res = vkQueueSubmit (pass->device->graphics_queue, n_submits, submits,
                       VK_NULL_HANDLE);
  if (res != VK_SUCCESS)
    {
      cogl_vk_device_note_vk_result (pass->device, res);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkQueueSubmit failed (%d)", (int) res);
      goto failed;
    }

  pass->submitted = TRUE;
  pass->fence->submit_serial = ++pass->device->submit_serial;
  if (pass->fence->timeline != VK_NULL_HANDLE)
    pass->fence->point = next_point;
  cogl_vk_device_gpu_trace_note_submit (pass->device, pass->timestamp_begin,
                                        pass->timestamp_end,
                                        pass->fence->timeline,
                                        pass->fence->point);

  if (pass->device->owner && pass->fence)
    cogl_vk_pipeline_cache_note_submit (
      cogl_driver_vk_peek_pipeline_cache (pass->device->owner),
      pass->fence->timeline,
      pass->fence->point);

  cogl_vk_debug_log ("fence",
                     "pass_submit pass=%p fence=%p point=%" G_GUINT64_FORMAT
                     " cmd=%p",
                     (void *) pass, (void *) pass->fence,
                     pass->fence ? pass->fence->point : 0,
                     (void *) (uintptr_t) pass->command_buffer);
  cogl_vk_debug_log ("draw",
                     "pass_submit DONE pass=%p point=%" G_GUINT64_FORMAT,
                     (void *) pass,
                     pass->fence ? pass->fence->point : 0);

  /* Transfer wait semaphore ownership to fence ticket until signaled. */
  if (pass->wait_semaphores && pass->wait_semaphores->len > 0 &&
      pass->fence->wait_semaphores)
    {
      for (i = 0; i < pass->wait_semaphores->len; i++)
        {
          VkSemaphore sem =
            g_array_index (pass->wait_semaphores, VkSemaphore, i);
          g_array_append_val (pass->fence->wait_semaphores, sem);
        }
      g_array_set_size (pass->wait_semaphores, 0);
    }

  /* Stash indices for debug peek (non-blocking get via peek_last_timestamps).
   * Do not wait on fences or WAIT_BIT here — results rarely ready
   * immediately.
   */
  if (pass->timestamp_begin >= 0 && pass->timestamp_end >= 0)
    {
      pass->device->last_timestamp_begin = pass->timestamp_begin;
      pass->device->last_timestamp_end = pass->timestamp_end;
    }

  return TRUE;

failed:
  restore_image_states (pass);
  /* No commands from this pass were accepted. A failed timeline signal must
   * never become a retirement point. Drain older work only on this error
   * path, so descriptors retired during the discarded recording are safe.
   */
  res = vkQueueWaitIdle (pass->device->graphics_queue);
  cogl_vk_device_note_vk_result (pass->device, res);
  if (res == VK_SUCCESS && pass->device->owner)
    cogl_vk_pipeline_cache_discard_recording (
      cogl_driver_vk_peek_pipeline_cache (pass->device->owner));
  if (pass->device->owner)
    {
      pass->device->owner->submission_failed = TRUE;
      if (pass->device->owner->latest_sync_fd >= 0)
        close (pass->device->owner->latest_sync_fd);
      pass->device->owner->latest_sync_fd = -1;
    }
  return FALSE;
}

void
cogl_vk_pass_free (CoglVkPass *pass)
{
  unsigned int i;

  if (!pass)
    return;

  cogl_vk_debug_log ("fence",
                     "pass_free pass=%p fence=%p cmd=%p in_rp=%d",
                     (void *) pass, (void *) pass->fence,
                     (void *) (uintptr_t) pass->command_buffer,
                     pass->in_render_pass);

  if (pass->wait_semaphores)
    {
      for (i = 0; i < pass->wait_semaphores->len; i++)
        {
          VkSemaphore sem =
            g_array_index (pass->wait_semaphores, VkSemaphore, i);
          vkDestroySemaphore (pass->device->device, sem, NULL);
        }
      g_array_free (pass->wait_semaphores, TRUE);
    }

  g_clear_pointer (&pass->foreign_images, g_ptr_array_unref);
  if (!pass->submitted)
    restore_image_states (pass);
  g_clear_pointer (&pass->image_states, g_array_unref);

  cogl_vk_debug_log ("draw", "pass_free: before fence release pass=%p",
                     (void *) pass);

  /* Defer freeing the CB to the fence ticket's recycle path so present does
   * not vkQueueWaitIdle: the CB is freed once the timeline point is observed
   * signaled. This needs the timeline to track completion. Without it (opt
   * extension absent), or when the ticket was already flagged signaled by a
   * fallback path, fall back to the safe blocking free below. */
  if (pass->fence && !pass->submitted)
    {
      /* Retirement may cover uses in earlier submissions as well as this
       * recording. Discarding the recording does not complete that work. */
      if (pass->fence->retired.handles || pass->fence->retired.images)
        {
          VkResult res = vkQueueWaitIdle (pass->device->graphics_queue);

          cogl_vk_device_note_vk_result (pass->device, res);
          if (res != VK_SUCCESS && res != VK_ERROR_DEVICE_LOST)
            g_error ("Cogl Vulkan: cannot retire discarded pass (%d)", (int) res);
        }
      pass->fence->signaled_cached = TRUE;
    }

  if (pass->submitted && pass->fence && pass->command_buffer != VK_NULL_HANDLE &&
      pass->fence->timeline != VK_NULL_HANDLE)
    {
      cogl_vk_fence_ticket_defer_command_buffer (pass->fence,
                                                 pass->command_buffer);
      pass->command_buffer = VK_NULL_HANDLE;
    }
  else if (pass->fence && pass->device &&
           pass->device->graphics_queue != VK_NULL_HANDLE &&
           !cogl_vk_fence_ticket_poll_signaled (pass->fence))
    {
      /* No timeline to defer against — must not free an in-flight CB. */
      cogl_vk_debug_log ("fence",
                         "pass_free: WaitIdle before FreeCommandBuffers");
      vkQueueWaitIdle (pass->device->graphics_queue);
      pass->fence->signaled_cached = TRUE;
    }

  /* Return completed tickets to the pool and retain incomplete submissions. */
  cogl_vk_fence_ticket_release (pass->fence);
  pass->fence = NULL;

  if (pass->command_buffer != VK_NULL_HANDLE &&
      pass->device->command_pool != VK_NULL_HANDLE)
    {
      /* CB not deferred (no timeline, or submit never ran); free directly. */
      vkFreeCommandBuffers (pass->device->device,
                            pass->device->command_pool,
                            1, &pass->command_buffer);
    }
  g_free (pass);
}
