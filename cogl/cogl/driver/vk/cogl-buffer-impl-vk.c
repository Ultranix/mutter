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

#include "cogl/driver/vk/cogl-buffer-impl-vk-private.h"
#include "cogl/driver/vk/cogl-driver-vk-private.h"
#include "cogl/driver/vk/cogl-device-vk-private.h"
#include "cogl/driver/vk/cogl-pass-vk-private.h"
#include "cogl/driver/vk/cogl-pipeline-vk-private.h"
#include "cogl/driver/vk/cogl-vk-debug-private.h"

#include "cogl/cogl-buffer-private.h"
#include "cogl/cogl-context-private.h"
#include "cogl/cogl-private.h"

#include <stdint.h>
#include <string.h>

/* Amortize HOST_VISIBLE staging + vkCmdCopyBuffer vs direct map. */
#define COGL_VK_BUFFER_DEVICE_LOCAL_MIN_BYTES 4096

struct _CoglBufferImplVk
{
  CoglBufferImpl parent_instance;

  VkBuffer buffer;
  VkDeviceMemory memory;
  gboolean device_local;

  /* HOST_VISIBLE shadow for DEVICE_LOCAL map/set_data. */
  VkBuffer staging;
  VkDeviceMemory staging_memory;

  void *mapped;
  size_t mapped_offset;
  size_t mapped_size;
  CoglBufferAccess mapped_access;
};

G_DEFINE_FINAL_TYPE (CoglBufferImplVk, cogl_buffer_impl_vk, COGL_TYPE_BUFFER_IMPL)

static CoglVkDevice *
peek_device (CoglBuffer * buffer)
{
  CoglDriver *driver = cogl_context_get_driver (buffer->context);

  if (!COGL_IS_DRIVER_VK (driver))
    return NULL;
  return cogl_driver_vk_peek_device (COGL_DRIVER_VK (driver));
}

static CoglDriverVk *
peek_driver_vk (CoglBuffer *buffer)
{
  CoglDriver *driver = cogl_context_get_driver (buffer->context);

  if (!COGL_IS_DRIVER_VK (driver))
    return NULL;
  return COGL_DRIVER_VK (driver);
}

static int
find_memory_type (CoglVkDevice         *device,
                  uint32_t              type_bits,
                  VkMemoryPropertyFlags props)
{
  VkPhysicalDeviceMemoryProperties mem_props;
  uint32_t i;

  vkGetPhysicalDeviceMemoryProperties (device->physical_device, &mem_props);
  for (i = 0; i < mem_props.memoryTypeCount; i++)
    {
      if ((type_bits & (1u << i)) &&
          (mem_props.memoryTypes[i].propertyFlags & props) == props)
        return (int) i;
    }
  return -1;
}

static gboolean
want_device_local (CoglBuffer *buffer)
{
  /* Journal attributes set DYNAMIC before map — keep host-visible for
   * frequent map/unmap. Prefer DEVICE_LOCAL only for write-once / STATIC.
   */
  if (buffer->update_hint != COGL_BUFFER_UPDATE_HINT_STATIC)
    return FALSE;

  if (buffer->size < COGL_VK_BUFFER_DEVICE_LOCAL_MIN_BYTES)
    return FALSE;

  switch (buffer->last_target)
    {
    case COGL_BUFFER_BIND_TARGET_ATTRIBUTE_BUFFER:
    case COGL_BUFFER_BIND_TARGET_PIXEL_UNPACK:
      return TRUE;
    case COGL_BUFFER_BIND_TARGET_INDEX_BUFFER:
      /* Journal may expand u8→u16 / LINE_LOOP via map while a render pass
       * is open; DEVICE_LOCAL map needs a GPU copy+submit and fails then.
       */
      return FALSE;
    case COGL_BUFFER_BIND_TARGET_PIXEL_PACK:
    default:
      /* Pack / unknown: CPU readback needs host-visible. */
      return FALSE;
    }
}

