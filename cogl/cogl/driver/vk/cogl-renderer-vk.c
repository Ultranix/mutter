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

#include "cogl/driver/vk/cogl-renderer-vk-private.h"

#include <drm_fourcc.h>
#include <sys/stat.h>

#include "cogl/cogl-context-private.h"
#include "cogl/cogl-offscreen.h"
#include "cogl/cogl-render-device.h"
#include "cogl/cogl-renderer-private.h"
#include "cogl/driver/vk/cogl-display-vk-private.h"
#include "cogl/driver/vk/cogl-driver-vk-private.h"
#include "cogl/driver/vk/cogl-image-vk-private.h"
#include "cogl/driver/vk/cogl-texture-driver-vk-private.h"

struct _CoglRendererVk
{
  CoglRenderer parent_instance;
};

G_DEFINE_TYPE (CoglRendererVk, cogl_renderer_vk, COGL_TYPE_RENDERER)

static gboolean
cogl_renderer_vk_load_driver (CoglRenderer  *renderer,
                              CoglDriverId   driver_id,
                              GError       **error)
{
  g_autoptr (CoglDriverVk) driver_vk = NULL;

  if (driver_id != COGL_DRIVER_ID_VULKAN)
    return FALSE;

  driver_vk = g_object_new (COGL_TYPE_DRIVER_VK, NULL);
  if (!cogl_driver_update_features (COGL_DRIVER (driver_vk), renderer, error) ||
      !cogl_driver_vk_initialize (driver_vk, renderer, error))
    return FALSE;

  cogl_renderer_set_driver (renderer, COGL_DRIVER (g_steal_pointer (&driver_vk)));
  return TRUE;
}

static CoglDisplay *
cogl_renderer_vk_create_display (CoglRenderer *renderer)
{
  return g_object_new (COGL_TYPE_DISPLAY_VK,
                       "renderer", renderer,
                       NULL);
}

static int
cogl_renderer_vk_get_sync_fd (CoglRenderer *renderer)
{
  return cogl_driver_vk_get_latest_sync_fd (
    COGL_DRIVER_VK (cogl_renderer_get_driver (renderer)));
}

static CoglFramebuffer *
cogl_renderer_vk_create_dma_buf_framebuffer (CoglRenderer     *renderer,
                                             CoglContext      *context,
                                             uint32_t          width,
                                             uint32_t          height,
                                             uint32_t          drm_format,
                                             CoglPixelFormat   cogl_format,
                                             int               n_planes,
                                             const int        *fds,
                                             const uint32_t   *strides,
                                             const uint32_t   *offsets,
                                             const uint64_t   *modifiers,
                                             GError          **error)
{
  g_autoptr (CoglTexture) texture = NULL;
  g_autoptr (CoglOffscreen) offscreen = NULL;

  texture = cogl_texture_2d_vk_new_from_dmabuf (context, width, height,
                                                cogl_format, drm_format,
                                                n_planes, fds, strides,
                                                offsets, modifiers,
                                                COGL_YUV_MATRIX_BT709,
                                                COGL_YUV_RANGE_NARROW,
                                                error);
  if (!texture)
    return NULL;

  offscreen = cogl_offscreen_new_with_texture (texture);
  if (!cogl_framebuffer_allocate (COGL_FRAMEBUFFER (offscreen), error))
    return NULL;

  return COGL_FRAMEBUFFER (g_steal_pointer (&offscreen));
}

static gboolean
cogl_renderer_vk_is_hardware_accelerated (CoglRenderer *renderer)
{
  CoglVkDevice *device = cogl_driver_vk_peek_device (
    COGL_DRIVER_VK (cogl_renderer_get_driver (renderer)));

  VkPhysicalDeviceProperties properties;

  if (!device)
    return FALSE;

  vkGetPhysicalDeviceProperties (device->physical_device, &properties);
  return properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU;
}

static gboolean
cogl_renderer_vk_query_dma_buf_modifiers (CoglRenderer  *renderer,
                                          uint32_t       drm_format,
                                          GArray        *modifiers,
                                          GArray        *external_only,
                                          GError       **error)
{
  CoglVkDevice *device = cogl_driver_vk_peek_device (
    COGL_DRIVER_VK (cogl_renderer_get_driver (renderer)));
  VkFormat format = cogl_vk_format_from_drm_fourcc (drm_format);
  gboolean external = FALSE;
  uint64_t linear = DRM_FORMAT_MOD_LINEAR;

  if (!device || !device->has_dma_buf || format == VK_FORMAT_UNDEFINED)
    return TRUE;

  if (cogl_vk_dmabuf_format_supported (device, drm_format, linear,
                                       cogl_vk_dmabuf_image_usage (drm_format),
                                       NULL))
    {
      g_array_append_val (modifiers, linear);
      g_array_append_val (external_only, external);
    }

  if (device->has_drm_modifiers)
    {
      VkDrmFormatModifierPropertiesListEXT list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
      };
      VkFormatProperties2 properties = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &list,
      };
      g_autofree VkDrmFormatModifierPropertiesEXT *entries = NULL;
      uint32_t i;

      vkGetPhysicalDeviceFormatProperties2 (device->physical_device, format,
                                            &properties);
      entries = g_new0 (VkDrmFormatModifierPropertiesEXT,
                        list.drmFormatModifierCount);
      list.pDrmFormatModifierProperties = entries;
      vkGetPhysicalDeviceFormatProperties2 (device->physical_device, format,
                                            &properties);
      for (i = 0; i < list.drmFormatModifierCount; i++)
        {
          uint64_t modifier = entries[i].drmFormatModifier;

          if (modifier == linear ||
              !cogl_vk_dmabuf_format_supported (
                device, drm_format, modifier,
                cogl_vk_dmabuf_image_usage (drm_format), NULL))
            continue;

          g_array_append_val (modifiers, modifier);
          g_array_append_val (external_only, external);
        }
    }

  return TRUE;
}

