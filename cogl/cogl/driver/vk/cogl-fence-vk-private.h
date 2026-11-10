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

/* Submission completion and SYNC_FD export for the Cogl Vulkan driver.
 *
 * A timeline semaphore tracks completion within Cogl. A binary semaphore
 * supplies the SYNC_FD passed to Mutter native presentation backend.
 * Fence tickets and their resources are recycled only after completion has
 * been observed.
 */

#pragma once

#include "cogl/driver/vk/cogl-device-vk-private.h"

G_BEGIN_DECLS

struct _CoglVkImage;

typedef struct
{
  VkBuffer buffer;
  VkDeviceMemory memory;
} CoglVkTransientBuffer;


struct _CoglVkFenceTicket
{
  CoglVkDevice *device; /* unowned */
  VkSemaphore timeline;
  uint64_t point;
  /* Recreated each submit / after SYNC_FD export. */
  VkSemaphore binary;
  /* SYNC_FD export left `binary` logically consumed but not yet destroyed;
   * it is destroyed lazily once the timeline point is observed signaled, so
   * the export path never has to drain the queue.
   */
  gboolean binary_exported;
  gboolean signaled_cached;
  /* Command buffer whose free was deferred until this ticket's timeline point
   * is observed signaled — lets present return without vkQueueWaitIdle. Freed
   * from device->command_pool by the recycle path. VK_NULL_HANDLE when none.
   */
  VkCommandBuffer deferred_cb;
  /* Imported wait semaphores remain alive until submission completion. */
  GArray *wait_semaphores; /* VkSemaphore */
  GArray *transient_buffers; /* CoglVkTransientBuffer, owned until completion */
  /* Freed while this or an earlier submit may still use them; destroyed once
   * the point is observed signaled (cogl_driver_vk_retire_list). */
  CoglVkRetireList retired;
  /* device->submit_serial of this ticket's last submit. */
  uint64_t submit_serial;
};

CoglVkFenceTicket * cogl_vk_fence_ticket_acquire (CoglVkDevice *device,
                                                  GError      **error);

/* After submit+export: park in inflight, or destroy if no pool. */
void cogl_vk_fence_ticket_release (CoglVkFenceTicket *ticket);

void cogl_vk_fence_ticket_free (CoglVkFenceTicket *ticket);

gboolean cogl_vk_fence_ticket_ensure_binary (CoglVkFenceTicket *ticket,
                                             GError           **error);

gboolean cogl_vk_fence_ticket_poll_signaled (CoglVkFenceTicket *ticket);

/* Transfer ownership of a submitted command buffer to the ticket. The recycle
 * path frees it from device->command_pool once the timeline point is observed
 * signaled, so present need not vkQueueWaitIdle before releasing the pass.
 */
void cogl_vk_fence_ticket_defer_command_buffer (CoglVkFenceTicket *ticket,
                                                VkCommandBuffer    cb);

/* Take ownership of an image (destroyed with cogl_vk_image_destroy_now) or a
 * framebuffer / view handle. The owner of the list releases it once its
 * submit is observed complete, or at teardown after the device is idle. */
void cogl_vk_retire_list_add_image (CoglVkRetireList    *list,
                                    struct _CoglVkImage *image);

void cogl_vk_retire_list_add_handle (CoglVkRetireList  *list,
                                     CoglVkRetiredKind  kind,
                                     uint64_t           handle);

void cogl_vk_retire_list_release (CoglVkRetireList *list,
                                  CoglVkDevice     *device);

/* Returns a new sync_file fd for KMS, or -1 on failure. Caller owns fd.
 * Marks the binary semaphore consumed; the recycle path recreates it before
 * the next submit, without draining the queue.
 */
int cogl_vk_fence_ticket_export_sync_fd (CoglVkFenceTicket *ticket,
                                         GError           **error);

G_END_DECLS
