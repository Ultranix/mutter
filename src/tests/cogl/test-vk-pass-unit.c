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

/* Exercise production submission construction without an ICD or GPU. */
#include "config.h"
#include "cogl/driver/vk/cogl-pass-vk.c"

static unsigned int expected_waits;
static unsigned int submissions;
static VkResult submit_result;
static VkResult end_result;
static gboolean binary_failure;
static unsigned int cache_notifications;
static unsigned int discarded_recordings;
static gboolean has_timeline;

void
cogl_vk_debug_log (const char *channel,
                   const char *format,
                   ...)
{
}

void
cogl_vk_pass_end_render (CoglVkPass *pass)
{
  pass->in_render_pass = FALSE;
}

void
cogl_vk_image_cmd_foreign_release (CoglVkImage    *image,
                                   VkCommandBuffer command_buffer,
                                   uint32_t        queue_family,
                                   VkImageLayout   layout)
{
}

int
cogl_vk_device_cmd_write_timestamp (CoglVkDevice           *device,
                                    VkCommandBuffer         command_buffer,
                                    VkPipelineStageFlagBits stage)
{
  return -1;
}

void
cogl_vk_device_note_vk_result (CoglVkDevice *device,
                               VkResult      result)
{
}

gboolean
cogl_vk_fence_ticket_ensure_binary (CoglVkFenceTicket *ticket,
                                    GError           **error)
{
  if (binary_failure)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Injected binary semaphore allocation failure");
      return FALSE;
    }
  g_assert_true (ticket->binary != VK_NULL_HANDLE);
  return TRUE;
}

CoglVkPipelineCache *
cogl_driver_vk_peek_pipeline_cache (CoglDriverVk *driver)
{
  return NULL;
}

