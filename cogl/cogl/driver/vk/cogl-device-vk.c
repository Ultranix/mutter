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

#include "cogl/driver/vk/cogl-pipeline-vk-vbo-private.h"

#include "cogl/driver/vk/cogl-device-vk-private.h"
#include "cogl/driver/vk/cogl-fence-vk-private.h"

#include "cogl/cogl-debug.h"

#include <gio/gio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#ifndef VK_EXT_DEVICE_FAULT_EXTENSION_NAME
#define VK_EXT_DEVICE_FAULT_EXTENSION_NAME "VK_EXT_device_fault"
#endif
#ifndef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT \
  ((VkStructureType) 1000341000)
#endif
#ifndef VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT
#define VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT ((VkStructureType) 1000341001)
#endif

/* Soft stubs when headers predate VK_EXT_device_fault. */
#if !defined (VK_EXT_device_fault)
typedef struct VkPhysicalDeviceFaultFeaturesEXT
{
  VkStructureType sType;
  void *pNext;
  VkBool32 deviceFault;
  VkBool32 deviceFaultVendorBinary;
} VkPhysicalDeviceFaultFeaturesEXT;

typedef struct VkDeviceFaultCountsEXT
{
  VkStructureType sType;
  void *pNext;
  uint32_t addressInfoCount;
  uint32_t vendorInfoCount;
  VkDeviceSize vendorBinarySize;
} VkDeviceFaultCountsEXT;

typedef VkResult (VKAPI_PTR *PFN_vkGetDeviceFaultInfoEXT) (
  VkDevice device,
  VkDeviceFaultCountsEXT *pFaultCounts,
  void *pFaultInfo);
#endif

static gboolean
extension_in_list (const char                    *name,
                   const VkExtensionProperties   *props,
                   uint32_t                       count)
{
  uint32_t i;

  for (i = 0; i < count; i++)
    {
      if (strcmp (props[i].extensionName, name) == 0)
        return TRUE;
    }

  return FALSE;
}

static gboolean
get_drm_dev_ids (int       drm_fd,
                 uint64_t *out_major,
                 uint64_t *out_minor)
{
  struct stat st;

  if (drm_fd < 0)
    return FALSE;

  if (fstat (drm_fd, &st) != 0)
    return FALSE;

  *out_major = (uint64_t) major (st.st_rdev);
  *out_minor = (uint64_t) minor (st.st_rdev);
  return TRUE;
}

static gboolean
physical_device_matches_drm (VkPhysicalDevice phys,
                             uint64_t         want_major,
                             uint64_t         want_minor)
{
  VkPhysicalDeviceProperties2 props2 = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
  };
  VkPhysicalDeviceDrmPropertiesEXT drm_props = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
  };

  props2.pNext = &drm_props;
  vkGetPhysicalDeviceProperties2 (phys, &props2);

  if (drm_props.hasPrimary &&
      drm_props.primaryMajor == want_major &&
      drm_props.primaryMinor == want_minor)
    return TRUE;

  if (drm_props.hasRender &&
      drm_props.renderMajor == want_major &&
      drm_props.renderMinor == want_minor)
    return TRUE;

  return FALSE;
}

