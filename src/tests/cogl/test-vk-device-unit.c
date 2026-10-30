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

/* Capability selection against production code, with synthetic ICD reports. */
#include "config.h"
#include "cogl/driver/vk/cogl-device-vk.c"

static const char * const external_extensions[] = {
  VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
  VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
  VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
};
static unsigned int extension_count;
static VkExternalSemaphoreFeatureFlags semaphore_features;

VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateDeviceExtensionProperties (VkPhysicalDevice       physical_device,
                                      const char            *layer_name,
                                      uint32_t              *count,
                                      VkExtensionProperties *properties)
{
  if (!properties)
    *count = extension_count;
  else
    {
      g_assert_cmpuint (*count, >=, extension_count);
      for (unsigned int i = 0; i < extension_count; i++)
        g_strlcpy (properties[i].extensionName, external_extensions[i],
                   sizeof (properties[i].extensionName));
      *count = extension_count;
    }
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceExternalSemaphoreProperties (
  VkPhysicalDevice                             physical_device,
  const VkPhysicalDeviceExternalSemaphoreInfo *info,
  VkExternalSemaphoreProperties               *properties)
{
  g_assert_cmpuint (info->handleType, ==,
                    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
  properties->externalSemaphoreFeatures = semaphore_features;
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFeatures2 (VkPhysicalDevice           physical_device,
                              VkPhysicalDeviceFeatures2 *features)
{
  VkPhysicalDeviceSamplerYcbcrConversionFeatures *ycbcr = features->pNext;
  ycbcr->samplerYcbcrConversion = VK_FALSE;
}

static void
test_offscreen (void)
{
  CoglVkDevice device = { .drm_fd = -1 };
  g_autofree const char **names = NULL;
  unsigned int count = 0;

  extension_count = 0;
  g_assert_true (collect_device_extensions (VK_NULL_HANDLE, &device,
                                            &names, &count, NULL));
  g_assert_cmpuint (count, ==, 0);
  g_assert_false (device.has_dma_buf);
  g_assert_false (device.has_external_semaphore_fd);
}

static void
test_drm_requires_external (void)
{
  CoglVkDevice device = { .drm_fd = 4 };
  g_autofree const char **names = NULL;
  g_autoptr (GError) error = NULL;
  unsigned int count = 0;

  extension_count = 0;
  g_assert_false (collect_device_extensions (VK_NULL_HANDLE, &device,
                                             &names, &count, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
}

static void
test_sync_fd_features (void)
{
  const VkExternalSemaphoreFeatureFlags unsupported[] = {
    0, VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT,
    VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT,
  };

  extension_count = G_N_ELEMENTS (external_extensions);
  for (unsigned int i = 0; i < G_N_ELEMENTS (unsupported); i++)
    {
      CoglVkDevice device = { .drm_fd = -1 };
      g_autofree const char **names = NULL;
      g_autoptr (GError) error = NULL;
      unsigned int count = 0;

      semaphore_features = unsupported[i];
      g_assert_true (collect_device_extensions (VK_NULL_HANDLE, &device,
                                                &names, &count, NULL));
      g_assert_true (device.has_dma_buf);
      g_assert_false (device.has_external_semaphore_fd);
      g_assert_cmpuint (count, ==, 2);
      g_clear_pointer (&names, g_free);
      device.drm_fd = 4;
      g_assert_false (collect_device_extensions (VK_NULL_HANDLE, &device,
                                                 &names, &count, &error));
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
    }
}

static void
test_native_external (void)
{
  CoglVkDevice device = { .drm_fd = 4 };
  g_autofree const char **names = NULL;
  unsigned int count = 0;

  extension_count = G_N_ELEMENTS (external_extensions);
  semaphore_features = VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT |
                       VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT;
  g_assert_true (collect_device_extensions (VK_NULL_HANDLE, &device,
                                            &names, &count, NULL));
  g_assert_true (device.has_dma_buf);
  g_assert_true (device.has_external_semaphore_fd);
  g_assert_cmpuint (count, ==, 3);
}

int
main (int    argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/vk-device/offscreen", test_offscreen);
  g_test_add_func ("/vk-device/drm-requires-external", test_drm_requires_external);
  g_test_add_func ("/vk-device/sync-fd-features", test_sync_fd_features);
  g_test_add_func ("/vk-device/native-external", test_native_external);
  return g_test_run ();
}
