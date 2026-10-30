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

/* Real Vulkan execution of production pass/fence code, without DRM or WSI.
 * Only non-rendering Cogl services are stubbed; no Vulkan calls are mocked.
 */
#include "config.h"
#include "cogl/driver/vk/cogl-pass-vk.c"
#include "cogl/driver/vk/cogl-fence-vk.c"

static int validation_errors;

void cogl_vk_debug_log (const char *channel,
                        const char *format,
                        ...) {
}
void cogl_vk_device_note_vk_result (CoglVkDevice *device,
                                    VkResult      result) {
}
int cogl_vk_device_cmd_write_timestamp (CoglVkDevice           *device,
                                        VkCommandBuffer         cmd,
                                        VkPipelineStageFlagBits stage)
{
  return -1;
}
void cogl_vk_pass_end_render (CoglVkPass *pass)
{
  g_assert_false (pass->in_render_pass);
}
void cogl_vk_image_cmd_foreign_release (CoglVkImage    *image,
                                        VkCommandBuffer cmd,
                                        uint32_t        family,
                                        VkImageLayout   layout)
{
  g_assert_not_reached ();
}
CoglVkPipelineCache * cogl_driver_vk_peek_pipeline_cache (CoglDriverVk *driver)
{
  g_assert_not_reached ();
}
void cogl_vk_pipeline_cache_note_submit (CoglVkPipelineCache *cache,
                                         VkSemaphore          timeline,
                                         uint64_t             point)
{
  g_assert_not_reached ();
}

void cogl_vk_pipeline_cache_discard_recording (CoglVkPipelineCache *cache)
{
  g_assert_not_reached ();
}

static VKAPI_ATTR VkBool32 VKAPI_CALL
debug_message (VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
               VkDebugUtilsMessageTypeFlagsEXT             type,
               const VkDebugUtilsMessengerCallbackDataEXT *data,
               void                                       *user)
{
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
      g_atomic_int_inc (&validation_errors);
      g_printerr ("VALIDATION: %s\n", data->pMessage);
    }
  return VK_FALSE;
}


