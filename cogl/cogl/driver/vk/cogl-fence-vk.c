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

#include "cogl/driver/vk/cogl-fence-vk-private.h"
#include "cogl/driver/vk/cogl-image-vk-private.h"
#include "cogl/driver/vk/cogl-vk-debug-private.h"

#include <gio/gio.h>
#include <stdint.h>
#include <string.h>

static void
destroy_wait_semaphores (CoglVkFenceTicket *ticket)
{
  unsigned int i;

  if (!ticket || !ticket->wait_semaphores || !ticket->device)
    return;

  for (i = 0; i < ticket->wait_semaphores->len; i++)
    {
      VkSemaphore sem =
        g_array_index (ticket->wait_semaphores, VkSemaphore, i);
      if (sem != VK_NULL_HANDLE)
        vkDestroySemaphore (ticket->device->device, sem, NULL);
    }
  g_array_set_size (ticket->wait_semaphores, 0);
}

/* Free the command buffer whose destruction was deferred until this ticket's
 * timeline point signaled. Callers must only invoke this once the point is
 * observed signaled (recycle paths) or at teardown after device idle.
 */
static void
free_deferred_cb (CoglVkFenceTicket *ticket)
{
  if (!ticket || ticket->deferred_cb == VK_NULL_HANDLE || !ticket->device)
    return;

  if (ticket->device->command_pool != VK_NULL_HANDLE)
    vkFreeCommandBuffers (ticket->device->device,
                          ticket->device->command_pool,
                          1, &ticket->deferred_cb);
  ticket->deferred_cb = VK_NULL_HANDLE;
}

/* Called only after completion. A skipped/failed export leaves the binary
 * signaled, so it must not be signaled again on the next submission. */
static void
reset_binary (CoglVkFenceTicket *ticket)
{
  if (ticket->binary != VK_NULL_HANDLE)
    vkDestroySemaphore (ticket->device->device, ticket->binary, NULL);
  ticket->binary = VK_NULL_HANDLE;
  ticket->binary_exported = FALSE;
}

static void
free_transient_buffers (CoglVkFenceTicket *ticket)
{
  if (!ticket->transient_buffers)
    return;

  for (unsigned int i = 0; i < ticket->transient_buffers->len; i++)
    {
      CoglVkTransientBuffer *buffer =
        &g_array_index (ticket->transient_buffers, CoglVkTransientBuffer, i);

      vkDestroyBuffer (ticket->device->device, buffer->buffer, NULL);
      vkFreeMemory (ticket->device->device, buffer->memory, NULL);
    }
  g_clear_pointer (&ticket->transient_buffers, g_array_unref);
}

void
cogl_vk_retire_list_add_image (CoglVkRetireList    *list,
                               struct _CoglVkImage *image)
{
  if (!list->images)
    list->images = g_ptr_array_new ();
  g_ptr_array_add (list->images, image);
}

void
cogl_vk_retire_list_add_handle (CoglVkRetireList  *list,
                                CoglVkRetiredKind  kind,
                                uint64_t           handle)
{
  CoglVkRetiredHandle retired = { .kind = kind, .handle = handle };

  if (!list->handles)
    list->handles = g_array_new (FALSE, FALSE, sizeof (CoglVkRetiredHandle));
  g_array_append_val (list->handles, retired);
}

/* Only once the owning submit is observed complete, or at teardown after the
 * device is idle: nothing submitted up to it still uses these objects. */
void
cogl_vk_retire_list_release (CoglVkRetireList *list,
                             CoglVkDevice     *device)
{
  if (list->handles)
    {
      for (unsigned int i = 0; i < list->handles->len; i++)
        {
          CoglVkRetiredHandle *retired =
            &g_array_index (list->handles, CoglVkRetiredHandle, i);

          switch (retired->kind)
            {
            case COGL_VK_RETIRED_FRAMEBUFFER:
              vkDestroyFramebuffer (device->device,
                                    (VkFramebuffer) retired->handle, NULL);
              break;
            case COGL_VK_RETIRED_IMAGE_VIEW:
              vkDestroyImageView (device->device,
                                  (VkImageView) retired->handle, NULL);
              break;
            case COGL_VK_RETIRED_BUFFER:
              vkDestroyBuffer (device->device,
                               (VkBuffer) retired->handle, NULL);
              break;
            case COGL_VK_RETIRED_MEMORY:
              vkFreeMemory (device->device,
                            (VkDeviceMemory) retired->handle, NULL);
              break;
            }
        }
      g_clear_pointer (&list->handles, g_array_unref);
    }

  if (list->images)
    {
      for (unsigned int i = 0; i < list->images->len; i++)
        cogl_vk_image_destroy_now (g_ptr_array_index (list->images, i));
      g_clear_pointer (&list->images, g_ptr_array_unref);
    }
}

