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

/* Vulkan instance, device and graphics queue used by the Cogl driver.
 * Mutter native backend manages KMS presentation.
 *
 * Submissions use timeline semaphores when available. Normal rendering does
 * not wait for the graphics queue to become idle.
 */

#pragma once

#include "cogl/cogl-driver.h"

#include <glib.h>
#include <vulkan/vulkan.h>

G_BEGIN_DECLS

typedef struct _CoglVkDevice CoglVkDevice;
typedef struct _CoglVkFenceTicket CoglVkFenceTicket;
typedef struct _CoglDriverVk CoglDriverVk;

#define COGL_VK_TIMESTAMP_QUERY_COUNT 64

/* Function signature usable with headers lacking VK_EXT_device_fault. */
typedef VkResult (VKAPI_PTR *CoglVkGetDeviceFaultInfoEXT) (VkDevice device,
                                                           void     *pFaultCounts,
                                                           void     *pFaultInfo);

struct _CoglVkDevice
{
  VkInstance instance;
  VkPhysicalDevice physical_device;
  VkPhysicalDeviceLimits limits;
  VkDevice device;
  uint32_t graphics_queue_family;
  VkQueue graphics_queue;

  /* Borrowed driver pointer used to finish the current pass before destroying
   * images or views referenced by its command buffer.
   */
  CoglDriverVk *owner;

  /* Optional DRM render/primary node fd used to pick the same GPU as KMS
   * (-1 = unset; first graphics device wins).
   */
  int drm_fd;

  /* Command pool shared by rendering submissions on this device. */
  VkCommandPool command_pool;
  /* Separate pool for out-of-band oneshot clear/copy submits. Never allocate
   * oneshot CBs from command_pool while a frame CB is recording — ANV can
   * SIGSEGV in vkCmdEndRenderPass (meson MALLOC_PERTURB).
   */
  VkCommandPool oneshot_command_pool;

  /* VBO pipelines and vertex snapshots belong to this VkDevice. */
  GHashTable *vbo_pipes;
  VkBuffer vbo_dummy_buf;
  VkDeviceMemory vbo_dummy_mem;
  VkBuffer vbo_float_shadow_buf;
  VkDeviceMemory vbo_float_shadow_mem;
  size_t vbo_float_shadow_cap;
  size_t vbo_float_shadow_used;
  GPtrArray *vbo_float_shadow_retired_buf;
  GPtrArray *vbo_float_shadow_retired_mem;

  gboolean has_dma_buf;
  gboolean has_drm_modifiers;
  gboolean has_external_semaphore_fd;
  gboolean has_timeline_semaphore;
  gboolean has_queue_family_foreign;
  gboolean has_sampler_ycbcr;
  /* timestampPeriod > 0 and queue timestampValidBits > 0. */
  gboolean has_timestamp_query;
  /* Whether device-loss diagnostics can query VK_EXT_device_fault. */
  gboolean has_device_fault;
  /* VK_KHR_push_descriptor: bind the hot texture sampler straight into the
   * command buffer, skipping per-draw vkAllocateDescriptorSets/Update.
   */
  gboolean has_push_descriptor;

  /* Nanoseconds per timestamp tick (VkPhysicalDeviceLimits::timestampPeriod).
   */
  float timestamp_period;
  uint32_t timestamp_valid_bits;
  VkQueryPool timestamp_pool;
  uint32_t timestamp_next; /* ring index into timestamp_pool */
  /* Last submitted pass begin/end query indices (-1 = none). Debug only. */
  int last_timestamp_begin;
  int last_timestamp_end;

  /* Sticky: set on VK_ERROR_DEVICE_LOST (submit / fence / export). */
  gboolean device_lost;
  CoglGraphicsResetStatus reset_status;

  /* Available tickets and submissions awaiting completion. */
  GPtrArray *fence_free;     /* CoglVkFenceTicket * */
  GPtrArray *fence_inflight; /* CoglVkFenceTicket * */

  PFN_vkGetMemoryFdKHR vkGetMemoryFdKHR;
  PFN_vkGetMemoryFdPropertiesKHR vkGetMemoryFdPropertiesKHR;
  PFN_vkGetSemaphoreFdKHR vkGetSemaphoreFdKHR;
  PFN_vkImportSemaphoreFdKHR vkImportSemaphoreFdKHR;
  PFN_vkGetFenceFdKHR vkGetFenceFdKHR;
  /* Core 1.2 / KHR_timeline_semaphore — must not call the libvulkan stub
   * (can be NULL); always resolve via vkGetDeviceProcAddr.
   */
  PFN_vkGetSemaphoreCounterValue vkGetSemaphoreCounterValue;
  PFN_vkGetPhysicalDeviceImageFormatProperties2
    vkGetPhysicalDeviceImageFormatProperties2;
  CoglVkGetDeviceFaultInfoEXT vkGetDeviceFaultInfoEXT;
  /* VK_KHR_push_descriptor; NULL when the extension is absent. */
  PFN_vkCmdPushDescriptorSetKHR vkCmdPushDescriptorSetKHR;
};

CoglVkDevice * cogl_vk_device_new (int      drm_fd,
                                   GError **error);

void cogl_vk_device_free (CoglVkDevice *device);

/* Drain inflight tickets whose timeline has signaled into fence_free. */
void cogl_vk_device_drain_fence_pool (CoglVkDevice *device);

/* Record DEVICE_LOST (and optional device_fault note) for reset-status. */
void cogl_vk_device_note_vk_result (CoglVkDevice *device,
                                    VkResult      res);

CoglGraphicsResetStatus
cogl_vk_device_get_graphics_reset_status (CoglVkDevice *device);

/* Write a GPU timestamp; returns query index or -1 if unsupported. */
int cogl_vk_device_cmd_write_timestamp (CoglVkDevice           *device,
                                        VkCommandBuffer         cmd,
                                        VkPipelineStageFlagBits stage);

/* Non-blocking read; FALSE if unavailable / unsupported. No CPU fence wait. */
gboolean cogl_vk_device_get_timestamp_ns (CoglVkDevice  *device,
                                          uint32_t       query_index,
                                          uint64_t      *out_ns);

/* Debug: last pass begin/end query indices; optional non-blocking ns (NULL
 * ok).
 * Returns FALSE if no timestamps recorded; never waits on GPU.
 */
gboolean cogl_vk_device_peek_last_timestamps (CoglVkDevice  *device,
                                              int           *out_begin_index,
                                              int           *out_end_index,
                                              uint64_t      *out_begin_ns,
                                              uint64_t      *out_end_ns);

G_END_DECLS
