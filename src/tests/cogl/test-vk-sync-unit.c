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

/* Fault injection against the production implementation, without a GPU.
 * Unused driver entry points are discarded by --gc-sections.
 */
#include "config.h"
#include "cogl/driver/vk/cogl-driver-vk-private.h"

#define COGL_IS_DRIVER_VK(object) ((object) != NULL)
#include "cogl/driver/vk/cogl-driver-vk.c"
#include "cogl/driver/vk/cogl-fence-vk.c"

static unsigned int waits;
static unsigned int destroys;
static unsigned int creates;
static VkResult export_result;
static VkResult idle_result = VK_SUCCESS;
static VkResult semaphore_create_result = VK_SUCCESS;
static int exported_fd = -1;
static uint64_t completed_point;
static unsigned int flushes;
static unsigned int buffers_destroyed;
static gboolean index_upload_test;
static unsigned int memory_flushes;
static VkDeviceSize last_buffer_size;
static unsigned int indexed_draws;
static unsigned int drawn_indices;
static int32_t drawn_vertex_offset;
static VkIndexType bound_index_type;
static unsigned int source_maps;
static unsigned int source_unmaps;
static gboolean source_map_failure;

gboolean
cogl_vk_debug_enabled (const char *channel)
{
  return FALSE;
}

static void
test_flush_pass (CoglDriverVk *driver)
{
  flushes++;
}

#define cogl_driver_vk_flush_current_pass test_flush_pass
#include "cogl/driver/vk/cogl-buffer-impl-vk.c"
#undef cogl_driver_vk_flush_current_pass
#include "cogl/driver/vk/cogl-pipeline-vk-vbo.c"
#define peek_driver_vk framebuffer_peek_driver_vk
#include "cogl/driver/vk/cogl-framebuffer-vk.c"
#undef peek_driver_vk

static CoglPipelineLayer acquire_layer;
static int texture_acquire_fd;
static gboolean acquire_import_succeeds;

void
cogl_vk_pipeline_collect_layers (CoglPipeline        *pipeline,
                                 CoglPipelineLayer ***layers,
                                 int                 *count)
{
  *layers = g_new (CoglPipelineLayer *, 1);
  (*layers)[0] = &acquire_layer;
  *count = 1;
}

CoglTexture *
_cogl_pipeline_layer_get_texture (CoglPipelineLayer *layer)
{
  return (CoglTexture *) &acquire_layer;
}

CoglTexture2DVk *
cogl_texture_2d_vk_resolve (CoglTexture *texture)
{
  return (CoglTexture2DVk *) texture;
}

int
cogl_texture_2d_vk_take_acquire_sync_fd (CoglTexture2DVk *texture)
{
  int fd = texture_acquire_fd;
  texture_acquire_fd = -1;
  return fd;
}

void
cogl_texture_2d_vk_set_acquire_sync_fd (CoglTexture2DVk *texture,
                                        int              fd)
{
  texture_acquire_fd = fd;
}

gboolean
cogl_vk_pass_add_wait_sync_fd (CoglVkPass *pass,
                               int         fd,
                               GError    **error)
{
  if (!acquire_import_succeeds)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "injected failure");
      return FALSE;
    }
  close (fd);
  return TRUE;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyBuffer (VkDevice                     device,
                 VkBuffer                     buffer,
                 const VkAllocationCallbacks *allocator)
{
  if (!index_upload_test)
    {
      g_assert_cmpuint (waits, >, 0);
      g_assert_cmpuint (flushes, >, 0);
    }
  buffers_destroyed++;
}