/* Hand a submitted command buffer to the ticket so present can return without
 * a vkQueueWaitIdle; the recycle path frees it once the point is signaled.
 */
void
cogl_vk_fence_ticket_defer_command_buffer (CoglVkFenceTicket *ticket,
                                           VkCommandBuffer    cb)
{
  g_return_if_fail (ticket != NULL);

  /* A ticket only carries one CB per submit cycle; any prior deferred CB was
   * freed when this ticket was recycled (its point was signaled first).
   */
  ticket->deferred_cb = cb;
}

static CoglVkFenceTicket *
fence_ticket_new_unpooled (CoglVkDevice *device,
                           GError      **error)
{
  CoglVkFenceTicket *ticket;
  VkResult res;

  ticket = g_new0 (CoglVkFenceTicket, 1);
  ticket->device = device;
  ticket->point = 0;
  ticket->binary = VK_NULL_HANDLE;
  ticket->wait_semaphores = g_array_new (FALSE, FALSE, sizeof (VkSemaphore));

  if (device->has_timeline_semaphore)
    {
      VkSemaphoreTypeCreateInfo type_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
      };
      VkSemaphoreCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &type_info,
      };

      res = vkCreateSemaphore (device->device, &create_info, NULL,
                               &ticket->timeline);
      if (res != VK_SUCCESS)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "vkCreateSemaphore (timeline) failed (%d)", (int) res);
          g_array_free (ticket->wait_semaphores, TRUE);
          g_free (ticket);
          return NULL;
        }
    }

  if (!cogl_vk_fence_ticket_ensure_binary (ticket, error))
    {
      cogl_vk_fence_ticket_free (ticket);
      return NULL;
    }

  return ticket;
}

void
cogl_vk_device_drain_fence_pool (CoglVkDevice *device)
{
  unsigned int i = 0;

  g_return_if_fail (device != NULL);

  cogl_vk_device_reap_oneshots (device);

  if (!device->fence_inflight)
    return;

  while (i < device->fence_inflight->len)
    {
      CoglVkFenceTicket *ticket =
        g_ptr_array_index (device->fence_inflight, i);

      if (!cogl_vk_fence_ticket_poll_signaled (ticket))
        {
          i++;
          continue;
        }

      /* steal: fence_* arrays use free_func; remove_index would free ticket. */
      ticket = g_ptr_array_steal_index (device->fence_inflight, i);
      destroy_wait_semaphores (ticket);
      /* Point is signaled: the CB is no longer executing, free it now. */
      free_deferred_cb (ticket);
      free_transient_buffers (ticket);
      cogl_vk_retire_list_release (&ticket->retired, ticket->device);
      reset_binary (ticket);
      ticket->signaled_cached = FALSE;
      g_ptr_array_add (device->fence_free, ticket);
      cogl_vk_debug_log ("fence",
                         "drain → free ticket=%p point=%" G_GUINT64_FORMAT
                         " free=%u inflight=%u",
                         ticket, ticket->point,
                         device->fence_free->len,
                         device->fence_inflight->len);
    }
}