static void
destroy_vk_buffer (CoglVkDevice    *device,
                   VkBuffer        *buffer,
                   VkDeviceMemory  *memory)
{
  if (device)
    {
      /* Recording CBs that bind this buffer are invalidated on destroy;
       * WaitIdle does not cover unsubmitted recording state.
       */
      if (device->owner)
        cogl_driver_vk_flush_current_pass (device->owner);
      if ((*buffer != VK_NULL_HANDLE || *memory != VK_NULL_HANDLE) &&
          device->graphics_queue != VK_NULL_HANDLE)
        {
          VkResult res = vkQueueWaitIdle (device->graphics_queue);

          cogl_vk_device_note_vk_result (device, res);
          if (res != VK_SUCCESS && res != VK_ERROR_DEVICE_LOST)
            g_error ("Cogl Vulkan: cannot retire buffer (%d)", (int) res);
        }
      if (*buffer != VK_NULL_HANDLE)
        vkDestroyBuffer (device->device, *buffer, NULL);
      if (*memory != VK_NULL_HANDLE)
        vkFreeMemory (device->device, *memory, NULL);
    }

  *buffer = VK_NULL_HANDLE;
  *memory = VK_NULL_HANDLE;
}

static gboolean
alloc_vk_buffer (CoglVkDevice          *device,
                 VkDeviceSize           size,
                 VkBufferUsageFlags     usage,
                 VkMemoryPropertyFlags  mem_props,
                 VkBuffer              *out_buffer,
                 VkDeviceMemory        *out_memory,
                 GError               **error)
{
  VkBufferCreateInfo info = {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size = size,
    .usage = usage,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  VkMemoryRequirements reqs;
  VkMemoryAllocateInfo alloc = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
  };
  VkResult res;
  int mem_type;

  *out_buffer = VK_NULL_HANDLE;
  *out_memory = VK_NULL_HANDLE;

  res = vkCreateBuffer (device->device, &info, NULL, out_buffer);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkCreateBuffer failed (%d)", (int) res);
      return FALSE;
    }

  vkGetBufferMemoryRequirements (device->device, *out_buffer, &reqs);
  mem_type = find_memory_type (device, reqs.memoryTypeBits, mem_props);
  if (mem_type < 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "No suitable memory type for VkBuffer");
      destroy_vk_buffer (device, out_buffer, out_memory);
      return FALSE;
    }

  alloc.allocationSize = reqs.size;
  alloc.memoryTypeIndex = (uint32_t) mem_type;
  res = vkAllocateMemory (device->device, &alloc, NULL, out_memory);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkAllocateMemory failed (%d)", (int) res);
      destroy_vk_buffer (device, out_buffer, out_memory);
      return FALSE;
    }

  res = vkBindBufferMemory (device->device, *out_buffer, *out_memory, 0);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkBindBufferMemory failed (%d)", (int) res);
      destroy_vk_buffer (device, out_buffer, out_memory);
      return FALSE;
    }

  return TRUE;
}

static gboolean
flush_current_pass_for_copy (CoglDriverVk *driver_vk,
                             GError      **error)
{
  if (driver_vk && (driver_vk->submission_failed || driver_vk->vk_device->device_lost))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Vulkan frame submission has failed");
      return FALSE;
    }

  if (!driver_vk || !driver_vk->current_pass)
    return TRUE;

  if (!cogl_vk_pass_submit (driver_vk->current_pass, error))
    {
      g_clear_pointer (&driver_vk->current_pass, cogl_vk_pass_free);
      return FALSE;
    }

  cogl_driver_vk_store_sync_fd_from_pass (driver_vk);
  g_clear_pointer (&driver_vk->current_pass, cogl_vk_pass_free);
  return TRUE;
}