static gboolean
cogl_renderer_vk_query_dma_buf_formats (CoglRenderer  *renderer,
                                        GArray        *formats,
                                        GError       **error)
{
  const uint32_t candidates[] = {
    DRM_FORMAT_R8,
    DRM_FORMAT_GR88,
    DRM_FORMAT_R16,
    DRM_FORMAT_GR1616,
    DRM_FORMAT_RGB565,
    DRM_FORMAT_ABGR16161616,
    DRM_FORMAT_XBGR16161616,
    DRM_FORMAT_ABGR16161616F,
    DRM_FORMAT_XBGR16161616F,
    DRM_FORMAT_ARGB16161616F,
    DRM_FORMAT_XRGB16161616F,
    DRM_FORMAT_ARGB8888,
    DRM_FORMAT_XRGB8888,
    DRM_FORMAT_ABGR8888,
    DRM_FORMAT_XBGR8888,
    DRM_FORMAT_RGBA8888,
    DRM_FORMAT_RGBX8888,
    DRM_FORMAT_BGRA8888,
    DRM_FORMAT_BGRX8888,
    DRM_FORMAT_ARGB2101010,
    DRM_FORMAT_XRGB2101010,
    DRM_FORMAT_ABGR2101010,
    DRM_FORMAT_XBGR2101010,
    DRM_FORMAT_NV12,
  };
  unsigned int i;

  for (i = 0; i < G_N_ELEMENTS (candidates); i++)
    {
      g_autoptr (GArray) modifiers =
        g_array_new (FALSE, FALSE, sizeof (uint64_t));
      g_autoptr (GArray) external_only =
        g_array_new (FALSE, FALSE, sizeof (gboolean));
      uint32_t format = candidates[i];

      if (!cogl_renderer_vk_query_dma_buf_modifiers (renderer, format,
                                                     modifiers, external_only,
                                                     error))
        return FALSE;
      if (modifiers->len)
        g_array_append_val (formats, format);
    }

  return TRUE;
}

static gboolean
cogl_renderer_vk_query_drm_device (CoglRenderer  *renderer,
                                   dev_t         *device_id,
                                   GError       **error)
{
  CoglRenderDevice *render_device = cogl_renderer_get_render_device (renderer);
  int fd = render_device ? cogl_render_device_get_drm_fd (render_device) : -1;
  struct stat device_stat;

  if (fd < 0 || fstat (fd, &device_stat) != 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "Vulkan renderer has no DRM node");
      return FALSE;
    }

  *device_id = device_stat.st_rdev;
  return TRUE;
}

static gboolean
cogl_renderer_vk_can_import_dma_buf (CoglRenderer *renderer,
                                     uint32_t      drm_format,
                                     uint64_t      modifier,
                                     int           n_planes)
{
  CoglVkDevice *device = cogl_driver_vk_peek_device (
    COGL_DRIVER_VK (cogl_renderer_get_driver (renderer)));
  uint32_t required_planes;

  return cogl_vk_dmabuf_format_supported (
    device, drm_format, modifier, cogl_vk_dmabuf_image_usage (drm_format),
    &required_planes) &&
         (n_planes == 0 || n_planes == required_planes);
}

static void
cogl_renderer_vk_class_init (CoglRendererVkClass *class)
{
  CoglRendererClass *renderer_class = COGL_RENDERER_CLASS (class);

  renderer_class->can_import_dma_buf = cogl_renderer_vk_can_import_dma_buf;
  renderer_class->query_dma_buf_formats = cogl_renderer_vk_query_dma_buf_formats;
  renderer_class->query_dma_buf_modifiers =
    cogl_renderer_vk_query_dma_buf_modifiers;
  renderer_class->query_drm_device = cogl_renderer_vk_query_drm_device;
  renderer_class->load_driver = cogl_renderer_vk_load_driver;
  renderer_class->create_display = cogl_renderer_vk_create_display;
  renderer_class->get_sync_fd = cogl_renderer_vk_get_sync_fd;
  renderer_class->create_dma_buf_framebuffer =
    cogl_renderer_vk_create_dma_buf_framebuffer;
  renderer_class->is_hardware_accelerated =
    cogl_renderer_vk_is_hardware_accelerated;
}

static void
cogl_renderer_vk_init (CoglRendererVk *renderer_vk)
{
  cogl_renderer_set_feature (COGL_RENDERER (renderer_vk),
                             COGL_RENDERER_FEATURE_OFFSCREEN_ONLY, TRUE);
}