static gboolean
pick_physical_device (VkInstance         instance,
                      int                drm_fd,
                      gboolean           have_device_drm_ext,
                      VkPhysicalDevice  *out_phys,
                      uint32_t          *out_queue_family,
                      GError           **error)
{
  uint32_t count = 0;
  g_autofree VkPhysicalDevice *devs = NULL;
  uint32_t i;
  uint64_t drm_major = 0, drm_minor = 0;
  gboolean have_drm_ids;
  int fallback = -1;
  uint32_t fallback_q = 0;

  vkEnumeratePhysicalDevices (instance, &count, NULL);
  if (count == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No Vulkan physical devices");
      return FALSE;
    }

  devs = g_new0 (VkPhysicalDevice, count);
  vkEnumeratePhysicalDevices (instance, &count, devs);

  have_drm_ids = have_device_drm_ext &&
                 get_drm_dev_ids (drm_fd, &drm_major, &drm_minor);

  for (i = 0; i < count; i++)
    {
      uint32_t qcount = 0;
      g_autofree VkQueueFamilyProperties *qprops = NULL;
      uint32_t q;
      gboolean matched = FALSE;

      vkGetPhysicalDeviceQueueFamilyProperties (devs[i], &qcount, NULL);
      qprops = g_new0 (VkQueueFamilyProperties, qcount);
      vkGetPhysicalDeviceQueueFamilyProperties (devs[i], &qcount, qprops);

      for (q = 0; q < qcount; q++)
        {
          if (!(qprops[q].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            continue;

          if (fallback < 0)
            {
              fallback = (int) i;
              fallback_q = q;
            }

          if (have_drm_ids)
            matched = physical_device_matches_drm (devs[i],
                                                   drm_major, drm_minor);

          if (!have_drm_ids || matched)
            {
              *out_phys = devs[i];
              *out_queue_family = q;
              if (have_drm_ids && matched)
                {
                  COGL_NOTE (WINSYS,
                             "Cogl Vulkan: matched DRM %llu:%llu (phys %u)",
                             (unsigned long long) drm_major,
                             (unsigned long long) drm_minor,
                             i);
                }
              return TRUE;
            }
          break;
        }
    }

  if (fallback >= 0)
    {
      *out_phys = devs[fallback];
      *out_queue_family = fallback_q;
      COGL_NOTE (WINSYS,
                 "Cogl Vulkan: DRM match unavailable; using first graphics GPU");
      return TRUE;
    }

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "No Vulkan graphics queue family");
  return FALSE;
}

static gboolean
collect_device_extensions (VkPhysicalDevice   phys,
                           CoglVkDevice      *dev,
                           const char      ***out_names,
                           uint32_t          *out_count,
                           GError           **error)
{
  uint32_t count = 0;
  g_autofree VkExtensionProperties *props = NULL;
  g_autoptr (GPtrArray) names = g_ptr_array_new ();

  vkEnumerateDeviceExtensionProperties (phys, NULL, &count, NULL);
  props = g_new0 (VkExtensionProperties, count);
  vkEnumerateDeviceExtensionProperties (phys, NULL, &count, props);

  /* Native scanout needs dma-buf and SYNC_FD. A renderer without a DRM
   * device can render into ordinary Vulkan images and read them back.
   */
  dev->has_dma_buf =
    extension_in_list (VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, props, count) &&
    extension_in_list (VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME, props, count);
  dev->has_external_semaphore_fd =
    extension_in_list (VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, props, count);
  if (dev->has_external_semaphore_fd)
    {
      VkPhysicalDeviceExternalSemaphoreInfo info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
      };
      VkExternalSemaphoreProperties properties = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
      };
      VkExternalSemaphoreFeatureFlags required =
        VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT |
        VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT;

      vkGetPhysicalDeviceExternalSemaphoreProperties (phys, &info, &properties);
      dev->has_external_semaphore_fd =
        (properties.externalSemaphoreFeatures & required) == required;
    }

  if (dev->drm_fd >= 0 &&
      (!dev->has_dma_buf || !dev->has_external_semaphore_fd))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Vulkan device missing dmabuf/SYNC_FD support for DRM rendering");
      return FALSE;
    }

  if (dev->has_dma_buf)
    {
      g_ptr_array_add (names, (gpointer) VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
      g_ptr_array_add (names, (gpointer) VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    }
  if (dev->has_external_semaphore_fd)
    g_ptr_array_add (names, (gpointer) VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);

  if (dev->has_dma_buf &&
      extension_in_list (VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
                         props, count))
    {
      g_ptr_array_add (names,
                       (gpointer) VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
      if (extension_in_list (VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME, props, count))
        g_ptr_array_add (names, (gpointer) VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
      if (extension_in_list (VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
                             props, count))
        {
          g_ptr_array_add (names,
                           (gpointer) VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
          dev->has_queue_family_foreign = TRUE;
        }
      if (extension_in_list (VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME,
                             props, count))
        {
          g_ptr_array_add (names,
                           (gpointer) VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
        }
      dev->has_drm_modifiers = TRUE;
    }
  else if (extension_in_list (VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME,
                              props, count))
    {
      g_ptr_array_add (names,
                       (gpointer) VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
    }

  if (extension_in_list (VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME, props, count))
    {
      g_ptr_array_add (names, (gpointer) VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
      dev->has_timeline_semaphore = TRUE;
    }

  /* Soft reset detail: optional; DEVICE_LOST from submit still works without
   * it.
   */
  if (extension_in_list (VK_EXT_DEVICE_FAULT_EXTENSION_NAME, props, count))
    {
      g_ptr_array_add (names, (gpointer) VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
      dev->has_device_fault = TRUE;
    }

  /* Push the hot texture sampler into the CB, avoiding per-draw DS alloc. */
  if (extension_in_list (VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, props, count))
    {
      g_ptr_array_add (names,
                       (gpointer) VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
      dev->has_push_descriptor = TRUE;
    }

  /* NV12 sampling requires sampler YCbCr conversion, core in Vulkan 1.1. */
  {
    VkPhysicalDeviceFeatures2 features2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
    };
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr = {
      .sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
    };
    features2.pNext = &ycbcr;
    vkGetPhysicalDeviceFeatures2 (phys, &features2);
    if (ycbcr.samplerYcbcrConversion)
      dev->has_sampler_ycbcr = TRUE;
  }

  *out_count = names->len;
  *out_names = (const char **) g_ptr_array_free (g_steal_pointer (&names), FALSE);
  return TRUE;
}

CoglVkDevice *
cogl_vk_device_new (int      drm_fd,
                    GError **error)
{
  CoglVkDevice *dev;
  VkApplicationInfo app_info = {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = "mutter-cogl",
    .applicationVersion = VK_MAKE_VERSION (1, 0, 0),
    .pEngineName = "cogl-vk",
    .engineVersion = VK_MAKE_VERSION (1, 0, 0),
    .apiVersion = VK_API_VERSION_1_1,
  };
  const char *instance_exts[4];
  uint32_t n_instance_exts = 0;
  uint32_t inst_ext_count = 0;
  g_autofree VkExtensionProperties *inst_props = NULL;
  gboolean have_device_drm_ext = FALSE;
  VkInstanceCreateInfo inst_info;
  float queue_prio = 1.0f;
  VkDeviceQueueCreateInfo queue_info;
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline_feat = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
    .timelineSemaphore = VK_TRUE,
  };
  VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr_feat = {
    .sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
    .samplerYcbcrConversion = VK_TRUE,
  };
  VkPhysicalDeviceFaultFeaturesEXT fault_feat = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT,
    .deviceFault = VK_TRUE,
  };
  VkDeviceCreateInfo device_info;
  void *feat_chain = NULL;
  VkPhysicalDeviceFeatures supported_features;
  VkPhysicalDeviceFeatures enabled_features;
  g_autofree const char **dev_ext_names = NULL;
  uint32_t n_dev_exts = 0;
  VkResult res;

  dev = g_new0 (CoglVkDevice, 1);
  dev->drm_fd = drm_fd;
  dev->reset_status = COGL_GRAPHICS_RESET_STATUS_NO_ERROR;
  dev->last_timestamp_begin = -1;
  dev->last_timestamp_end = -1;

  vkEnumerateInstanceExtensionProperties (NULL, &inst_ext_count, NULL);
  inst_props = g_new0 (VkExtensionProperties, inst_ext_count);
  vkEnumerateInstanceExtensionProperties (NULL, &inst_ext_count, inst_props);

  if (extension_in_list (VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
                         inst_props, inst_ext_count))
    {
      instance_exts[n_instance_exts++] =
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    }
  if (extension_in_list (VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
                         inst_props, inst_ext_count))
    {
      instance_exts[n_instance_exts++] =
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME;
    }
  if (extension_in_list (VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
                         inst_props, inst_ext_count))
    {
      instance_exts[n_instance_exts++] =
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME;
    }
  if (extension_in_list (VK_EXT_DEBUG_REPORT_EXTENSION_NAME,
                         inst_props, inst_ext_count))
    {
      /* skip debug by default */
    }

  /* Device DRM props are queried via properties2; the device extension is
   * advertised on physical devices but matching uses instance props2.
   */
  have_device_drm_ext = TRUE;

  memset (&inst_info, 0, sizeof (inst_info));
  inst_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  inst_info.pApplicationInfo = &app_info;
  inst_info.enabledExtensionCount = n_instance_exts;
  inst_info.ppEnabledExtensionNames = instance_exts;

  /* Opt-in: VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation (conform debug). */
  {
    const char *layer_env = g_getenv ("VK_INSTANCE_LAYERS");
    static const char *validation_layer = "VK_LAYER_KHRONOS_validation";

    if (layer_env && *layer_env &&
        strstr (layer_env, "VK_LAYER_KHRONOS_validation") != NULL)
      {
        inst_info.enabledLayerCount = 1;
        inst_info.ppEnabledLayerNames = &validation_layer;
        g_message ("Cogl Vulkan: enabling %s", validation_layer);
      }
  }

  res = vkCreateInstance (&inst_info, NULL, &dev->instance);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkCreateInstance failed (%d)", (int) res);
      g_free (dev);
      return NULL;
    }

  if (!pick_physical_device (dev->instance, drm_fd, have_device_drm_ext,
                             &dev->physical_device,
                             &dev->graphics_queue_family,
                             error))
    {
      vkDestroyInstance (dev->instance, NULL);
      g_free (dev);
      return NULL;
    }

  if (!collect_device_extensions (dev->physical_device, dev,
                                  &dev_ext_names, &n_dev_exts, error))
    {
      vkDestroyInstance (dev->instance, NULL);
      g_free (dev);
      return NULL;
    }

  memset (&queue_info, 0, sizeof (queue_info));
  queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queue_info.queueFamilyIndex = dev->graphics_queue_family;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &queue_prio;

  /* Core features: start from a zeroed enable mask (pEnabledFeatures was
   * previously NULL → all off). Query support, then enable only what shaders
   * need — largePoints so gl_PointSize > 1 is honored
   * (GL_PROGRAM_POINT_SIZE).
   */
  memset (&enabled_features, 0, sizeof (enabled_features));
  vkGetPhysicalDeviceFeatures (dev->physical_device, &supported_features);
  if (supported_features.largePoints)
    {
      enabled_features.largePoints = VK_TRUE;
    }
  else
    {
      static gboolean large_points_warned;

      if (!large_points_warned)
        {
          g_warning ("Cogl Vulkan: largePoints unsupported; "
                     "gl_PointSize > 1 may be ignored");
          large_points_warned = TRUE;
        }
    }

  memset (&device_info, 0, sizeof (device_info));
  device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  device_info.enabledExtensionCount = n_dev_exts;
  device_info.ppEnabledExtensionNames = dev_ext_names;
  device_info.pEnabledFeatures = &enabled_features;
  if (dev->has_timeline_semaphore)
    {
      timeline_feat.pNext = feat_chain;
      feat_chain = &timeline_feat;
    }
  if (dev->has_sampler_ycbcr)
    {
      ycbcr_feat.pNext = feat_chain;
      feat_chain = &ycbcr_feat;
    }
  if (dev->has_device_fault)
    {
      fault_feat.pNext = feat_chain;
      feat_chain = &fault_feat;
    }
  device_info.pNext = feat_chain;

  res = vkCreateDevice (dev->physical_device, &device_info, NULL, &dev->device);
  /* Names owned by g_autofree; steal so cleanup does not double-free. */
  g_free (g_steal_pointer (&dev_ext_names));
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkCreateDevice failed (%d)", (int) res);
      vkDestroyInstance (dev->instance, NULL);
      g_free (dev);
      return NULL;
    }

  vkGetDeviceQueue (dev->device, dev->graphics_queue_family, 0,
                    &dev->graphics_queue);

  {
    VkPhysicalDeviceProperties props;
    uint32_t qcount = 0;
    g_autofree VkQueueFamilyProperties *qprops = NULL;

    vkGetPhysicalDeviceProperties (dev->physical_device, &props);
    dev->limits = props.limits;
    COGL_NOTE (WINSYS, "Cogl Vulkan physical device: %s", props.deviceName);
    dev->timestamp_period = props.limits.timestampPeriod;

    vkGetPhysicalDeviceQueueFamilyProperties (dev->physical_device, &qcount,
                                              NULL);
    qprops = g_new0 (VkQueueFamilyProperties, qcount);
    vkGetPhysicalDeviceQueueFamilyProperties (dev->physical_device, &qcount,
                                              qprops);
    if (dev->graphics_queue_family < qcount)
      dev->timestamp_valid_bits =
        qprops[dev->graphics_queue_family].timestampValidBits;
  }

  if (dev->timestamp_period > 0.0f && dev->timestamp_valid_bits > 0)
    {
      VkQueryPoolCreateInfo qinfo = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = COGL_VK_TIMESTAMP_QUERY_COUNT,
      };

      res = vkCreateQueryPool (dev->device, &qinfo, NULL, &dev->timestamp_pool);
      if (res == VK_SUCCESS)
        {
          dev->has_timestamp_query = TRUE;
          dev->timestamp_next = 0;
        }
      else
        {
          COGL_NOTE (WINSYS,
                     "Cogl Vulkan: timestamp query pool failed (%d); "
                     "TIMESTAMP_QUERY unset",
                     (int) res);
          dev->timestamp_pool = VK_NULL_HANDLE;
        }
    }

  {
    VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = dev->graphics_queue_family,
    };

    res = vkCreateCommandPool (dev->device, &pool_info, NULL, &dev->command_pool);
    if (res != VK_SUCCESS)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "vkCreateCommandPool failed (%d)", (int) res);
        if (dev->timestamp_pool != VK_NULL_HANDLE)
          vkDestroyQueryPool (dev->device, dev->timestamp_pool, NULL);
        vkDestroyDevice (dev->device, NULL);
        vkDestroyInstance (dev->instance, NULL);
        g_free (dev);
        return NULL;
      }

    res = vkCreateCommandPool (dev->device, &pool_info, NULL,
                               &dev->oneshot_command_pool);
    if (res != VK_SUCCESS)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "vkCreateCommandPool (oneshot) failed (%d)", (int) res);
        vkDestroyCommandPool (dev->device, dev->command_pool, NULL);
        if (dev->timestamp_pool != VK_NULL_HANDLE)
          vkDestroyQueryPool (dev->device, dev->timestamp_pool, NULL);
        vkDestroyDevice (dev->device, NULL);
        vkDestroyInstance (dev->instance, NULL);
        g_free (dev);
        return NULL;
      }
  }

  dev->vkGetMemoryFdKHR =
    (PFN_vkGetMemoryFdKHR) vkGetDeviceProcAddr (dev->device, "vkGetMemoryFdKHR");
  dev->vkGetMemoryFdPropertiesKHR =
    (PFN_vkGetMemoryFdPropertiesKHR)
    vkGetDeviceProcAddr (dev->device, "vkGetMemoryFdPropertiesKHR");
  dev->vkGetSemaphoreFdKHR =
    (PFN_vkGetSemaphoreFdKHR) vkGetDeviceProcAddr (dev->device,
                                                   "vkGetSemaphoreFdKHR");
  dev->vkImportSemaphoreFdKHR =
    (PFN_vkImportSemaphoreFdKHR) vkGetDeviceProcAddr (dev->device,
                                                      "vkImportSemaphoreFdKHR");
  dev->vkGetFenceFdKHR =
    (PFN_vkGetFenceFdKHR) vkGetDeviceProcAddr (dev->device, "vkGetFenceFdKHR");
  dev->vkGetSemaphoreCounterValue =
    (PFN_vkGetSemaphoreCounterValue)
    vkGetDeviceProcAddr (dev->device, "vkGetSemaphoreCounterValue");
  if (!dev->vkGetSemaphoreCounterValue)
    {
      dev->vkGetSemaphoreCounterValue =
        (PFN_vkGetSemaphoreCounterValue)
        vkGetDeviceProcAddr (dev->device, "vkGetSemaphoreCounterValueKHR");
    }
  if (dev->has_device_fault)
    {
      dev->vkGetDeviceFaultInfoEXT =
        (CoglVkGetDeviceFaultInfoEXT)
        vkGetDeviceProcAddr (dev->device, "vkGetDeviceFaultInfoEXT");
      if (!dev->vkGetDeviceFaultInfoEXT)
        dev->has_device_fault = FALSE;
    }
  dev->vkGetPhysicalDeviceImageFormatProperties2 =
    (PFN_vkGetPhysicalDeviceImageFormatProperties2)
    vkGetInstanceProcAddr (dev->instance,
                           "vkGetPhysicalDeviceImageFormatProperties2");

  if (dev->has_push_descriptor)
    {
      dev->vkCmdPushDescriptorSetKHR =
        (PFN_vkCmdPushDescriptorSetKHR)
        vkGetDeviceProcAddr (dev->device, "vkCmdPushDescriptorSetKHR");
      if (!dev->vkCmdPushDescriptorSetKHR)
        dev->has_push_descriptor = FALSE;
    }

  /* Keep available fence tickets separate from incomplete submissions. */
  dev->fence_free = g_ptr_array_new_with_free_func (
    (GDestroyNotify) cogl_vk_fence_ticket_free);
  dev->fence_inflight = g_ptr_array_new_with_free_func (
    (GDestroyNotify) cogl_vk_fence_ticket_free);

  COGL_NOTE (WINSYS,
             "Cogl Vulkan device ready (queue family %u, dmabuf=%d modifiers=%d "
             "timeline=%d foreign=%d ycbcr=%d timestamp=%d fault=%d)",
             dev->graphics_queue_family,
             dev->has_dma_buf,
             dev->has_drm_modifiers,
             dev->has_timeline_semaphore,
             dev->has_queue_family_foreign,
             dev->has_sampler_ycbcr,
             dev->has_timestamp_query,
             dev->has_device_fault);

  return dev;
}