/* One-shot vkCmdCopyBuffer + fence wait (same pattern as mip upload). */
static gboolean
copy_buffers_oneshot (CoglBuffer      *buffer,
                      VkBuffer         src,
                      VkBuffer         dst,
                      VkDeviceSize     offset,
                      VkDeviceSize     size,
                      gboolean         to_device,
                      GError         **error)
{
  CoglVkDevice *device = peek_device (buffer);
  CoglDriverVk *driver_vk = peek_driver_vk (buffer);
  VkCommandBufferAllocateInfo cb_alloc = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
  };
  VkCommandBufferBeginInfo begin_info = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  VkFenceCreateInfo fence_info = {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
  };
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkSubmitInfo submit_info;
  VkBufferCopy region;
  VkBufferMemoryBarrier barrier;
  VkResult res;
  gboolean ok = FALSE;

  if (!device)
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR,
                           COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "Vulkan device missing for buffer copy");
      return FALSE;
    }

  if (!flush_current_pass_for_copy (driver_vk, error))
    return FALSE;

  cb_alloc.commandPool = device->command_pool;
  cb_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cb_alloc.commandBufferCount = 1;
  res = vkAllocateCommandBuffers (device->device, &cb_alloc, &cmd);
  if (res != VK_SUCCESS)
    goto out;

  res = vkBeginCommandBuffer (cmd, &begin_info);
  if (res != VK_SUCCESS)
    goto out;

  memset (&region, 0, sizeof (region));
  region.srcOffset = offset;
  region.dstOffset = offset;
  region.size = size;
  vkCmdCopyBuffer (cmd, src, dst, 1, &region);

  memset (&barrier, 0, sizeof (barrier));
  barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = dst;
  barrier.offset = offset;
  barrier.size = size;

  if (to_device)
    {
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                              VK_ACCESS_INDEX_READ_BIT |
                              VK_ACCESS_TRANSFER_READ_BIT;
      vkCmdPipelineBarrier (cmd,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 0, NULL, 1, &barrier, 0, NULL);
    }
  else
    {
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      vkCmdPipelineBarrier (cmd,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_HOST_BIT,
                            0, 0, NULL, 1, &barrier, 0, NULL);
    }

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
    {
      cogl_vk_device_note_vk_result (device, res);
      goto out;
    }

  res = vkWaitForFences (device->device, 1, &fence, VK_TRUE, UINT64_MAX);
  if (res != VK_SUCCESS)
    {
      cogl_vk_device_note_vk_result (device, res);
      goto out;
    }

  ok = TRUE;

out:
  if (fence != VK_NULL_HANDLE)
    vkDestroyFence (device->device, fence, NULL);
  if (cmd != VK_NULL_HANDLE)
    vkFreeCommandBuffers (device->device, device->command_pool, 1, &cmd);

  if (!ok && error && !*error)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Vulkan buffer staging copy failed");
  return ok;
}

static gboolean host_write_barrier_oneshot (CoglBuffer   *buffer,
                                            VkDeviceSize  offset,
                                            VkDeviceSize  size,
                                            GError      **error);

/* HOST_COHERENT still needs a dependency for GPU vertex/index reads
 * after prior queue work. Prefer recording into the open pass; only
 * fall back to a oneshot submit when no pass is recording (flushing the
 * current pass mid-frame was dropping earlier draws).
 */
static gboolean
host_write_barrier (CoglBuffer   *buffer,
                    VkDeviceSize  offset,
                    VkDeviceSize  size,
                    GError      **error)
{
  CoglVkDevice *device = peek_device (buffer);
  CoglDriverVk *driver_vk = peek_driver_vk (buffer);
  CoglBufferImplVk *vk_impl = COGL_BUFFER_IMPL_VK (buffer->impl);
  VkBufferMemoryBarrier barrier;
  VkCommandBuffer cmd;

  if (!device || vk_impl->buffer == VK_NULL_HANDLE)
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR,
                           COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "Vulkan device/buffer missing for host barrier");
      return FALSE;
    }

  memset (&barrier, 0, sizeof (barrier));
  barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                          VK_ACCESS_INDEX_READ_BIT |
                          VK_ACCESS_UNIFORM_READ_BIT |
                          VK_ACCESS_TRANSFER_READ_BIT;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = vk_impl->buffer;
  barrier.offset = offset;
  barrier.size = size;

  if (driver_vk && driver_vk->current_pass &&
      driver_vk->current_pass->command_buffer != VK_NULL_HANDLE)
    {
      /* Must be outside a render pass for global buffer barriers. */
      cogl_vk_pass_end_render (driver_vk->current_pass);
      cmd = driver_vk->current_pass->command_buffer;
      cogl_vk_debug_log ("buf",
                         "host_barrier INLINE pass=%p buf=%p off=%llu sz=%llu",
                         (void *) driver_vk->current_pass,
                         (void *) buffer,
                         (unsigned long long) offset,
                         (unsigned long long) size);
      vkCmdPipelineBarrier (cmd,
                            VK_PIPELINE_STAGE_HOST_BIT,
                            VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 0, NULL, 1, &barrier, 0, NULL);
      return TRUE;
    }

  return host_write_barrier_oneshot (buffer, offset, size, error);
}