VKAPI_ATTR void VKAPI_CALL
vkFreeMemory (VkDevice                     device,
              VkDeviceMemory               memory,
              const VkAllocationCallbacks *allocator)
{
  if (index_upload_test)
    g_free ((void *) (uintptr_t) memory);
  else
    g_assert_cmpuint (buffers_destroyed, ==, 1);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateBuffer (VkDevice                     device,
                const VkBufferCreateInfo    *info,
                const VkAllocationCallbacks *allocator,
                VkBuffer                    *buffer)
{
  static uintptr_t next_buffer = 10;
  last_buffer_size = info->size;
  *buffer = (VkBuffer) next_buffer++;
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkGetBufferMemoryRequirements (VkDevice              device,
                               VkBuffer              buffer,
                               VkMemoryRequirements *requirements)
{
  *requirements = (VkMemoryRequirements) {
    .size = MAX (4096, last_buffer_size), .alignment = 64, .memoryTypeBits = 1,
  };
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceMemoryProperties (VkPhysicalDevice                  device,
                                     VkPhysicalDeviceMemoryProperties *properties)
{
  *properties = (VkPhysicalDeviceMemoryProperties) { .memoryTypeCount = 1 };
  properties->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateMemory (VkDevice                     device,
                  const VkMemoryAllocateInfo  *info,
                  const VkAllocationCallbacks *allocator,
                  VkDeviceMemory              *memory)
{
  *memory = (VkDeviceMemory) (uintptr_t) g_malloc0 (info->allocationSize);
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkBindBufferMemory (VkDevice       device,
                    VkBuffer       buffer,
                    VkDeviceMemory memory,
                    VkDeviceSize   offset)
{
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkMapMemory (VkDevice         device,
             VkDeviceMemory   memory,
             VkDeviceSize     offset,
             VkDeviceSize     size,
             VkMemoryMapFlags flags,
             void           **data)
{
  *data = (void *) (uintptr_t) memory;
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkFlushMappedMemoryRanges (VkDevice                   device,
                           uint32_t                   count,
                           const VkMappedMemoryRange *ranges)
{
  g_assert_cmpuint (count, ==, 1);
  g_assert_true (ranges[0].size == VK_WHOLE_SIZE);
  memory_flushes++;
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkUnmapMemory (VkDevice       device,
               VkDeviceMemory memory)
{
}

void
cogl_vk_debug_log (const char *channel,
                   const char *format,
                   ...)
{
}

void
cogl_vk_device_note_vk_result (CoglVkDevice *device,
                               VkResult      result)
{
  if (result == VK_ERROR_DEVICE_LOST)
    device->device_lost = TRUE;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkQueueWaitIdle (VkQueue queue)
{
  waits++;
  return idle_result;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateSemaphore (VkDevice                     device,
                   const VkSemaphoreCreateInfo *info,
                   const VkAllocationCallbacks *allocator,
                   VkSemaphore                 *out)
{
  creates++;
  *out = (VkSemaphore) (uintptr_t) (100 + creates);
  return semaphore_create_result;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroySemaphore (VkDevice                     device,
                    VkSemaphore                  semaphore,
                    const VkAllocationCallbacks *allocator)
{
  destroys++;
}

VKAPI_ATTR void VKAPI_CALL
vkFreeCommandBuffers (VkDevice               device,
                      VkCommandPool          pool,
                      uint32_t               count,
                      const VkCommandBuffer *buffers)
{
}

static VkResult VKAPI_CALL
export_fd (VkDevice                       device,
           const VkSemaphoreGetFdInfoKHR *info,
           int                           *fd)
{
  *fd = exported_fd;
  return export_result;
}

static VkResult VKAPI_CALL
get_counter (VkDevice    device,
             VkSemaphore semaphore,
             uint64_t   *value)
{
  *value = completed_point;
  return VK_SUCCESS;
}

static void
test_export_fallback (void)
{
  CoglVkDevice device = {
    .graphics_queue = (VkQueue) (uintptr_t) 1,
    .has_external_semaphore_fd = TRUE,
    .vkGetSemaphoreFdKHR = export_fd,
  };
  CoglVkFenceTicket ticket = { .device = &device, .binary = (VkSemaphore) 1 };
  CoglVkPass pass = { .fence = &ticket };
  CoglDriverVk driver = {
    .vk_device = &device, .current_pass = &pass, .pending_drm_fd = 3,
  };
  int stale_fd = open ("/dev/null", O_RDONLY);

  g_assert_cmpint (stale_fd, >=, 0);
  driver.latest_sync_fd = stale_fd;
  waits = 0;
  export_result = VK_ERROR_TOO_MANY_OBJECTS;
  cogl_driver_vk_store_sync_fd_from_pass (&driver);
  g_assert_cmpuint (waits, ==, 1);
  g_assert_true (ticket.signaled_cached);
  g_assert_cmpint (driver.latest_sync_fd, ==, -1);
  g_assert_cmpint (fcntl (stale_fd, F_GETFD), ==, -1);
}

static void
test_export_signaled_fd (void)
{
  CoglVkDevice device = {
    .graphics_queue = (VkQueue) (uintptr_t) 1,
    .has_external_semaphore_fd = TRUE,
    .vkGetSemaphoreFdKHR = export_fd,
  };
  CoglVkFenceTicket ticket = { .device = &device, .binary = (VkSemaphore) 1 };
  CoglVkPass pass = { .fence = &ticket };
  CoglDriverVk driver = {
    .vk_device = &device, .current_pass = &pass, .pending_drm_fd = 3,
    .latest_sync_fd = -1,
  };

  waits = 0;
  export_result = VK_SUCCESS;
  cogl_driver_vk_store_sync_fd_from_pass (&driver);
  g_assert_cmpuint (waits, ==, 0);
  g_assert_true (ticket.binary_exported);
  g_assert_cmpint (driver.latest_sync_fd, ==, -1);
}

static void
test_export_current_fd (void)
{
  CoglVkDevice device = {
    .graphics_queue = (VkQueue) (uintptr_t) 1,
    .has_external_semaphore_fd = TRUE,
    .vkGetSemaphoreFdKHR = export_fd,
  };
  CoglVkFenceTicket ticket = { .device = &device, .binary = (VkSemaphore) 1 };
  CoglVkPass pass = { .fence = &ticket };
  CoglDriverVk driver = {
    .vk_device = &device, .current_pass = &pass, .pending_drm_fd = 3,
  };
  int old_fd = open ("/dev/null", O_RDONLY);

  exported_fd = open ("/dev/null", O_RDONLY);
  g_assert_cmpint (old_fd, >=, 0);
  g_assert_cmpint (exported_fd, >=, 0);
  driver.latest_sync_fd = old_fd;
  export_result = VK_SUCCESS;
  waits = 0;
  cogl_driver_vk_store_sync_fd_from_pass (&driver);
  g_assert_cmpuint (waits, ==, 0);
  g_assert_cmpint (driver.latest_sync_fd, ==, exported_fd);
  g_assert_cmpint (fcntl (old_fd, F_GETFD), ==, -1);
  close (exported_fd);
  exported_fd = -1;
}

static void
test_no_export_support (void)
{
  CoglVkDevice device = { .graphics_queue = (VkQueue) (uintptr_t) 1 };
  CoglVkFenceTicket ticket = { .device = &device };
  CoglVkPass pass = { .fence = &ticket };
  CoglDriverVk driver = {
    .vk_device = &device, .current_pass = &pass, .pending_drm_fd = 3,
    .latest_sync_fd = -1,
  };

  waits = 0;
  cogl_driver_vk_store_sync_fd_from_pass (&driver);
  g_assert_cmpuint (waits, ==, 1);
  g_assert_true (ticket.signaled_cached);
}

static void
test_offscreen_flush_is_async (void)
{
  CoglVkDevice device = { .graphics_queue = (VkQueue) (uintptr_t) 1 };
  CoglVkFenceTicket ticket = { .device = &device, .timeline = (VkSemaphore) 1 };
  CoglVkPass pass = { .fence = &ticket };
  CoglDriverVk driver = {
    .vk_device = &device, .current_pass = &pass, .pending_drm_fd = -1,
  };
  int stale_fd = open ("/dev/null", O_RDONLY);

  g_assert_cmpint (stale_fd, >=, 0);
  driver.latest_sync_fd = stale_fd;
  waits = 0;
  cogl_driver_vk_store_sync_fd_from_pass (&driver);
  g_assert_cmpuint (waits, ==, 0);
  g_assert_false (ticket.signaled_cached);
  g_assert_cmpint (driver.latest_sync_fd, ==, -1);
  g_assert_cmpint (fcntl (stale_fd, F_GETFD), ==, -1);
}

static void
test_failed_wait_stops_presentation (void)
{
  if (g_test_subprocess ())
    {
      idle_result = VK_ERROR_DEVICE_LOST;
      test_no_export_support ();
      return;
    }
  g_test_trap_subprocess (NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
  g_test_trap_assert_failed ();
  g_test_trap_assert_stderr ("*presentation wait failed*");
}

static void
test_dup_failure_waits (void)
{
  CoglVkDevice device = { .graphics_queue = (VkQueue) (uintptr_t) 1 };
  CoglDriverVk driver = { .vk_device = &device };
  int fd = open ("/dev/null", O_RDONLY);

  g_assert_cmpint (fd, >=, 0);
  close (fd);
  driver.latest_sync_fd = fd;
  waits = 0;
  g_assert_cmpint (cogl_driver_vk_get_latest_sync_fd (&driver), ==, -1);
  g_assert_cmpuint (waits, ==, 1);
}

static void
test_missing_counter (void)
{
  CoglVkDevice device = { 0 };
  CoglVkFenceTicket ticket = {
    .device = &device, .timeline = (VkSemaphore) 1, .point = 1,
  };

  g_assert_false (cogl_vk_fence_ticket_poll_signaled (&ticket));
  g_assert_false (ticket.signaled_cached);
}

static void
test_buffer_retirement (void)
{
  CoglDriverVk driver = { 0 };
  CoglVkDevice device = {
    .owner = &driver, .graphics_queue = (VkQueue) (uintptr_t) 1,
  };
  VkBuffer buffer = (VkBuffer) 1;
  VkDeviceMemory memory = (VkDeviceMemory) 2;

  waits = flushes = buffers_destroyed = 0;
  destroy_vk_buffer (&device, &buffer, &memory);
  g_assert_cmpuint (waits, ==, 1);
  g_assert_cmpuint (buffers_destroyed, ==, 1);
  g_assert_true (buffer == VK_NULL_HANDLE);
  g_assert_true (memory == VK_NULL_HANDLE);
}

static void
test_index_snapshots (void)
{
  CoglVkDevice device = {
    .vkGetSemaphoreCounterValue = get_counter,
    .fence_free = g_ptr_array_new (),
    .fence_inflight = g_ptr_array_new (),
  };
  CoglVkFenceTicket ticket = {
    .device = &device, .timeline = (VkSemaphore) 1, .point = 2,
  };
  CoglVkPass pass = { .device = &device, .fence = &ticket };
  const uint16_t first[] = { 1, 2, 3 };
  const uint16_t second[] = { 4, 5, 6 };
  VkBuffer a, b;
  CoglVkTransientBuffer *stored;

  index_upload_test = TRUE;
  buffers_destroyed = memory_flushes = 0;
  g_assert_true (upload_indices (&pass, first, sizeof (first), &a, NULL));
  g_assert_true (upload_indices (&pass, second, sizeof (second), &b, NULL));
  g_assert_true (a != b);
  g_assert_cmpuint (ticket.transient_buffers->len, ==, 2);
  stored = (CoglVkTransientBuffer *) ticket.transient_buffers->data;
  g_assert_cmpmem ((void *) (uintptr_t) stored[0].memory, sizeof (first),
                   first, sizeof (first));
  g_assert_cmpmem ((void *) (uintptr_t) stored[1].memory, sizeof (second),
                   second, sizeof (second));
  g_assert_cmpuint (memory_flushes, ==, 2);
  g_assert_cmpuint (buffers_destroyed, ==, 0);
  completed_point = 1;
  cogl_vk_fence_ticket_release (&ticket);
  cogl_vk_device_drain_fence_pool (&device);
  g_assert_cmpuint (buffers_destroyed, ==, 0);
  completed_point = 2;
  cogl_vk_device_drain_fence_pool (&device);
  g_assert_cmpuint (buffers_destroyed, ==, 2);
  g_ptr_array_unref (device.fence_free);
  g_ptr_array_unref (device.fence_inflight);
  index_upload_test = FALSE;
}

VKAPI_ATTR void VKAPI_CALL
vkCmdBindIndexBuffer (VkCommandBuffer command_buffer,
                      VkBuffer        buffer,
                      VkDeviceSize    offset,
                      VkIndexType     type)
{
  g_assert_true (buffer != VK_NULL_HANDLE);
  g_assert_cmpuint (offset, ==, 0);
  bound_index_type = type;
}

VKAPI_ATTR void VKAPI_CALL
vkCmdDrawIndexed (VkCommandBuffer command_buffer,
                  uint32_t        index_count,
                  uint32_t        instance_count,
                  uint32_t        first_index,
                  int32_t         vertex_offset,
                  uint32_t        first_instance)
{
  indexed_draws++;
  drawn_indices = index_count;
  drawn_vertex_offset = vertex_offset;
  g_assert_cmpuint (instance_count, ==, 1);
  g_assert_cmpuint (first_index, ==, 0);
  g_assert_cmpuint (first_instance, ==, 0);
}

static void
test_line_loop (gconstpointer data)
{
  CoglVkDevice device = { 0 };
  CoglVkFenceTicket ticket = { .device = &device };
  CoglVkPass pass = { .device = &device, .fence = &ticket };
  int first = GPOINTER_TO_INT (data);
  int count = first == 0 ? 65536 : 3;
  CoglVkTransientBuffer *stored;
  const uint16_t *indices;

  index_upload_test = TRUE;
  indexed_draws = 0;
  g_assert_true (draw_line_loop (&pass, first, count, NULL));
  g_assert_cmpuint (indexed_draws, ==, 1);
  g_assert_cmpint (bound_index_type, ==, VK_INDEX_TYPE_UINT16);
  g_assert_cmpuint (drawn_indices, ==, count + 1);
  g_assert_cmpint (drawn_vertex_offset, ==, first);
  g_assert_cmpuint (ticket.transient_buffers->len, ==, 1);
  stored = &g_array_index (ticket.transient_buffers, CoglVkTransientBuffer, 0);
  indices = (const uint16_t *) (uintptr_t) stored->memory;
  for (int k = 0; k < count; k++)
    g_assert_cmpuint ((uint32_t) indices[k] + drawn_vertex_offset, ==,
                      (uint32_t) first + k);
  g_assert_cmpuint (indices[count], ==, 0);
  free_transient_buffers (&ticket);
  index_upload_test = FALSE;
}

static void
test_line_loop_invalid (void)
{
  CoglVkPass pass = { 0 };
  const int counts[] = { -1, G_MININT };

  indexed_draws = 0;
  for (unsigned int i = 0; i < G_N_ELEMENTS (counts); i++)
    {
      g_autoptr (GError) error = NULL;
      g_assert_false (draw_line_loop (&pass, 0, counts[i], &error));
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
    }
  g_assert_false (draw_line_loop (&pass, -1, 3, NULL));
  g_assert_true (draw_line_loop (&pass, 0, 0, NULL));
  g_assert_true (draw_line_loop (&pass, 0, 1, NULL));
  g_assert_cmpuint (indexed_draws, ==, 0);
}

/* The fixtures below provide Cogl storage only. Conversion, bounds checks,
 * uploads and draw recording all use the production implementation.
 */
unsigned int
cogl_buffer_get_size (CoglBuffer *buffer)
{
  return buffer->size;
}

size_t
cogl_indices_type_get_size (CoglIndicesType type)
{
  switch (type)
    {
    case COGL_INDICES_TYPE_UNSIGNED_BYTE: return 1;
    case COGL_INDICES_TYPE_UNSIGNED_SHORT: return 2;
    case COGL_INDICES_TYPE_UNSIGNED_INT: return 4;
    }
  return 0;
}

void *
_cogl_buffer_map (CoglBuffer       *buffer,
                  CoglBufferAccess  access,
                  CoglBufferMapHint hints,
                  GError          **error)
{
  source_maps++;
  g_assert_cmpint (access, ==, COGL_BUFFER_ACCESS_READ);
  if (source_map_failure)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Injected index map failure");
      return NULL;
    }
  return buffer->data;
}

void
cogl_buffer_unmap (CoglBuffer *buffer)
{
  source_unmaps++;
}

static void
test_line_loop_wide (void)
{
  CoglVkDevice device = { 0 };
  CoglVkFenceTicket ticket = { .device = &device };
  CoglVkPass pass = { .device = &device, .fence = &ticket };
  const int counts[] = { 65537, 70000 };

  index_upload_test = TRUE;
  for (unsigned int i = 0; i < G_N_ELEMENTS (counts); i++)
    {
      CoglVkTransientBuffer *stored;
      const uint32_t *indices;

      indexed_draws = 0;
      g_assert_true (draw_line_loop (&pass, 70000, counts[i], NULL));
      g_assert_cmpuint (indexed_draws, ==, 1);
      g_assert_cmpint (bound_index_type, ==, VK_INDEX_TYPE_UINT32);
      g_assert_cmpuint (drawn_indices, ==, counts[i] + 1);
      g_assert_cmpint (drawn_vertex_offset, ==, 70000);
      g_assert_cmpuint (last_buffer_size, ==, (counts[i] + 1) * sizeof (uint32_t));
      stored = &g_array_index (ticket.transient_buffers, CoglVkTransientBuffer, 0);
      indices = (const uint32_t *) (uintptr_t) stored->memory;
      for (int k = 0; k < counts[i]; k++)
        g_assert_cmpuint (indices[k], ==, k);
      g_assert_cmpuint (indices[counts[i]], ==, 0);
      free_transient_buffers (&ticket);
    }
  index_upload_test = FALSE;
}

static void
test_converted_indices (gconstpointer data)
{
  CoglIndicesType type = GPOINTER_TO_INT (data);
  uint8_t source8[] = { 9, 255, 0, 42, 7 };
  uint16_t source16[] = { 9, 65535, 0, 42, 7 };
  uint32_t source32[] = { 9, 70000, 0, G_MAXUINT32, 7 };
  CoglBuffer source = { .size = 5 * cogl_indices_type_get_size (type) };
  CoglVkDevice device = { 0 };
  CoglVkFenceTicket ticket = { .device = &device };
  CoglVkPass pass = { .device = &device, .fence = &ticket };

  source.data = type == COGL_INDICES_TYPE_UNSIGNED_BYTE ? (void *) source8 :
                type == COGL_INDICES_TYPE_UNSIGNED_SHORT ? (void *) source16 : (void *) source32;
  index_upload_test = TRUE;
  source_maps = source_unmaps = 0;
  for (int loop = 0; loop <= 1; loop++)
    {
      CoglVkTransientBuffer *stored;
      size_t size = type == COGL_INDICES_TYPE_UNSIGNED_INT ? 4 : 2;
      uint16_t expected16[] = { type == COGL_INDICES_TYPE_UNSIGNED_BYTE ? 255 : 65535,
                                0, 42, type == COGL_INDICES_TYPE_UNSIGNED_BYTE ? 255 : 65535 };
      uint32_t expected32[] = { 70000, 0, G_MAXUINT32, 70000 };
      const void *expected = size == 4 ? (void *) expected32 : (void *) expected16;

      indexed_draws = 0;
      g_assert_true (draw_converted_indices (&pass, &source, type,
                                             1, 3, loop, NULL));
      g_assert_cmpuint (indexed_draws, ==, 1);
      g_assert_cmpint (bound_index_type, ==, size == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
      g_assert_cmpuint (drawn_indices, ==, 3 + loop);
      g_assert_cmpint (drawn_vertex_offset, ==, 0);
      g_assert_cmpuint (last_buffer_size, ==, (3 + loop) * size);
      stored = &g_array_index (ticket.transient_buffers, CoglVkTransientBuffer, 0);
      g_assert_cmpmem ((void *) (uintptr_t) stored->memory, last_buffer_size,
                       expected, (3 + loop) * size);
      free_transient_buffers (&ticket);
    }
  g_assert_cmpuint (source_maps, ==, 2);
  g_assert_cmpuint (source_unmaps, ==, 2);
  index_upload_test = FALSE;
}

static void
test_index_ranges (void)
{
  CoglVkPass pass = { 0 };
  CoglBuffer source = { .size = 16 };
  const int ranges[][2] = {
    { -1, 3 }, { 1, -1 }, { 5, 0 }, { 4, 1 }, { 3, 2 },
    { G_MAXINT, 2 }, { 1, G_MAXINT }, { G_MAXINT, G_MAXINT },
  };

  indexed_draws = source_maps = source_unmaps = 0;
  for (unsigned int i = 0; i < G_N_ELEMENTS (ranges); i++)
    {
      g_autoptr (GError) error = NULL;

      g_assert_false (draw_converted_indices (&pass, &source,
                                              COGL_INDICES_TYPE_UNSIGNED_INT,
                                              ranges[i][0], ranges[i][1], TRUE, &error));
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
    }
  g_assert_true (validate_index_range (16, 4, 1, 3, NULL));
  g_assert_true (validate_index_range (16, 4, 4, 0, NULL));
  g_assert_false (validate_index_range (15, 4, 0, 4, NULL));
  g_assert_false (validate_index_range (16, 0, 0, 1, NULL));
  g_assert_true (draw_converted_indices (&pass, &source,
                                         COGL_INDICES_TYPE_UNSIGNED_INT,
                                         4, 0, TRUE, NULL));
  g_assert_true (draw_converted_indices (&pass, &source,
                                         COGL_INDICES_TYPE_UNSIGNED_INT,
                                         3, 1, TRUE, NULL));
  g_assert_cmpuint (indexed_draws, ==, 0);
  g_assert_cmpuint (source_maps, ==, 0);
  g_assert_cmpuint (source_unmaps, ==, 0);
}

static void
test_index_premapped (void)
{
  uint32_t values[] = { 7, 70000, 80000 };
  CoglBuffer source = {
    .size = sizeof (values), .data = (void *) values,
    .flags = COGL_BUFFER_FLAG_MAPPED,
  };
  CoglVkDevice device = { 0 };
  CoglVkFenceTicket ticket = { .device = &device };
  CoglVkPass pass = { .device = &device, .fence = &ticket };
  CoglVkTransientBuffer *stored;
  const uint32_t expected[] = { 70000, 80000, 70000 };

  index_upload_test = TRUE;
  source_maps = source_unmaps = indexed_draws = 0;
  /* A second map would submit/free the active pass for DEVICE_LOCAL storage. */
  source_map_failure = TRUE;
  g_assert_true (draw_converted_indices (&pass, &source,
                                         COGL_INDICES_TYPE_UNSIGNED_INT,
                                         1, 2, TRUE, NULL));
  g_assert_cmpuint (source_maps, ==, 0);
  g_assert_cmpuint (source_unmaps, ==, 0);
  g_assert_cmpuint (indexed_draws, ==, 1);
  stored = &g_array_index (ticket.transient_buffers, CoglVkTransientBuffer, 0);
  g_assert_cmpmem ((void *) (uintptr_t) stored->memory, sizeof (expected),
                   expected, sizeof (expected));
  free_transient_buffers (&ticket);
  source_map_failure = FALSE;
  index_upload_test = FALSE;
}

static void
test_index_map_failure (void)
{
  CoglVkPass pass = { 0 };
  CoglBuffer source = { .size = 16 };
  g_autoptr (GError) error = NULL;

  indexed_draws = source_maps = source_unmaps = 0;
  source_map_failure = TRUE;
  g_assert_false (draw_converted_indices (&pass, &source,
                                          COGL_INDICES_TYPE_UNSIGNED_INT,
                                          1, 3, TRUE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpuint (source_maps, ==, 1);
  g_assert_cmpuint (source_unmaps, ==, 0);
  g_assert_cmpuint (indexed_draws, ==, 0);
  source_map_failure = FALSE;
}

static void
test_acquire_retry (void)
{
  CoglVkPass pass = { 0 };
  int fd = open ("/dev/null", O_RDONLY);

  g_assert_cmpint (fd, >=, 0);
  texture_acquire_fd = fd;
  acquire_import_succeeds = FALSE;
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*acquire sync wait failed*");
  g_assert_false (pass_wait_pipeline_acquire_fds (&pass, (CoglPipeline *) &pass));
  g_test_assert_expected_messages ();
  g_assert_cmpint (texture_acquire_fd, ==, fd);
  g_assert_cmpint (fcntl (fd, F_GETFD), >=, 0);
  acquire_import_succeeds = TRUE;
  g_assert_true (pass_wait_pipeline_acquire_fds (&pass, (CoglPipeline *) &pass));
  g_assert_cmpint (texture_acquire_fd, ==, -1);
  g_assert_cmpint (fcntl (fd, F_GETFD), ==, -1);
}

static void
test_recycle_unexported (void)
{
  CoglVkDevice device = {
    .has_external_semaphore_fd = TRUE,
    .vkGetSemaphoreCounterValue = get_counter,
    .fence_free = g_ptr_array_new (),
    .fence_inflight = g_ptr_array_new (),
  };
  CoglVkFenceTicket ticket = {
    .device = &device, .timeline = (VkSemaphore) 1, .point = 2,
    .binary = (VkSemaphore) 2,
  };

  destroys = creates = 0;
  completed_point = 1;
  cogl_vk_fence_ticket_release (&ticket);
  g_assert_cmpuint (device.fence_inflight->len, ==, 1);
  g_assert_cmpuint (destroys, ==, 0);
  completed_point = 2;
  cogl_vk_device_drain_fence_pool (&device);
  g_assert_cmpuint (destroys, ==, 1);
  g_assert_cmpuint (device.fence_free->len, ==, 1);
  g_assert_true (cogl_vk_fence_ticket_acquire (&device, NULL) == &ticket);
  g_assert_cmpuint (creates, ==, 1);
  g_assert_true (ticket.binary != (VkSemaphore) 2);
  g_ptr_array_unref (device.fence_free);
  g_ptr_array_unref (device.fence_inflight);
}

static void
test_recycle_allocation_failure (void)
{
  CoglVkDevice device = {
    .has_external_semaphore_fd = TRUE,
    .vkGetSemaphoreCounterValue = get_counter,
    .fence_free = g_ptr_array_new (),
    .fence_inflight = g_ptr_array_new (),
  };
  CoglVkFenceTicket *ticket = g_new0 (CoglVkFenceTicket, 1);
  const VkSemaphore borrowed_timeline = (VkSemaphore) (uintptr_t) 7;

  ticket->device = &device;
  ticket->timeline = borrowed_timeline;
  ticket->point = 3;
  ticket->wait_semaphores = g_array_new (FALSE, FALSE, sizeof (VkSemaphore));
  g_ptr_array_add (device.fence_free, ticket);
  destroys = creates = 0;
  semaphore_create_result = VK_ERROR_OUT_OF_HOST_MEMORY;

  for (int attempt = 0; attempt < 2; attempt++)
    {
      g_autoptr (GError) error = NULL;

      g_assert_null (cogl_vk_fence_ticket_acquire (&device, &error));
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
      g_assert_cmpuint (device.fence_free->len, ==, 1);
      g_assert_true (g_ptr_array_index (device.fence_free, 0) == ticket);
      g_assert_cmpuint (destroys, ==, 0);
      g_assert_true (ticket->timeline == borrowed_timeline);
      g_assert_cmpuint (ticket->point, ==, 3);
      g_assert_true (ticket->binary == VK_NULL_HANDLE);
    }

  semaphore_create_result = VK_SUCCESS;
  g_assert_true (cogl_vk_fence_ticket_acquire (&device, NULL) == ticket);
  g_assert_cmpuint (device.fence_free->len, ==, 0);
  g_assert_cmpuint (creates, ==, 3);
  g_assert_cmpuint (destroys, ==, 0);
  g_assert_true (ticket->timeline == borrowed_timeline);
  g_assert_true (ticket->binary != VK_NULL_HANDLE);
  cogl_vk_fence_ticket_free (ticket);
  g_assert_cmpuint (destroys, ==, 2);
  g_ptr_array_unref (device.fence_free);
  g_ptr_array_unref (device.fence_inflight);
}

static void
test_vertex_input_limits (void)
{
  VkPhysicalDeviceLimits limits = {
    .maxVertexInputAttributes = 32, .maxVertexInputBindings = 24,
  };
  g_autoptr (GError) error = NULL;

  g_assert_true (validate_vertex_input_capacity (&limits, 24, 32, &error));
  g_assert_no_error (error);
  g_assert_false (validate_vertex_input_capacity (&limits, 25, 32, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error (&error);
  g_assert_false (validate_vertex_input_capacity (&limits, 24, 33, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
}

static void
test_vertex_input_mapping (void)
{
  enum { COUNT = 15 };
  CoglAttribute attrs[COUNT] = {0};
  CoglAttribute *pointers[COUNT];
  CoglAttributeNameState names[COUNT] = {0};
  g_auto (CoglVkVboMask) mask = {0};
  g_auto (CoglVkVboMask) changed = {0};
  g_auto (CoglVkVboMask) integer_mask = {0};
  g_autoptr (GBytes) original_key = NULL;
  g_autoptr (GBytes) changed_key = NULL;
  VkVertexInputBindingDescription bindings[COUNT + 5];
  VkVertexInputAttributeDescription inputs[COUNT + 5];
  uint32_t n_bind, n_attr;

  for (int i = 0; i < COUNT; i++)
    {
      pointers[i] = &attrs[i];
      attrs[i].name_state = &names[i];
      attrs[i].n_components = 2;
      attrs[i].type = COGL_ATTRIBUTE_TYPE_FLOAT;
      attrs[i].stride = 8;
      names[i].name_index = i;
      names[i].name_id = COGL_ATTRIBUTE_NAME_ID_CUSTOM_ARRAY;
    }
  names[0].name_id = COGL_ATTRIBUTE_NAME_ID_POSITION_ARRAY;
  names[1].name_id = names[2].name_id = COGL_ATTRIBUTE_NAME_ID_TEXTURE_COORD_ARRAY;
  names[1].layer_number = 1000;
  names[2].layer_number = 8;
  g_assert_true (collect_mask (pointers, COUNT, &mask));
  g_assert_cmpint (mask.n_custom, ==, 12);
  g_assert_cmpint (texcoord_location (texcoord_slot (&mask, 1000)), ==, 5);
  g_assert_cmpint (texcoord_location (texcoord_slot (&mask, 8)), ==, 6);
  g_assert_cmpint (mask.custom_base, ==, 7);
  build_vi (&mask, bindings, inputs, &n_bind, &n_attr);
  g_assert_cmpuint (n_bind, ==, 19);
  g_assert_cmpuint (n_attr, ==, 19);
  for (unsigned i = 0; i < n_attr; i++)
    for (unsigned j = i + 1; j < n_attr; j++)
      g_assert_cmpuint (inputs[i].location, !=, inputs[j].location);
  original_key = vertex_layout_key (&mask);
  attrs[COUNT - 1].type = COGL_ATTRIBUTE_TYPE_SHORT;
  attrs[COUNT - 1].normalized = TRUE;
  g_assert_true (collect_mask (pointers, COUNT, &changed));
  changed_key = vertex_layout_key (&changed);
  g_assert_false (g_bytes_equal (original_key, changed_key));

  attrs[1].type = COGL_ATTRIBUTE_TYPE_UNSIGNED_SHORT;
  attrs[1].stride = 4;
  attrs[1].normalized = TRUE;
  g_assert_true (collect_mask (pointers, COUNT, &integer_mask));
  build_vi (&integer_mask, bindings, inputs, &n_bind, &n_attr);
  g_assert_cmpuint (bindings[5].stride, ==, 2 * sizeof (float));
}

int
main (int    argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/vk-sync/export-failure", test_export_fallback);
  g_test_add_func ("/vk-sync/export-signaled", test_export_signaled_fd);
  g_test_add_func ("/vk-sync/missing-counter", test_missing_counter);
  g_test_add_func ("/vk-sync/recycle-unexported", test_recycle_unexported);
  g_test_add_func ("/vk-sync/recycle-allocation-failure", test_recycle_allocation_failure);
  g_test_add_func ("/vk-sync/buffer-retirement", test_buffer_retirement);
  g_test_add_func ("/vk-sync/index-snapshots", test_index_snapshots);
  g_test_add_data_func ("/vk-index/line-loop-large-first", GINT_TO_POINTER (70000), test_line_loop);
  g_test_add_data_func ("/vk-index/line-loop-cross-u16", GINT_TO_POINTER (65535), test_line_loop);
  g_test_add_data_func ("/vk-index/line-loop-max-count", GINT_TO_POINTER (0), test_line_loop);
  g_test_add_func ("/vk-index/line-loop-wide", test_line_loop_wide);
  g_test_add_data_func ("/vk-index/converted-u8", GINT_TO_POINTER (COGL_INDICES_TYPE_UNSIGNED_BYTE), test_converted_indices);
  g_test_add_data_func ("/vk-index/converted-u16", GINT_TO_POINTER (COGL_INDICES_TYPE_UNSIGNED_SHORT), test_converted_indices);
  g_test_add_data_func ("/vk-index/converted-u32", GINT_TO_POINTER (COGL_INDICES_TYPE_UNSIGNED_INT), test_converted_indices);
  g_test_add_func ("/vk-index/ranges", test_index_ranges);
  g_test_add_func ("/vk-index/map-failure", test_index_map_failure);
  g_test_add_func ("/vk-index/premapped", test_index_premapped);
  g_test_add_func ("/vk-index/line-loop-invalid", test_line_loop_invalid);
  g_test_add_func ("/vk-sync/acquire-retry", test_acquire_retry);
  g_test_add_func ("/vk-sync/current-fd", test_export_current_fd);
  g_test_add_func ("/vk-sync/no-export-support", test_no_export_support);
  g_test_add_func ("/vk-sync/offscreen-flush-async", test_offscreen_flush_is_async);
  g_test_add_func ("/vk-sync/failed-wait", test_failed_wait_stops_presentation);
  g_test_add_func ("/vk-sync/dup-failure", test_dup_failure_waits);
  g_test_add_func ("/vk-input/device-limits", test_vertex_input_limits);
  g_test_add_func ("/vk-input/compact-mapping", test_vertex_input_mapping);
  return g_test_run ();
}