void
cogl_vk_device_free (CoglVkDevice *device)
{
  if (!device)
    return;

  /* Fence tickets may hold a binary semaphore whose destruction was deferred
   * past a SYNC_FD export (see cogl_vk_fence_ticket_export_sync_fd). Idle the
   * device before tearing the pools down so those, and the timelines, are
   * destroyed with no submit still referencing them. Teardown only.
   */
  if (device->device != VK_NULL_HANDLE)
    vkDeviceWaitIdle (device->device);

  cogl_vk_vbo_device_clear (device);
  g_clear_pointer (&device->fence_inflight, g_ptr_array_unref);
  g_clear_pointer (&device->fence_free, g_ptr_array_unref);

  if (device->timestamp_pool != VK_NULL_HANDLE)
    vkDestroyQueryPool (device->device, device->timestamp_pool, NULL);
  if (device->oneshot_command_pool != VK_NULL_HANDLE)
    vkDestroyCommandPool (device->device, device->oneshot_command_pool, NULL);
  if (device->command_pool != VK_NULL_HANDLE)
    vkDestroyCommandPool (device->device, device->command_pool, NULL);
  if (device->device != VK_NULL_HANDLE)
    vkDestroyDevice (device->device, NULL);
  if (device->instance != VK_NULL_HANDLE)
    vkDestroyInstance (device->instance, NULL);
  g_free (device);
}