static gboolean
host_write_barrier_oneshot (CoglBuffer   *buffer,
                            VkDeviceSize  offset,
                            VkDeviceSize  size,
                            GError      **error)
{
  CoglVkDevice *device = peek_device (buffer);
  CoglDriverVk *driver_vk = peek_driver_vk (buffer);
  CoglBufferImplVk *vk_impl = COGL_BUFFER_IMPL_VK (buffer->impl);
  VkCommandBufferAllocateInfo cb_alloc = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
  };
  VkCommandBufferBeginInfo begin_info = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  VkFenceCreateInfo fence_info = {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
  };
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkSubmitInfo submit_info;
  VkBufferMemoryBarrier barrier;
  VkResult res;
  gboolean ok = FALSE;

  if (!device || vk_impl->buffer == VK_NULL_HANDLE)
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR,
                           COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "Vulkan device/buffer missing for host barrier");
      return FALSE;
    }

  if (!flush_current_pass_for_copy (driver_vk, error))
    return FALSE;

  cogl_vk_debug_log ("buf",
                     "host_barrier buf=%p vk=%p off=%llu size=%llu "
                     "pass_after_flush=%p",
                     (void *) buffer,
                     (void *) (uintptr_t) vk_impl->buffer,
                     (unsigned long long) offset,
                     (unsigned long long) size,
                     driver_vk ? (void *) driver_vk->current_pass : NULL);

  cb_alloc.commandPool = device->command_pool;
  cb_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cb_alloc.commandBufferCount = 1;
  res = vkAllocateCommandBuffers (device->device, &cb_alloc, &cmd);
  if (res != VK_SUCCESS)
    goto out;

  res = vkBeginCommandBuffer (cmd, &begin_info);
  if (res != VK_SUCCESS)
    goto out;

  memset (&barrier, 0, sizeof (barrier));
  barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                          VK_ACCESS_INDEX_READ_BIT |
                          VK_ACCESS_UNIFORM_READ_BIT |
                          VK_ACCESS_TRANSFER_READ_BIT;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = vk_impl->buffer;
  barrier.offset = offset;
  barrier.size = size;
  vkCmdPipelineBarrier (cmd,
                        VK_PIPELINE_STAGE_HOST_BIT,
                        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        0, 0, NULL, 1, &barrier, 0, NULL);

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
    {
      cogl_vk_device_note_vk_result (device, res);
      goto out;
    }

  res = vkWaitForFences (device->device, 1, &fence, VK_TRUE, UINT64_MAX);
  if (res != VK_SUCCESS)
    {
      cogl_vk_device_note_vk_result (device, res);
      goto out;
    }

  ok = TRUE;

out:
  if (fence != VK_NULL_HANDLE)
    vkDestroyFence (device->device, fence, NULL);
  if (cmd != VK_NULL_HANDLE)
    vkFreeCommandBuffers (device->device, device->command_pool, 1, &cmd);

  if (!ok && error && !*error)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Vulkan host buffer barrier failed");
  return ok;
}

static gboolean
ensure_staging (CoglBufferImplVk *impl,
                CoglBuffer       *buffer,
                GError          **error)
{
  CoglVkDevice *device;

  if (impl->staging != VK_NULL_HANDLE)
    return TRUE;

  device = peek_device (buffer);
  if (!device)
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR,
                           COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "Vulkan device missing for buffer staging");
      return FALSE;
    }

  return alloc_vk_buffer (device, buffer->size,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &impl->staging, &impl->staging_memory, error);
}

static gboolean
ensure_store (CoglBufferImplVk *impl,
              CoglBuffer       *buffer,
              GError          **error)
{
  CoglVkDevice *device;
  VkBufferUsageFlags usage =
    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
    VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
    VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  gboolean prefer_device_local;
  GError *local_error = NULL;

  if (impl->buffer != VK_NULL_HANDLE)
    return TRUE;

  device = peek_device (buffer);
  if (!device)
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR,
                           COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "Vulkan device missing for buffer");
      return FALSE;
    }

  if (buffer->size == 0)
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR,
                           COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "Zero-sized Vulkan buffer");
      return FALSE;
    }

  prefer_device_local = want_device_local (buffer);

  if (prefer_device_local)
    {
      if (alloc_vk_buffer (device, buffer->size, usage,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                           &impl->buffer, &impl->memory, &local_error))
        {
          impl->device_local = TRUE;
        }
      else
        {
          /* Fall back to host-visible if DEVICE_LOCAL is unavailable. */
          g_clear_error (&local_error);
          prefer_device_local = FALSE;
        }
    }

  if (!prefer_device_local)
    {
      if (!alloc_vk_buffer (device, buffer->size, usage,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            &impl->buffer, &impl->memory, error))
        return FALSE;
      impl->device_local = FALSE;
    }

  buffer->store_created = TRUE;
  buffer->flags |= COGL_BUFFER_FLAG_BUFFER_OBJECT;
  return TRUE;
}