void
cogl_vk_pipeline_cache_note_submit (CoglVkPipelineCache *cache,
                                    VkSemaphore          timeline,
                                    uint64_t             point)
{
  cache_notifications++;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkEndCommandBuffer (VkCommandBuffer command_buffer)
{
  return end_result;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit (VkQueue             queue,
               uint32_t            count,
               const VkSubmitInfo *info,
               VkFence             fence)
{
  submissions++;
  g_assert_cmpuint (count, ==, has_timeline ? 2 : 1);
  g_assert_cmpuint (info->commandBufferCount, ==, 1);
  g_assert_cmpuint (info->waitSemaphoreCount, ==, expected_waits);
  for (unsigned int i = 0; i < expected_waits; i++)
    {
      g_assert_cmpuint (info->pWaitDstStageMask[i], ==,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
      g_assert_true (info->pWaitSemaphores[i] ==
                     (VkSemaphore) (uintptr_t) (10 + i));
    }
  if (expected_waits == 0)
    {
      g_assert_null (info->pWaitDstStageMask);
      g_assert_null (info->pWaitSemaphores);
    }
  g_assert_null (info->pNext);
  g_assert_cmpuint (info->signalSemaphoreCount, ==, 1);
  g_assert_true (info->pSignalSemaphores[0] == (VkSemaphore) (uintptr_t) 2);
  if (has_timeline)
    {
      const VkSubmitInfo *completion = &info[1];
      const VkTimelineSemaphoreSubmitInfo *timeline = completion->pNext;

      g_assert_cmpuint (completion->commandBufferCount, ==, 0);
      g_assert_cmpuint (completion->waitSemaphoreCount, ==, 0);
      g_assert_cmpuint (completion->signalSemaphoreCount, ==, 1);
      g_assert_true (completion->pSignalSemaphores[0] ==
                     (VkSemaphore) (uintptr_t) 1);
      g_assert_nonnull (timeline);
      g_assert_cmpuint (timeline->signalSemaphoreValueCount, ==, 1);
      g_assert_cmpuint (timeline->pSignalSemaphoreValues[0], ==, 1);
    }
  return submit_result;
}

void
cogl_vk_pipeline_cache_discard_recording (CoglVkPipelineCache *cache)
{
  discarded_recordings++;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkQueueWaitIdle (VkQueue queue)
{
  return VK_SUCCESS;
}

static void
check_submit (gconstpointer data)
{
  CoglDriverVk driver = { .latest_sync_fd = -1 };
  CoglVkDevice device = { .owner = &driver };
  CoglVkFenceTicket ticket = {
    .timeline = (VkSemaphore) (uintptr_t) 1,
    .binary = (VkSemaphore) (uintptr_t) 2,
  };
  CoglVkPass pass = {
    .device = &device,
    .fence = &ticket,
    .recording = TRUE,
    .timestamp_begin = -1,
    .timestamp_end = -1,
  };
  g_autoptr (GError) error = NULL;
  unsigned int scenario = GPOINTER_TO_UINT (data);

  has_timeline = scenario != 3;
  if (!has_timeline)
    ticket.timeline = VK_NULL_HANDLE;
  expected_waits = scenario == 0 ? 0 : 2;
  submit_result = scenario == 2 ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS;
  end_result = scenario == 4 ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS;
  binary_failure = scenario == 5;
  submissions = 0;
  cache_notifications = 0;
  discarded_recordings = 0;
  pass.wait_semaphores = g_array_new (FALSE, FALSE, sizeof (VkSemaphore));
  pass.foreign_images = g_ptr_array_new ();
  ticket.wait_semaphores = g_array_new (FALSE, FALSE, sizeof (VkSemaphore));
  for (unsigned int i = 0; i < expected_waits; i++)
    {
      VkSemaphore semaphore = (VkSemaphore) (uintptr_t) (10 + i);
      g_array_append_val (pass.wait_semaphores, semaphore);
    }

  if (submit_result == VK_SUCCESS && end_result == VK_SUCCESS && !binary_failure)
    {
      g_assert_true (cogl_vk_pass_submit (&pass, &error));
      g_assert_no_error (error);
      g_assert_cmpuint (pass.wait_semaphores->len, ==, 0);
      g_assert_cmpuint (ticket.wait_semaphores->len, ==, expected_waits);
      for (unsigned int i = 0; i < expected_waits; i++)
        g_assert_true (g_array_index (ticket.wait_semaphores, VkSemaphore, i) ==
                       (VkSemaphore) (uintptr_t) (10 + i));
      g_assert_cmpuint (cache_notifications, ==, 1);
      g_assert_true (pass.submitted);
      g_assert_false (driver.submission_failed);
      g_assert_cmpuint (ticket.point, ==, has_timeline ? 1 : 0);
    }
  else
    {
      g_assert_false (cogl_vk_pass_submit (&pass, &error));
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
      g_assert_cmpuint (pass.wait_semaphores->len, ==, expected_waits);
      g_assert_cmpuint (ticket.wait_semaphores->len, ==, 0);
      g_assert_cmpuint (cache_notifications, ==, 0);
      g_assert_cmpuint (ticket.point, ==, 0);
      g_assert_false (pass.submitted);
      g_assert_true (driver.submission_failed);
      g_assert_cmpuint (discarded_recordings, ==, 1);
    }
  g_assert_cmpuint (submissions, ==, scenario >= 4 ? 0 : 1);
  g_array_unref (pass.wait_semaphores);
  g_array_unref (ticket.wait_semaphores);
  g_ptr_array_unref (pass.foreign_images);
}

static void
test_image_state_rollback (void)
{
  CoglDriverVk driver = { .latest_sync_fd = -1 };
  CoglVkDevice device = { .owner = &driver };
  CoglVkFenceTicket ticket = { .binary = (VkSemaphore) (uintptr_t) 2 };
  CoglVkImage color = { .layout = VK_IMAGE_LAYOUT_GENERAL },
              depth = { .layout = VK_IMAGE_LAYOUT_UNDEFINED };
  CoglVkPass pass = { .device = &device, .fence = &ticket,
                      .timestamp_begin = -1, .timestamp_end = -1 };
  g_autoptr (GError) error = NULL;

  pass.foreign_images = g_ptr_array_new ();
  cogl_vk_pass_note_image_state (&pass, &color);
  color.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.dmabuf_graphics_owned = TRUE;
  cogl_vk_pass_note_image_state (&pass, &color);
  cogl_vk_pass_note_image_state (&pass, &depth);
  depth.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  g_assert_cmpuint (pass.image_states->len, ==, 2);
  has_timeline = FALSE;
  end_result = VK_SUCCESS;
  binary_failure = FALSE;
  expected_waits = 0;
  submit_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
  g_assert_false (cogl_vk_pass_submit (&pass, &error));
  g_assert_cmpint (color.layout, ==, VK_IMAGE_LAYOUT_GENERAL);
  g_assert_false (color.dmabuf_graphics_owned);
  g_assert_cmpint (depth.layout, ==, VK_IMAGE_LAYOUT_UNDEFINED);
  g_assert_true (driver.submission_failed);
  g_assert_cmpuint (ticket.point, ==, 0);
  g_ptr_array_unref (pass.foreign_images);
  g_array_unref (pass.image_states);
}

int
main (int    argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_data_func ("/vk-pass/no-waits", GUINT_TO_POINTER (0), check_submit);
  g_test_add_data_func ("/vk-pass/foreign-wait-scope", GUINT_TO_POINTER (1), check_submit);
  g_test_add_data_func ("/vk-pass/submit-failure", GUINT_TO_POINTER (2), check_submit);
  g_test_add_data_func ("/vk-pass/binary-only", GUINT_TO_POINTER (3), check_submit);
  g_test_add_data_func ("/vk-pass/end-recording-failure", GUINT_TO_POINTER (4), check_submit);
  g_test_add_data_func ("/vk-pass/binary-allocation-failure", GUINT_TO_POINTER (5), check_submit);
  g_test_add_func ("/vk-pass/image-state-rollback", test_image_state_rollback);
  return g_test_run ();
}