void
cogl_vk_device_note_vk_result (CoglVkDevice *device,
                               VkResult      res)
{
  if (!device)
    return;

  if (res != VK_ERROR_DEVICE_LOST)
    return;

  if (device->device_lost)
    return;

  device->device_lost = TRUE;
  /* Report device loss through the Cogl graphics reset status. */
  device->reset_status = COGL_GRAPHICS_RESET_STATUS_PURGED_CONTEXT_RESET;

  if (device->has_device_fault && device->vkGetDeviceFaultInfoEXT)
    {
      VkDeviceFaultCountsEXT counts = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT,
      };
      VkResult fault_res;

      fault_res = device->vkGetDeviceFaultInfoEXT (device->device, &counts, NULL);
      if (fault_res == VK_SUCCESS || fault_res == VK_INCOMPLETE)
        {
          COGL_NOTE (WINSYS,
                     "Cogl Vulkan: DEVICE_LOST (device_fault counts: "
                     "addr=%u vendor=%u)",
                     counts.addressInfoCount,
                     counts.vendorInfoCount);
        }
      else
        {
          COGL_NOTE (WINSYS,
                     "Cogl Vulkan: DEVICE_LOST (vkGetDeviceFaultInfoEXT %d)",
                     (int) fault_res);
        }
    }
  else
    {
      COGL_NOTE (WINSYS,
                 "Cogl Vulkan: DEVICE_LOST observed (no VK_EXT_device_fault)");
    }
}