VkBuffer
cogl_buffer_impl_vk_peek_buffer (CoglBufferImplVk *impl)
{
  g_return_val_if_fail (COGL_IS_BUFFER_IMPL_VK (impl), VK_NULL_HANDLE);
  return impl->buffer;
}

gboolean
cogl_buffer_impl_vk_ensure (CoglBufferImplVk *impl,
                            CoglBuffer       *buffer,
                            GError          **error)
{
  g_return_val_if_fail (COGL_IS_BUFFER_IMPL_VK (impl), FALSE);
  return ensure_store (impl, buffer, error);
}

static void
cogl_buffer_impl_vk_create (CoglBufferImpl *impl,
                            CoglBuffer     *buffer)
{
  (void) impl;
  (void) buffer;
  /* Lazily allocate on first map/set_data (GL recreate_store pattern). */
}

static void
cogl_buffer_impl_vk_destroy (CoglBufferImpl *impl,
                             CoglBuffer     *buffer)
{
  CoglBufferImplVk *vk_impl = COGL_BUFFER_IMPL_VK (impl);
  CoglVkDevice *device = peek_device (buffer);

  if (vk_impl->mapped)
    {
      if (device)
        {
          if (vk_impl->device_local)
            vkUnmapMemory (device->device, vk_impl->staging_memory);
          else
            vkUnmapMemory (device->device, vk_impl->memory);
        }
      vk_impl->mapped = NULL;
    }

  destroy_vk_buffer (device, &vk_impl->staging, &vk_impl->staging_memory);
  destroy_vk_buffer (device, &vk_impl->buffer, &vk_impl->memory);
  vk_impl->device_local = FALSE;
  buffer->store_created = FALSE;
}

static void *
cogl_buffer_impl_vk_map_range (CoglBufferImpl     *impl,
                               CoglBuffer         *buffer,
                               size_t              offset,
                               size_t              size,
                               CoglBufferAccess    access,
                               CoglBufferMapHint   hints,
                               GError            **error)
{
  CoglBufferImplVk *vk_impl = COGL_BUFFER_IMPL_VK (impl);
  CoglVkDevice *device;
  void *ptr;
  VkResult res;

  if (!ensure_store (vk_impl, buffer, error))
    return NULL;

  device = peek_device (buffer);

  if (!vk_impl->device_local)
    {
      res = vkMapMemory (device->device, vk_impl->memory, offset, size, 0, &ptr);
      if (res != VK_SUCCESS)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "vkMapMemory failed (%d)", (int) res);
          return NULL;
        }

      vk_impl->mapped = ptr;
      vk_impl->mapped_offset = offset;
      vk_impl->mapped_size = size;
      vk_impl->mapped_access = access;
      buffer->flags |= COGL_BUFFER_FLAG_MAPPED;
      return ptr;
    }

  /* DEVICE_LOCAL: shadow-map via HOST_VISIBLE staging. */
  if (!ensure_staging (vk_impl, buffer, error))
    return NULL;

  if ((access & COGL_BUFFER_ACCESS_READ) &&
      !(hints & (COGL_BUFFER_MAP_HINT_DISCARD |
                 COGL_BUFFER_MAP_HINT_DISCARD_RANGE)))
    {
      if (!copy_buffers_oneshot (buffer,
                                 vk_impl->buffer, vk_impl->staging,
                                 offset, size, FALSE, error))
        return NULL;
    }

  res = vkMapMemory (device->device, vk_impl->staging_memory,
                     offset, size, 0, &ptr);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkMapMemory (staging) failed (%d)", (int) res);
      return NULL;
    }

  vk_impl->mapped = ptr;
  vk_impl->mapped_offset = offset;
  vk_impl->mapped_size = size;
  vk_impl->mapped_access = access;
  buffer->flags |= COGL_BUFFER_FLAG_MAPPED;
  return ptr;
}