int
main (int    argc,
      char **argv)
{
  CoglVkDevice device = { 0 };
  const char *layer = "VK_LAYER_KHRONOS_validation";
  const char *extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
  VkValidationFeatureEnableEXT sync =
    VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
  VkValidationFeaturesEXT validation = {
    .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
    .enabledValidationFeatureCount = 1, .pEnabledValidationFeatures = &sync,
  };
  VkDebugUtilsMessengerCreateInfoEXT debug = {
    .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
    .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
    .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                   VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
    .pfnUserCallback = debug_message,
  };
  VkApplicationInfo app = {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = "Mutter software Vulkan regression",
    .apiVersion = VK_API_VERSION_1_2,
  };
  VkInstanceCreateInfo instance_info = {
    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    .pNext = &validation, .pApplicationInfo = &app,
    .enabledLayerCount = 1, .ppEnabledLayerNames = &layer,
    .enabledExtensionCount = 1, .ppEnabledExtensionNames = &extension,
  };
  VkDebugUtilsMessengerEXT messenger;
  VkPhysicalDevice physical[16];
  uint32_t n_physical = G_N_ELEMENTS (physical);
  VkQueueFamilyProperties queues[32];
  uint32_t n_queues = G_N_ELEMENTS (queues);
  float priority = 1.f;
  VkDeviceQueueCreateInfo queue_info = {
    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    .queueCount = 1, .pQueuePriorities = &priority,
  };
  VkPhysicalDeviceTimelineSemaphoreFeatures features = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
    .timelineSemaphore = VK_TRUE,
  };
  VkDeviceCreateInfo device_info = {
    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    .pNext = &features, .queueCreateInfoCount = 1,
    .pQueueCreateInfos = &queue_info,
  };
  VkCommandPoolCreateInfo pool = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
  };
  VkBufferCreateInfo buffer_info = {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size = 256 * 32, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
  };
  VkBuffer buffer;
  VkDeviceMemory memory;
  VkMemoryRequirements requirements;
  VkPhysicalDeviceMemoryProperties memory_properties;
  VkMemoryAllocateInfo allocation = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
  };
  uint32_t *mapped;
  gboolean negative = argc == 2 && g_str_equal (argv[1], "--negative-control");

  validation.pNext = &debug;
  g_assert_cmpint (vkCreateInstance (&instance_info, NULL, &device.instance), ==, VK_SUCCESS);
  PFN_vkCreateDebugUtilsMessengerEXT create_debug =
    (PFN_vkCreateDebugUtilsMessengerEXT) vkGetInstanceProcAddr (device.instance, "vkCreateDebugUtilsMessengerEXT");
  PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug =
    (PFN_vkDestroyDebugUtilsMessengerEXT) vkGetInstanceProcAddr (device.instance, "vkDestroyDebugUtilsMessengerEXT");
  g_assert_nonnull (create_debug);
  g_assert_cmpint (create_debug (device.instance, &debug, NULL, &messenger), ==, VK_SUCCESS);
  g_assert_cmpint (vkEnumeratePhysicalDevices (device.instance, &n_physical, physical), ==, VK_SUCCESS);
  g_assert_cmpuint (n_physical, >, 0);
  device.physical_device = physical[0];
  VkPhysicalDeviceProperties properties;
  vkGetPhysicalDeviceProperties (device.physical_device, &properties);
  g_print ("Real Vulkan device: %s\n", properties.deviceName);
  vkGetPhysicalDeviceQueueFamilyProperties (device.physical_device, &n_queues, queues);
  for (queue_info.queueFamilyIndex = 0; queue_info.queueFamilyIndex < n_queues;
       queue_info.queueFamilyIndex++)
    if (queues[queue_info.queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT)
      break;
  g_assert_cmpuint (queue_info.queueFamilyIndex, <, n_queues);
  g_assert_cmpint (vkCreateDevice (device.physical_device, &device_info, NULL, &device.device), ==, VK_SUCCESS);
  vkGetDeviceQueue (device.device, queue_info.queueFamilyIndex, 0, &device.graphics_queue);
  device.graphics_queue_family = queue_info.queueFamilyIndex;
  pool.queueFamilyIndex = queue_info.queueFamilyIndex;
  g_assert_cmpint (vkCreateCommandPool (device.device, &pool, NULL, &device.command_pool), ==, VK_SUCCESS);
  device.has_timeline_semaphore = TRUE;
  device.vkGetSemaphoreCounterValue = vkGetSemaphoreCounterValue;
  device.fence_free = g_ptr_array_new_with_free_func ((GDestroyNotify) cogl_vk_fence_ticket_free);
  device.fence_inflight = g_ptr_array_new_with_free_func ((GDestroyNotify) cogl_vk_fence_ticket_free);
  g_assert_cmpint (vkCreateBuffer (device.device, &buffer_info, NULL, &buffer), ==, VK_SUCCESS);
  vkGetBufferMemoryRequirements (device.device, buffer, &requirements);
  vkGetPhysicalDeviceMemoryProperties (device.physical_device, &memory_properties);
  for (allocation.memoryTypeIndex = 0; allocation.memoryTypeIndex < memory_properties.memoryTypeCount;
       allocation.memoryTypeIndex++)
    if ((requirements.memoryTypeBits & (1u << allocation.memoryTypeIndex)) &&
        (memory_properties.memoryTypes[allocation.memoryTypeIndex].propertyFlags &
         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
      break;
  g_assert_cmpuint (allocation.memoryTypeIndex, <, memory_properties.memoryTypeCount);
  allocation.allocationSize = requirements.size;
  g_assert_cmpint (vkAllocateMemory (device.device, &allocation, NULL, &memory), ==, VK_SUCCESS);
  g_assert_cmpint (vkBindBufferMemory (device.device, buffer, memory, 0), ==, VK_SUCCESS);
  g_assert_cmpint (vkMapMemory (device.device, memory, 0, VK_WHOLE_SIZE, 0, (void **) &mapped), ==, VK_SUCCESS);

  for (unsigned int round = 0; round < (negative ? 1 : 16); round++)
    {
      for (unsigned int i = 0; i < 32; i++)
        {
          g_autoptr (GError) error = NULL;
          CoglVkPass *pass;
          VkMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
          };

          pass = cogl_vk_pass_begin (&device, &error);
          g_assert_no_error (error);
          g_assert_nonnull (pass);
          vkCmdFillBuffer (pass->command_buffer, buffer, i * 256, 256, round * 32 + i);
          if (negative && i == 0)
            vkCmdFillBuffer (pass->command_buffer, buffer, 0, 256, 0);
          vkCmdPipelineBarrier (pass->command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier,
                                0, NULL, 0, NULL);
          g_assert_true (cogl_vk_pass_submit (pass, &error));
          g_assert_no_error (error);
          cogl_vk_pass_free (pass);
        }
      int64_t deadline = g_get_monotonic_time () + 30 * G_TIME_SPAN_SECOND;
      while (device.fence_inflight->len)
        {
          cogl_vk_device_drain_fence_pool (&device);
          g_assert_cmpint (g_get_monotonic_time (), <, deadline);
          g_usleep (100);
        }
      for (unsigned int i = 0; i < 32 * 64; i++)
        g_assert_cmpuint (mapped[i], ==, round * 32 + i / 64);
    }

  g_assert_cmpint (vkDeviceWaitIdle (device.device), ==, VK_SUCCESS);
  vkUnmapMemory (device.device, memory);
  vkDestroyBuffer (device.device, buffer, NULL);
  vkFreeMemory (device.device, memory, NULL);
  g_ptr_array_unref (device.fence_inflight);
  g_ptr_array_unref (device.fence_free);
  vkDestroyCommandPool (device.device, device.command_pool, NULL);
  vkDestroyDevice (device.device, NULL);
  destroy_debug (device.instance, messenger, NULL);
  vkDestroyInstance (device.instance, NULL);
  if (negative)
    g_assert_cmpint (g_atomic_int_get (&validation_errors), >, 0);
  else
    g_assert_cmpint (g_atomic_int_get (&validation_errors), ==, 0);
  g_print ("PASS: %s\n", negative ? "negative control detected a hazard" :
           "512 production pass submissions, readback and ticket recycling; zero validation errors");
  return 0;
}