CoglGraphicsResetStatus
cogl_vk_device_get_graphics_reset_status (CoglVkDevice *device)
{
  if (!device)
    return COGL_GRAPHICS_RESET_STATUS_NO_ERROR;

  if (!device->device_lost)
    return COGL_GRAPHICS_RESET_STATUS_NO_ERROR;

  return device->reset_status;
}

int
cogl_vk_device_cmd_write_timestamp (CoglVkDevice           *device,
                                    VkCommandBuffer         cmd,
                                    VkPipelineStageFlagBits stage)
{
  uint32_t index;

  g_return_val_if_fail (device != NULL, -1);
  g_return_val_if_fail (cmd != VK_NULL_HANDLE, -1);

  if (!device->has_timestamp_query ||
      device->timestamp_pool == VK_NULL_HANDLE)
    return -1;

  index = device->timestamp_next % COGL_VK_TIMESTAMP_QUERY_COUNT;
  device->timestamp_next++;

  /* Queries must be reset before (re)use; cheap single-slot reset. */
  vkCmdResetQueryPool (cmd, device->timestamp_pool, index, 1);
  vkCmdWriteTimestamp (cmd, stage, device->timestamp_pool, index);
  return (int) index;
}

gboolean
cogl_vk_device_get_timestamp_ns (CoglVkDevice  *device,
                                 uint32_t       query_index,
                                 uint64_t      *out_ns)
{
  uint64_t ticks = 0;
  VkResult res;

  g_return_val_if_fail (device != NULL, FALSE);
  g_return_val_if_fail (out_ns != NULL, FALSE);

  if (!device->has_timestamp_query ||
      device->timestamp_pool == VK_NULL_HANDLE ||
      query_index >= COGL_VK_TIMESTAMP_QUERY_COUNT)
    return FALSE;

  /* Non-blocking: no WAIT_BIT — matches "no hot-path fence wait" contract. */
  res = vkGetQueryPoolResults (device->device,
                               device->timestamp_pool,
                               query_index,
                               1,
                               sizeof (ticks),
                               &ticks,
                               sizeof (ticks),
                               VK_QUERY_RESULT_64_BIT);
  if (res != VK_SUCCESS)
    {
      cogl_vk_device_note_vk_result (device, res);
      return FALSE;
    }

  *out_ns = (uint64_t) ((double) ticks * (double) device->timestamp_period);
  return TRUE;
}

gboolean
cogl_vk_device_peek_last_timestamps (CoglVkDevice  *device,
                                     int           *out_begin_index,
                                     int           *out_end_index,
                                     uint64_t      *out_begin_ns,
                                     uint64_t      *out_end_ns)
{
  g_return_val_if_fail (device != NULL, FALSE);

  if (device->last_timestamp_begin < 0 || device->last_timestamp_end < 0)
    return FALSE;

  if (out_begin_index)
    *out_begin_index = device->last_timestamp_begin;
  if (out_end_index)
    *out_end_index = device->last_timestamp_end;

  /* Never WAIT_BIT — composite path must not stall on query readiness. */
  if (out_begin_ns)
    {
      if (!cogl_vk_device_get_timestamp_ns (device,
                                            (uint32_t) device->last_timestamp_begin,
                                            out_begin_ns))
        *out_begin_ns = 0;
    }
  if (out_end_ns)
    {
      if (!cogl_vk_device_get_timestamp_ns (device,
                                            (uint32_t) device->last_timestamp_end,
                                            out_end_ns))
        *out_end_ns = 0;
    }

  return TRUE;
}