static void
cogl_buffer_impl_vk_unmap (CoglBufferImpl *impl,
                           CoglBuffer     *buffer)
{
  CoglBufferImplVk *vk_impl = COGL_BUFFER_IMPL_VK (impl);
  CoglVkDevice *device = peek_device (buffer);
  size_t offset = vk_impl->mapped_offset;
  size_t size = vk_impl->mapped_size;
  CoglBufferAccess access = vk_impl->mapped_access;
  g_autoptr (GError) error = NULL;

  if (!vk_impl->mapped)
    return;

  if (device)
    {
      if (vk_impl->device_local)
        vkUnmapMemory (device->device, vk_impl->staging_memory);
      else
        vkUnmapMemory (device->device, vk_impl->memory);
    }

  vk_impl->mapped = NULL;
  vk_impl->mapped_offset = 0;
  vk_impl->mapped_size = 0;
  vk_impl->mapped_access = 0;
  buffer->flags &= ~COGL_BUFFER_FLAG_MAPPED;

  if (!(access & COGL_BUFFER_ACCESS_WRITE) || !device)
    return;

  if (vk_impl->device_local)
    {
      /* Staging → DEVICE_LOCAL; ignore errors to match GL unmap void API. */
      if (!copy_buffers_oneshot (buffer,
                                 vk_impl->staging, vk_impl->buffer,
                                 offset, size, TRUE, &error))
        g_debug ("Cogl Vulkan: buffer staging upload failed: %s",
                 error->message);
    }
  else
    {
      /* HOST_COHERENT still needs a dependency for GPU vertex/index reads
       * after prior queue work (primitive 2nd VBO otherwise stays black).
       */
      if (!host_write_barrier (buffer, offset, size, &error))
        g_debug ("Cogl Vulkan: host buffer barrier failed: %s",
                 error->message);
    }
}

static gboolean
cogl_buffer_impl_vk_set_data (CoglBufferImpl  *impl,
                              CoglBuffer      *buffer,
                              unsigned int     offset,
                              const void      *data,
                              unsigned int     size,
                              GError         **error)
{
  CoglBufferImplVk *vk_impl = COGL_BUFFER_IMPL_VK (impl);
  CoglVkDevice *device;
  void *mapped;
  VkResult res;

  if (!ensure_store (vk_impl, buffer, error))
    return FALSE;

  if (!vk_impl->device_local)
    {
      mapped = cogl_buffer_impl_vk_map_range (impl, buffer, offset, size,
                                              COGL_BUFFER_ACCESS_WRITE,
                                              COGL_BUFFER_MAP_HINT_DISCARD_RANGE,
                                              error);
      if (!mapped)
        return FALSE;

      memcpy (mapped, data, size);
      cogl_buffer_impl_vk_unmap (impl, buffer);
      return TRUE;
    }

  /* DEVICE_LOCAL: write staging then one-shot copy (report upload errors). */
  if (!ensure_staging (vk_impl, buffer, error))
    return FALSE;

  device = peek_device (buffer);
  res = vkMapMemory (device->device, vk_impl->staging_memory,
                     offset, size, 0, &mapped);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkMapMemory (staging) failed (%d)", (int) res);
      return FALSE;
    }

  memcpy (mapped, data, size);
  vkUnmapMemory (device->device, vk_impl->staging_memory);

  return copy_buffers_oneshot (buffer,
                               vk_impl->staging, vk_impl->buffer,
                               offset, size, TRUE, error);
}

static void
cogl_buffer_impl_vk_class_init (CoglBufferImplVkClass *klass)
{
  CoglBufferImplClass *impl_klass = COGL_BUFFER_IMPL_CLASS (klass);

  impl_klass->create = cogl_buffer_impl_vk_create;
  impl_klass->destroy = cogl_buffer_impl_vk_destroy;
  impl_klass->map_range = cogl_buffer_impl_vk_map_range;
  impl_klass->unmap = cogl_buffer_impl_vk_unmap;
  impl_klass->set_data = cogl_buffer_impl_vk_set_data;
}

static void
cogl_buffer_impl_vk_init (CoglBufferImplVk *impl)
{
  impl->buffer = VK_NULL_HANDLE;
  impl->memory = VK_NULL_HANDLE;
  impl->device_local = FALSE;
  impl->staging = VK_NULL_HANDLE;
  impl->staging_memory = VK_NULL_HANDLE;
  impl->mapped = NULL;
  impl->mapped_access = 0;
}