CoglVkFenceTicket *
cogl_vk_fence_ticket_acquire (CoglVkDevice *device,
                              GError      **error)
{
  CoglVkFenceTicket *ticket;

  g_return_val_if_fail (device != NULL, NULL);

  cogl_vk_device_drain_fence_pool (device);

  if (device->fence_free && device->fence_free->len > 0)
    {
      /* Must steal: remove_index invokes free_func and frees the ticket. */
      ticket = g_ptr_array_steal_index (device->fence_free,
                                        device->fence_free->len - 1);
      cogl_vk_debug_log ("fence",
                         "acquire recycle ticket=%p point=%" G_GUINT64_FORMAT
                         " binary=%p waits=%p free=%u inflight=%u",
                         ticket, ticket->point,
                         (void *) (uintptr_t) ticket->binary,
                         ticket->wait_semaphores,
                         device->fence_free->len,
                         device->fence_inflight ? device->fence_inflight->len : 0);
      if (!cogl_vk_fence_ticket_ensure_binary (ticket, error))
        {
          cogl_vk_debug_log ("fence",
                             "acquire ensure_binary FAIL ticket=%p", ticket);
          /* Descriptor pool slots may still query this ticket's timeline.
           * Keep the completed ticket alive so allocation can be retried.
           */
          g_ptr_array_add (device->fence_free, ticket);
          return NULL;
        }
      return ticket;
    }

  ticket = fence_ticket_new_unpooled (device, error);
  if (ticket)
    cogl_vk_debug_log ("fence",
                       "acquire new ticket=%p binary=%p waits=%p",
                       ticket,
                       (void *) (uintptr_t) ticket->binary,
                       ticket->wait_semaphores);
  return ticket;
}

void
cogl_vk_fence_ticket_release (CoglVkFenceTicket *ticket)
{
  if (!ticket)
    return;

  if (!ticket->device->fence_inflight)
    {
      cogl_vk_debug_log ("fence", "release free(no pool) ticket=%p", ticket);
      cogl_vk_fence_ticket_free (ticket);
      return;
    }

  /* Recycle only after observing completion; otherwise keep the ticket. */
  if (cogl_vk_fence_ticket_poll_signaled (ticket))
    {
      destroy_wait_semaphores (ticket);
      free_deferred_cb (ticket);
      free_transient_buffers (ticket);
      cogl_vk_retire_list_release (&ticket->retired, ticket->device);
      reset_binary (ticket);
      ticket->signaled_cached = FALSE;
      g_ptr_array_add (ticket->device->fence_free, ticket);
      cogl_vk_debug_log ("fence",
                         "release → free ticket=%p point=%" G_GUINT64_FORMAT
                         " free=%u",
                         ticket, ticket->point,
                         ticket->device->fence_free->len);
    }
  else
    {
      g_ptr_array_add (ticket->device->fence_inflight, ticket);
      cogl_vk_debug_log ("fence",
                         "release → inflight ticket=%p point=%" G_GUINT64_FORMAT
                         " inflight=%u",
                         ticket, ticket->point,
                         ticket->device->fence_inflight->len);
    }
}

gboolean
cogl_vk_fence_ticket_ensure_binary (CoglVkFenceTicket *ticket,
                                    GError           **error)
{
  VkExportSemaphoreCreateInfo export_info = {
    .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
    .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
  };
  VkSemaphoreCreateInfo create_info = {
    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    .pNext = &export_info,
  };
  VkSemaphore binary = VK_NULL_HANDLE;
  VkResult res;

  g_return_val_if_fail (ticket != NULL, FALSE);

  /* A SYNC_FD export consumed the old binary but deferred its destruction to
   * avoid draining the queue. Every caller reaches here only for a ticket
   * whose timeline point is already signaled (recycled from fence_free, or
   * drained from inflight after poll_signaled), so the binary's signal submit
   * has completed and destroying it now satisfies VUID-05149. Destroy then
   * recreate.
   */
  if (ticket->binary_exported && ticket->binary != VK_NULL_HANDLE)
    {
      vkDestroySemaphore (ticket->device->device, ticket->binary, NULL);
      ticket->binary = VK_NULL_HANDLE;
      ticket->binary_exported = FALSE;
    }

  if (ticket->binary != VK_NULL_HANDLE)
    return TRUE;

  /* Offscreen software devices may have no external semaphore handles.
   * Keep a real binary semaphore and the same completion/recycle ordering;
   * only the export capability is optional. */
  if (!ticket->device->has_external_semaphore_fd)
    create_info.pNext = NULL;

  res = vkCreateSemaphore (ticket->device->device, &create_info, NULL,
                           &binary);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkCreateSemaphore (binary SYNC_FD) failed (%d)", (int) res);
      return FALSE;
    }

  ticket->binary = binary;
  return TRUE;
}

