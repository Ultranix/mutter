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
#include <unistd.h>
#include <glib/gstdio.h>
#include "cogl/driver/vk/cogl-dmabuf-memo-vk.c"
#include "cogl/driver/vk/cogl-image-vk.c"

static VkImageMemoryBarrier last_barrier;

void
cogl_vk_pass_note_image_state (CoglVkPass  *pass,
                               CoglVkImage *image)
{
}


void
cogl_vk_debug_log (const char *channel,
                   const char *format,
                   ...)
{
}

VKAPI_ATTR void VKAPI_CALL
vkCmdPipelineBarrier (VkCommandBuffer              command_buffer,
                      VkPipelineStageFlags         src_stage,
                      VkPipelineStageFlags         dst_stage,
                      VkDependencyFlags            dependency_flags,
                      uint32_t                     memory_count,
                      const VkMemoryBarrier       *memory_barriers,
                      uint32_t                     buffer_count,
                      const VkBufferMemoryBarrier *buffer_barriers,
                      uint32_t                     image_count,
                      const VkImageMemoryBarrier  *image_barriers)
{
  g_assert_cmpuint (image_count, ==, 1);
  last_barrier = image_barriers[0];
}

static void
test_foreign_layout_handoff (void)
{
  CoglVkDevice device = { .has_queue_family_foreign = TRUE,
                          .graphics_queue_family = 2 };
  CoglVkImage image = { .device = &device, .is_dmabuf = TRUE,
                        .layout = VK_IMAGE_LAYOUT_GENERAL, .mip_levels = 1 };

  for (int i = 0; i < 2; i++)
    {
      cogl_vk_image_cmd_foreign_acquire (&image, VK_NULL_HANDLE, 2,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      /* Initial import must preserve the producer's contents too. */
      g_assert_cmpint (last_barrier.oldLayout, ==, VK_IMAGE_LAYOUT_GENERAL);
      g_assert_cmpint (last_barrier.newLayout, ==, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      g_assert_cmpuint (last_barrier.srcQueueFamilyIndex, ==, VK_QUEUE_FAMILY_FOREIGN_EXT);
      g_assert_cmpuint (last_barrier.dstQueueFamilyIndex, ==, 2);
      g_assert_true (image.dmabuf_graphics_owned);
      cogl_vk_image_cmd_foreign_release (&image, VK_NULL_HANDLE, 2,
                                         VK_IMAGE_LAYOUT_GENERAL);
      g_assert_cmpint (last_barrier.oldLayout, ==, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      g_assert_cmpint (last_barrier.newLayout, ==, VK_IMAGE_LAYOUT_GENERAL);
      g_assert_cmpuint (last_barrier.dstQueueFamilyIndex, ==, VK_QUEUE_FAMILY_FOREIGN_EXT);
      g_assert_false (image.dmabuf_graphics_owned);
    }
}

static void
test_plane_identity (void)
{
  g_autofree char *path = NULL;
  g_autoptr (GError) error = NULL;
  int fd = g_file_open_tmp ("mutter-vk-memo-XXXXXX", &path, &error);
  int fds[2];
  uint32_t strides[] = { 64, 64 }, offsets[] = { 0, 4096 };
  uint64_t modifiers[] = { DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_LINEAR };
  CoglVkDmabufImport import = {
    64, 64, COGL_PIXEL_FORMAT_R_8, DRM_FORMAT_R8, 1,
    fds, strides, offsets, modifiers
  };
  CoglVkDmabufMemo *memo = cogl_vk_dmabuf_memo_new ();
  g_autoptr (GObject) y = g_object_new (G_TYPE_OBJECT, NULL);
  g_autoptr (GObject) uv = g_object_new (G_TYPE_OBJECT, NULL);
  g_autoptr (CoglTexture) found = NULL;
  CoglVkDmabufImport chroma;

  g_assert_no_error (error);
  g_assert_cmpint (fd, >=, 0);
  fds[0] = fd;
  fds[1] = dup (fd);
  g_assert_cmpint (fds[1], >=, 0);
  cogl_vk_dmabuf_memo_store (memo, &import, (CoglTexture *) y);
  /* Duplicate descriptors denote the same allocation. */
  import.fds = &fds[1];
  found = cogl_vk_dmabuf_memo_lookup (memo, &import);
  g_assert_true ((gpointer) found == y);
  g_clear_object (&found);

  chroma = import;
  chroma.width /= 2;
  chroma.height /= 2;
  chroma.drm_fourcc = DRM_FORMAT_GR88;
  chroma.format = COGL_PIXEL_FORMAT_RG_88;
  chroma.offsets = &offsets[1];
  g_assert_null (cogl_vk_dmabuf_memo_lookup (memo, &chroma));
  cogl_vk_dmabuf_memo_store (memo, &chroma, (CoglTexture *) uv);
  found = cogl_vk_dmabuf_memo_lookup (memo, &chroma);
  g_assert_true ((gpointer) found == uv);
  g_clear_object (&found);
  found = cogl_vk_dmabuf_memo_lookup (memo, &import);
  g_assert_true ((gpointer) found == y);
  g_clear_object (&found);

  /* Every part of the interpretation must distinguish imports. */
  for (int field = 0; field < 8; field++)
    {
      CoglVkDmabufImport changed = import;
      uint32_t stride = strides[0] + 1, offset = offsets[0] + 1;
      uint64_t modifier = DRM_FORMAT_MOD_INVALID;
      switch (field)
        {
        case 0: changed.width++; break;
        case 1: changed.height++; break;
        case 2: changed.format = COGL_PIXEL_FORMAT_A_8; break;
        case 3: changed.drm_fourcc = DRM_FORMAT_GR88; break;
        case 4: changed.strides = &stride; break;
        case 5: changed.offsets = &offset; break;
        case 6: changed.modifiers = &modifier; break;
        case 7: changed.ycbcr_index = 1; break;
        }
      g_assert_null (cogl_vk_dmabuf_memo_lookup (memo, &changed));
    }
  cogl_vk_dmabuf_memo_free (memo);
  close (fds[1]);
  close (fd);
  g_unlink (path);
}

static void
test_all_planes_in_key (void)
{
  g_autofree char *path1 = NULL, *path2 = NULL;
  int fds[] = { g_file_open_tmp ("mutter-memo-a-XXXXXX", &path1, NULL),
                g_file_open_tmp ("mutter-memo-b-XXXXXX", &path2, NULL) };
  uint32_t strides[] = { 64, 64 }, offsets[] = { 0, 0 };
  CoglVkDmabufImport import = {
    64, 64, COGL_PIXEL_FORMAT_RGBA_8888_PRE, DRM_FORMAT_NV12, 2,
    fds, strides, offsets, NULL
  };
  CoglVkDmabufMemo *memo = cogl_vk_dmabuf_memo_new ();
  g_autoptr (GObject) texture = g_object_new (G_TYPE_OBJECT, NULL);
  int original = fds[1];

  g_assert_cmpint (fds[0], >=, 0);
  g_assert_cmpint (fds[1], >=, 0);
  cogl_vk_dmabuf_memo_store (memo, &import, (CoglTexture *) texture);
  fds[1] = fds[0];
  g_assert_null (cogl_vk_dmabuf_memo_lookup (memo, &import));
  fds[1] = original;
  offsets[1] = 1024;
  g_assert_null (cogl_vk_dmabuf_memo_lookup (memo, &import));
  offsets[1] = 0;
  strides[1] = 128;
  g_assert_null (cogl_vk_dmabuf_memo_lookup (memo, &import));
  cogl_vk_dmabuf_memo_free (memo);
  close (fds[0]);
  close (fds[1]);
  g_unlink (path1);
  g_unlink (path2);
}

static void
test_linear_plane_layout (void)
{
  VkSubresourceLayout layouts[] = {
    { .offset = 0, .rowPitch = 64 },
    { .offset = 4096, .rowPitch = 64 },
  };
  uint32_t strides[] = { 64, 64 }, offsets[] = { 8192, 12288 };
  VkDeviceSize bind_offset;
  g_autoptr (GError) error = NULL;

  g_assert_cmpint (cogl_vk_format_from_drm_fourcc (DRM_FORMAT_R8), ==, VK_FORMAT_R8_UNORM);
  g_assert_cmpint (cogl_vk_format_from_drm_fourcc (DRM_FORMAT_GR88), ==, VK_FORMAT_R8G8_UNORM);
  g_assert_true (dmabuf_linear_bind_offset (layouts, 2, strides, offsets,
                                            &bind_offset, &error));
  g_assert_cmpuint (bind_offset, ==, 8192);
  /* Importing only the UV plane still preserves its offset in the fd. */
  g_assert_true (dmabuf_linear_bind_offset (layouts, 1, strides, &offsets[1],
                                            &bind_offset, &error));
  g_assert_cmpuint (bind_offset, ==, 12288);
  strides[1] = 128;
  g_assert_false (dmabuf_linear_bind_offset (layouts, 2, strides, offsets,
                                             &bind_offset, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error (&error);
  strides[1] = 64;
  offsets[1]++;
  g_assert_false (dmabuf_linear_bind_offset (layouts, 2, strides, offsets,
                                             &bind_offset, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
}

static gboolean importable = TRUE;
static VkImageUsageFlags queried_usage;

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFormatProperties2 (VkPhysicalDevice     device,
                                      VkFormat             format,
                                      VkFormatProperties2 *properties)
{
  VkDrmFormatModifierPropertiesListEXT *list = properties->pNext;
  if (list->pDrmFormatModifierProperties)
    {
      list->pDrmFormatModifierProperties[0] =
        (VkDrmFormatModifierPropertiesEXT) { DRM_FORMAT_MOD_LINEAR, 1, 0 };
      list->pDrmFormatModifierProperties[1] =
        (VkDrmFormatModifierPropertiesEXT) { 42, 2, 0 };
    }
  list->drmFormatModifierCount = 2;
}

static VkResult VKAPI_CALL
query_image_format (VkPhysicalDevice                        device,
                    const VkPhysicalDeviceImageFormatInfo2 *info,
                    VkImageFormatProperties2               *properties)
{
  VkExternalImageFormatProperties *external = properties->pNext;
  queried_usage = info->usage;
  external->externalMemoryProperties.externalMemoryFeatures =
    importable ? VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT : 0;
  return VK_SUCCESS;
}

static void
test_import_capabilities (void)
{
  CoglVkDevice device = {
    .has_dma_buf = TRUE,
    .has_drm_modifiers = TRUE,
    .vkGetPhysicalDeviceImageFormatProperties2 = query_image_format,
  };
  uint32_t planes = 0;
  VkImageUsageFlags usage = cogl_vk_dmabuf_image_usage (DRM_FORMAT_XRGB2101010);

  g_assert_true (cogl_vk_dmabuf_format_supported (&device, DRM_FORMAT_XRGB2101010,
                                                  42, usage, &planes));
  g_assert_cmpuint (planes, ==, 2);
  g_assert_true ((queried_usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0);
  g_assert_false (cogl_vk_dmabuf_format_supported (&device, DRM_FORMAT_XRGB2101010,
                                                   43, usage, NULL));
  importable = FALSE;
  g_assert_false (cogl_vk_dmabuf_format_supported (&device, DRM_FORMAT_XRGB2101010,
                                                   42, usage, NULL));
  importable = TRUE;
  device.has_drm_modifiers = FALSE;
  g_assert_false (cogl_vk_dmabuf_format_supported (&device, DRM_FORMAT_XRGB2101010,
                                                   42, usage, NULL));
  g_assert_true (cogl_vk_dmabuf_format_supported (&device, DRM_FORMAT_XRGB2101010,
                                                  DRM_FORMAT_MOD_INVALID, usage, &planes));
  g_assert_cmpuint (planes, ==, 1);
  g_assert_false (cogl_vk_dmabuf_format_supported (&device, 0, 0, usage, NULL));
}

static void
test_import_formats (void)
{
  const struct { uint32_t drm; VkFormat vk; } formats[] = {
    { DRM_FORMAT_R16, VK_FORMAT_R16_UNORM },
    { DRM_FORMAT_GR1616, VK_FORMAT_R16G16_UNORM },
    { DRM_FORMAT_RGB565, VK_FORMAT_R5G6B5_UNORM_PACK16 },
    { DRM_FORMAT_ABGR16161616, VK_FORMAT_R16G16B16A16_UNORM },
    { DRM_FORMAT_ABGR16161616F, VK_FORMAT_R16G16B16A16_SFLOAT },
    { DRM_FORMAT_ARGB16161616F, VK_FORMAT_R16G16B16A16_SFLOAT },
    { DRM_FORMAT_XRGB2101010, VK_FORMAT_A2R10G10B10_UNORM_PACK32 },
    { DRM_FORMAT_XBGR2101010, VK_FORMAT_A2B10G10R10_UNORM_PACK32 },
  };
  VkComponentMapping swizzle;

  for (unsigned int i = 0; i < G_N_ELEMENTS (formats); i++)
    g_assert_cmpint (cogl_vk_format_from_drm_fourcc (formats[i].drm), ==, formats[i].vk);
  cogl_vk_format_swizzle_from_drm (DRM_FORMAT_RGBA8888, &swizzle);
  g_assert_cmpint (swizzle.r, ==, VK_COMPONENT_SWIZZLE_A);
  g_assert_cmpint (swizzle.a, ==, VK_COMPONENT_SWIZZLE_R);
  cogl_vk_format_swizzle_from_drm (DRM_FORMAT_BGRX8888, &swizzle);
  g_assert_cmpint (swizzle.r, ==, VK_COMPONENT_SWIZZLE_G);
  g_assert_cmpint (swizzle.b, ==, VK_COMPONENT_SWIZZLE_A);
  g_assert_cmpint (swizzle.a, ==, VK_COMPONENT_SWIZZLE_ONE);
  cogl_vk_format_swizzle_from_drm (DRM_FORMAT_XRGB16161616F, &swizzle);
  g_assert_cmpint (swizzle.r, ==, VK_COMPONENT_SWIZZLE_B);
  g_assert_cmpint (swizzle.a, ==, VK_COMPONENT_SWIZZLE_ONE);
  g_assert_false ((cogl_vk_dmabuf_image_usage (DRM_FORMAT_R16) & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0);
}

int main (int    argc,
          char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/vk-dmabuf/plane-identity", test_plane_identity);
  g_test_add_func ("/vk-dmabuf/all-planes", test_all_planes_in_key);
  g_test_add_func ("/vk-dmabuf/linear-layout", test_linear_plane_layout);
  g_test_add_func ("/vk-dmabuf/foreign-layout-handoff", test_foreign_layout_handoff);
  g_test_add_func ("/vk-dmabuf/import-capabilities", test_import_capabilities);
  g_test_add_func ("/vk-dmabuf/import-formats", test_import_formats);
  return g_test_run ();
}