gboolean
cogl_vk_fence_ticket_poll_signaled (CoglVkFenceTicket *ticket)
{
  uint64_t value = 0;
  VkResult res;

  g_return_val_if_fail (ticket != NULL, FALSE);

  if (ticket->signaled_cached)
    return TRUE;

  if (ticket->timeline == VK_NULL_HANDLE || ticket->point == 0)
    {
      /* Binary-only / not yet submitted: do NOT claim signaled — pass_free
       * would FreeCommandBuffers while the CB is still on the queue.
       */
      return ticket->signaled_cached;
    }

  if (!ticket->device->vkGetSemaphoreCounterValue)
    {
      /* An unavailable query is not evidence of completion. */
      return FALSE;
    }

  res = ticket->device->vkGetSemaphoreCounterValue (ticket->device->device,
                                                    ticket->timeline,
                                                    &value);
  if (res != VK_SUCCESS)
    {
      cogl_vk_device_note_vk_result (ticket->device, res);
      return FALSE;
    }

  if (value >= ticket->point)
    {
      ticket->signaled_cached = TRUE;
      return TRUE;
    }

  return FALSE;
}

void
cogl_vk_fence_ticket_free (CoglVkFenceTicket *ticket)
{
  if (!ticket)
    return;

  cogl_vk_debug_log ("fence",
                     "free ticket=%p point=%" G_GUINT64_FORMAT
                     " binary=%p timeline=%p waits=%p",
                     ticket, ticket->point,
                     (void *) (uintptr_t) ticket->binary,
                     (void *) (uintptr_t) ticket->timeline,
                     ticket->wait_semaphores);

  destroy_wait_semaphores (ticket);
  if (ticket->wait_semaphores)
    {
      g_array_free (ticket->wait_semaphores, TRUE);
      ticket->wait_semaphores = NULL;
    }

  /* Teardown only reaches here after the device is idle (cogl_vk_device_free
   * waits), so a still-deferred CB is no longer executing and is safe to
   * free.
   */
  free_deferred_cb (ticket);
  free_transient_buffers (ticket);
  cogl_vk_retire_list_release (&ticket->retired, ticket->device);

  if (ticket->device)
    {
      if (ticket->binary != VK_NULL_HANDLE)
        vkDestroySemaphore (ticket->device->device, ticket->binary, NULL);
      if (ticket->timeline != VK_NULL_HANDLE)
        vkDestroySemaphore (ticket->device->device, ticket->timeline, NULL);
    }
  ticket->binary = VK_NULL_HANDLE;
  ticket->timeline = VK_NULL_HANDLE;
  g_free (ticket);
}

int
cogl_vk_fence_ticket_export_sync_fd (CoglVkFenceTicket *ticket,
                                     GError           **error)
{
  VkSemaphoreGetFdInfoKHR info;
  int fd = -1;
  VkResult res;

  g_return_val_if_fail (ticket != NULL, -1);

  if (!ticket->device->has_external_semaphore_fd ||
      !ticket->device->vkGetSemaphoreFdKHR)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "VK_KHR_external_semaphore_fd unavailable");
      return -1;
    }

  if (ticket->binary == VK_NULL_HANDLE)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No binary semaphore to export (submit first)");
      return -1;
    }

  memset (&info, 0, sizeof (info));
  info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
  info.semaphore = ticket->binary;
  info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;

  res = ticket->device->vkGetSemaphoreFdKHR (ticket->device->device, &info, &fd);
  if (res != VK_SUCCESS)
    {
      cogl_vk_device_note_vk_result (ticket->device, res);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkGetSemaphoreFdKHR failed (%d)", (int) res);
      return -1;
    }

  /* SYNC_FD export transfers ownership; the binary must be recreated before
   * the next submit. Do NOT drain the queue and destroy it here — that put a
   * full vkQueueWaitIdle on every present, cancelling the KMS IN_FENCE model
   * (the display controller, not the CPU, waits on this fd). Mark it consumed
   * and let the recycle path destroy it once the timeline point is observed
   * signaled: timeline and binary are signalled by the same submit, so
   * poll_signaled() proves the binary's signal is no longer in flight, which
   * is what VUID-vkDestroySemaphore-semaphore-05149 requires.
   */
  ticket->binary_exported = TRUE;

  return fd;
}
