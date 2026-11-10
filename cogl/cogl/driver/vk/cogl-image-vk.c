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

#include "cogl/driver/vk/cogl-image-vk-private.h"
#include "cogl/driver/vk/cogl-driver-vk-private.h"
#include "cogl/driver/vk/cogl-fence-vk-private.h"
#include "cogl/driver/vk/cogl-vk-debug-private.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static gboolean
vk_format_sampled_linear (CoglVkDevice *device,
                          VkFormat      format)
{
  VkFormatProperties props;

  if (!device || format == VK_FORMAT_UNDEFINED)
    return FALSE;

  vkGetPhysicalDeviceFormatProperties (device->physical_device, format, &props);
  return (props.linearTilingFeatures &
          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
}

static gboolean
vk_format_color_attachment_linear (CoglVkDevice *device,
                                   VkFormat      format)
{
  VkFormatProperties props;

  if (!device || format == VK_FORMAT_UNDEFINED)
    return FALSE;

  vkGetPhysicalDeviceFormatProperties (device->physical_device, format, &props);
  return (props.linearTilingFeatures &
          VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
}

VkFormat
cogl_vk_format_from_cogl (CoglPixelFormat format)
{
  switch (format)
    {
    case COGL_PIXEL_FORMAT_A_8:
    case COGL_PIXEL_FORMAT_R_8:
      return VK_FORMAT_R8_UNORM;

    case COGL_PIXEL_FORMAT_RG_88:
      return VK_FORMAT_R8G8_UNORM;

    /* Prefer 32bpp: R8G8B8 is widely non-sampleable on desktop GPUs
     * (Intel/ANV reports sampled_lin=0). Callers that need a 3bpp VkFormat
     * for transfers can still use the packed enums via other paths.
     */
    case COGL_PIXEL_FORMAT_RGB_888:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case COGL_PIXEL_FORMAT_BGR_888:
      return VK_FORMAT_B8G8R8A8_UNORM;

    case COGL_PIXEL_FORMAT_RGBX_8888:
    case COGL_PIXEL_FORMAT_RGBA_8888:
    case COGL_PIXEL_FORMAT_RGBA_8888_PRE:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case COGL_PIXEL_FORMAT_BGRX_8888:
    case COGL_PIXEL_FORMAT_BGRA_8888:
    case COGL_PIXEL_FORMAT_BGRA_8888_PRE:
      return VK_FORMAT_B8G8R8A8_UNORM;
    /* AFIRST 8888: Cogl stores A,R,G,B (ARGB) or A,B,G,R (ABGR) in
     * address order. Map to B8G8R8A8 / R8G8B8A8 and correct via swizzle.
     */
    case COGL_PIXEL_FORMAT_XRGB_8888:
    case COGL_PIXEL_FORMAT_ARGB_8888:
    case COGL_PIXEL_FORMAT_ARGB_8888_PRE:
      return VK_FORMAT_B8G8R8A8_UNORM;
    case COGL_PIXEL_FORMAT_XBGR_8888:
    case COGL_PIXEL_FORMAT_ABGR_8888:
    case COGL_PIXEL_FORMAT_ABGR_8888_PRE:
      return VK_FORMAT_R8G8B8A8_UNORM;

    case COGL_PIXEL_FORMAT_RGB_565:
      return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case COGL_PIXEL_FORMAT_RGBA_4444:
    case COGL_PIXEL_FORMAT_RGBA_4444_PRE:
      return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
    case COGL_PIXEL_FORMAT_RGBA_5551:
    case COGL_PIXEL_FORMAT_RGBA_5551_PRE:
      return VK_FORMAT_R5G5B5A1_UNORM_PACK16;

    case COGL_PIXEL_FORMAT_RGBA_1010102:
    case COGL_PIXEL_FORMAT_RGBA_1010102_PRE:
    case COGL_PIXEL_FORMAT_XBGR_2101010:
    case COGL_PIXEL_FORMAT_ABGR_2101010:
    case COGL_PIXEL_FORMAT_ABGR_2101010_PRE:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case COGL_PIXEL_FORMAT_BGRA_1010102:
    case COGL_PIXEL_FORMAT_BGRA_1010102_PRE:
    case COGL_PIXEL_FORMAT_XRGB_2101010:
    case COGL_PIXEL_FORMAT_ARGB_2101010:
    case COGL_PIXEL_FORMAT_ARGB_2101010_PRE:
      return VK_FORMAT_A2R10G10B10_UNORM_PACK32;

    case COGL_PIXEL_FORMAT_RGBX_FP_16161616:
    case COGL_PIXEL_FORMAT_RGBA_FP_16161616:
    case COGL_PIXEL_FORMAT_RGBA_FP_16161616_PRE:
    case COGL_PIXEL_FORMAT_BGRX_FP_16161616:
    case COGL_PIXEL_FORMAT_BGRA_FP_16161616:
    case COGL_PIXEL_FORMAT_BGRA_FP_16161616_PRE:
    case COGL_PIXEL_FORMAT_XRGB_FP_16161616:
    case COGL_PIXEL_FORMAT_ARGB_FP_16161616:
    case COGL_PIXEL_FORMAT_ARGB_FP_16161616_PRE:
    case COGL_PIXEL_FORMAT_XBGR_FP_16161616:
    case COGL_PIXEL_FORMAT_ABGR_FP_16161616:
    case COGL_PIXEL_FORMAT_ABGR_FP_16161616_PRE:
      return VK_FORMAT_R16G16B16A16_SFLOAT;

    case COGL_PIXEL_FORMAT_RGBA_FP_32323232:
    case COGL_PIXEL_FORMAT_RGBA_FP_32323232_PRE:
      return VK_FORMAT_R32G32B32A32_SFLOAT;

    case COGL_PIXEL_FORMAT_R_16:
      return VK_FORMAT_R16_UNORM;
    case COGL_PIXEL_FORMAT_RG_1616:
      return VK_FORMAT_R16G16_UNORM;
    case COGL_PIXEL_FORMAT_RGBX_16161616:
    case COGL_PIXEL_FORMAT_RGBA_16161616:
    case COGL_PIXEL_FORMAT_RGBA_16161616_PRE:
      return VK_FORMAT_R16G16B16A16_UNORM;

    case COGL_PIXEL_FORMAT_DEPTH_16:
      return VK_FORMAT_D16_UNORM;
    case COGL_PIXEL_FORMAT_DEPTH_24_STENCIL_8:
      return VK_FORMAT_D24_UNORM_S8_UINT;

    case COGL_PIXEL_FORMAT_ANY:
    case COGL_PIXEL_FORMAT_YUV:
    default:
      return VK_FORMAT_UNDEFINED;
    }
}

void
cogl_vk_format_swizzle_from_cogl (CoglPixelFormat     format,
                                  VkComponentMapping *out)
{
  static const VkComponentMapping identity = {
    VK_COMPONENT_SWIZZLE_IDENTITY,
    VK_COMPONENT_SWIZZLE_IDENTITY,
    VK_COMPONENT_SWIZZLE_IDENTITY,
    VK_COMPONENT_SWIZZLE_IDENTITY,
  };

  g_return_if_fail (out != NULL);

  *out = identity;

  switch (format)
    {
    case COGL_PIXEL_FORMAT_A_8:
      /* GL3: R8 + swizzle (0,0,0,R) so .a samples the stored byte. */
      out->r = VK_COMPONENT_SWIZZLE_ZERO;
      out->g = VK_COMPONENT_SWIZZLE_ZERO;
      out->b = VK_COMPONENT_SWIZZLE_ZERO;
      out->a = VK_COMPONENT_SWIZZLE_R;
      break;

    /* ARGB/XRGB → B8G8R8A8: memory is A,R,G,B. Uploaded as BGRA bytes the
     * format names see B=A, G=R, R=G, A=B. Remap so sampled rgba = (R,G,B,A):
     * r←G, g←R, b←A, a←B
     */
    case COGL_PIXEL_FORMAT_XRGB_8888:
    case COGL_PIXEL_FORMAT_ARGB_8888:
    case COGL_PIXEL_FORMAT_ARGB_8888_PRE:
      out->r = VK_COMPONENT_SWIZZLE_G;
      out->g = VK_COMPONENT_SWIZZLE_R;
      out->b = VK_COMPONENT_SWIZZLE_A;
      out->a = VK_COMPONENT_SWIZZLE_B;
      break;

    /* ABGR/XBGR → R8G8B8A8: memory is A,B,G,R. Format names see R=A, G=B,
     * B=G, A=R. Remap: r←A, g←B, b←G, a←R.
     */
    case COGL_PIXEL_FORMAT_XBGR_8888:
    case COGL_PIXEL_FORMAT_ABGR_8888:
    case COGL_PIXEL_FORMAT_ABGR_8888_PRE:
      out->r = VK_COMPONENT_SWIZZLE_A;
      out->g = VK_COMPONENT_SWIZZLE_B;
      out->b = VK_COMPONENT_SWIZZLE_G;
      out->a = VK_COMPONENT_SWIZZLE_R;
      break;

    /* BGR* FP16 stored in RGBA float layout: byte/component order B,G,R,A. */
    case COGL_PIXEL_FORMAT_BGRX_FP_16161616:
    case COGL_PIXEL_FORMAT_BGRA_FP_16161616:
    case COGL_PIXEL_FORMAT_BGRA_FP_16161616_PRE:
      out->r = VK_COMPONENT_SWIZZLE_B;
      out->g = VK_COMPONENT_SWIZZLE_G;
      out->b = VK_COMPONENT_SWIZZLE_R;
      out->a = VK_COMPONENT_SWIZZLE_A;
      break;

    /* AFIRST FP16 in RGBA float layout — same remaps as 8888 AFIRST. */
    case COGL_PIXEL_FORMAT_XRGB_FP_16161616:
    case COGL_PIXEL_FORMAT_ARGB_FP_16161616:
    case COGL_PIXEL_FORMAT_ARGB_FP_16161616_PRE:
      out->r = VK_COMPONENT_SWIZZLE_G;
      out->g = VK_COMPONENT_SWIZZLE_B;
      out->b = VK_COMPONENT_SWIZZLE_A;
      out->a = VK_COMPONENT_SWIZZLE_R;
      break;

    case COGL_PIXEL_FORMAT_XBGR_FP_16161616:
    case COGL_PIXEL_FORMAT_ABGR_FP_16161616:
    case COGL_PIXEL_FORMAT_ABGR_FP_16161616_PRE:
      out->r = VK_COMPONENT_SWIZZLE_A;
      out->g = VK_COMPONENT_SWIZZLE_B;
      out->b = VK_COMPONENT_SWIZZLE_G;
      out->a = VK_COMPONENT_SWIZZLE_R;
      break;

    default:
      break;
    }
}

VkFormat
cogl_vk_format_from_drm_fourcc (uint32_t fourcc)
{
  switch (fourcc)
    {
    case DRM_FORMAT_R8:
      return VK_FORMAT_R8_UNORM;
    case DRM_FORMAT_GR88:
      return VK_FORMAT_R8G8_UNORM;
    case DRM_FORMAT_R16:
      return VK_FORMAT_R16_UNORM;
    case DRM_FORMAT_GR1616:
      return VK_FORMAT_R16G16_UNORM;
    case DRM_FORMAT_RGB565:
      return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case DRM_FORMAT_ABGR16161616:
    case DRM_FORMAT_XBGR16161616:
      return VK_FORMAT_R16G16B16A16_UNORM;
    case DRM_FORMAT_ABGR16161616F:
    case DRM_FORMAT_XBGR16161616F:
    case DRM_FORMAT_ARGB16161616F:
    case DRM_FORMAT_XRGB16161616F:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XRGB8888:
      return VK_FORMAT_B8G8R8A8_UNORM;
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_XBGR8888:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case DRM_FORMAT_RGBA8888:
    case DRM_FORMAT_RGBX8888:
    case DRM_FORMAT_BGRA8888:
    case DRM_FORMAT_BGRX8888:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case DRM_FORMAT_ARGB2101010:
    case DRM_FORMAT_XRGB2101010:
      return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    case DRM_FORMAT_ABGR2101010:
    case DRM_FORMAT_XBGR2101010:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case DRM_FORMAT_NV12: /* Sample both planes through YCbCr conversion. */
      return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    default:
      return VK_FORMAT_UNDEFINED;
    }
}

void
cogl_vk_format_swizzle_from_drm (uint32_t            fourcc,
                                 VkComponentMapping *components)
{
  *components = (VkComponentMapping) { 0 };
  switch (fourcc)
    {
    case DRM_FORMAT_RGBA8888:
    case DRM_FORMAT_RGBX8888:
      *components = (VkComponentMapping) {
        VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_B,
        VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R
      };
      break;
    case DRM_FORMAT_BGRA8888:
    case DRM_FORMAT_BGRX8888:
      *components = (VkComponentMapping) {
        VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
        VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_R
      };
      break;
    case DRM_FORMAT_ARGB16161616F:
    case DRM_FORMAT_XRGB16161616F:
      components->r = VK_COMPONENT_SWIZZLE_B;
      components->b = VK_COMPONENT_SWIZZLE_R;
      break;
    default:
      break;
    }
  switch (fourcc)
    {
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_XBGR8888:
    case DRM_FORMAT_RGBX8888:
    case DRM_FORMAT_BGRX8888:
    case DRM_FORMAT_XRGB2101010:
    case DRM_FORMAT_XBGR2101010:
    case DRM_FORMAT_XBGR16161616:
    case DRM_FORMAT_XRGB16161616F:
    case DRM_FORMAT_XBGR16161616F:
      components->a = VK_COMPONENT_SWIZZLE_ONE;
      break;
    default:
      break;
    }
}

VkImageUsageFlags
cogl_vk_dmabuf_image_usage (uint32_t fourcc)
{
  VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (fourcc == DRM_FORMAT_NV12)
    return usage;
  usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (fourcc != DRM_FORMAT_R8 && fourcc != DRM_FORMAT_GR88 &&
      fourcc != DRM_FORMAT_R16 && fourcc != DRM_FORMAT_GR1616)
    usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  return usage;
}

/* Gen12 CCS (especially RC_CCS_CC) needs the aux plane and the clear-color
 * plane. ANV samples the opaque window, but the fast-cleared shadow margin
 * comes back as a solid blue ring. Do not advertise those modifiers when
 * the compositor samples with Vulkan; single-plane Y-tiled and linear stay.
 */
static gboolean
intel_ccs_modifier (uint64_t modifier)
{
  switch (modifier)
    {
#ifdef I915_FORMAT_MOD_Y_TILED_CCS
    case I915_FORMAT_MOD_Y_TILED_CCS:
#endif
#ifdef I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS
    case I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS:
#endif
#ifdef I915_FORMAT_MOD_Y_TILED_GEN12_MC_CCS
    case I915_FORMAT_MOD_Y_TILED_GEN12_MC_CCS:
#endif
#ifdef I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS_CC
    case I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS_CC:
#endif
#ifdef I915_FORMAT_MOD_4_TILED_DG2_RC_CCS
    case I915_FORMAT_MOD_4_TILED_DG2_RC_CCS:
#endif
#ifdef I915_FORMAT_MOD_4_TILED_DG2_RC_CCS_CC
    case I915_FORMAT_MOD_4_TILED_DG2_RC_CCS_CC:
#endif
#ifdef I915_FORMAT_MOD_4_TILED_DG2_MC_CCS
    case I915_FORMAT_MOD_4_TILED_DG2_MC_CCS:
#endif
#ifdef I915_FORMAT_MOD_4_TILED_MTL_RC_CCS
    case I915_FORMAT_MOD_4_TILED_MTL_RC_CCS:
#endif
#ifdef I915_FORMAT_MOD_4_TILED_MTL_RC_CCS_CC
    case I915_FORMAT_MOD_4_TILED_MTL_RC_CCS_CC:
#endif
#ifdef I915_FORMAT_MOD_4_TILED_MTL_MC_CCS
    case I915_FORMAT_MOD_4_TILED_MTL_MC_CCS:
#endif
    return TRUE;
    default:
      return FALSE;
    }
}

gboolean
cogl_vk_dmabuf_format_supported (CoglVkDevice     *device,
                                 uint32_t          fourcc,
                                 uint64_t          modifier,
                                 VkImageUsageFlags usage,
                                 uint32_t         *n_planes)
{
  VkFormat format = cogl_vk_format_from_drm_fourcc (fourcc);
  VkPhysicalDeviceExternalImageFormatInfo external = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
  };
  VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
    .pNext = &external,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  VkPhysicalDeviceImageFormatInfo2 info = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
    .pNext = &external,
    .format = format,
    .type = VK_IMAGE_TYPE_2D,
    .tiling = VK_IMAGE_TILING_LINEAR,
    .usage = usage,
  };
  VkExternalImageFormatProperties external_props = {
    .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
  };
  VkImageFormatProperties2 props = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
    .pNext = &external_props,
  };
  uint32_t planes = 1;

  if (!device || !device->has_dma_buf || format == VK_FORMAT_UNDEFINED ||
      !device->vkGetPhysicalDeviceImageFormatProperties2)
    return FALSE;
  if (modifier == DRM_FORMAT_MOD_INVALID)
    modifier = DRM_FORMAT_MOD_LINEAR;
  if (intel_ccs_modifier (modifier))
    return FALSE;
  if (device->has_drm_modifiers)
    {
      VkDrmFormatModifierPropertiesListEXT list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
      };
      VkFormatProperties2 format_props = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &list,
      };
      g_autofree VkDrmFormatModifierPropertiesEXT *modifiers = NULL;
      gboolean found = FALSE;

      vkGetPhysicalDeviceFormatProperties2 (device->physical_device, format,
                                            &format_props);
      modifiers = g_new0 (VkDrmFormatModifierPropertiesEXT,
                          list.drmFormatModifierCount);
      list.pDrmFormatModifierProperties = modifiers;
      vkGetPhysicalDeviceFormatProperties2 (device->physical_device, format,
                                            &format_props);
      for (uint32_t i = 0; i < list.drmFormatModifierCount; i++)
        if (modifiers[i].drmFormatModifier == modifier)
          {
            planes = modifiers[i].drmFormatModifierPlaneCount;
            found = TRUE;
            break;
          }
      if (!found)
        return FALSE;
      modifier_info.drmFormatModifier = modifier;
      info.pNext = &modifier_info;
      info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    }
  else if (modifier != DRM_FORMAT_MOD_LINEAR)
    return FALSE;

  if (device->vkGetPhysicalDeviceImageFormatProperties2 (
        device->physical_device, &info, &props) != VK_SUCCESS ||
      !(external_props.externalMemoryProperties.externalMemoryFeatures &
        VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
    return FALSE;
  if (n_planes)
    *n_planes = planes;
  return TRUE;
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

static CoglVkImage *
image_alloc_common (CoglVkDevice    *device,
                    uint32_t         width,
                    uint32_t         height,
                    VkFormat         format,
                    CoglPixelFormat  cogl_format)
{
  CoglVkImage *img = g_new0 (CoglVkImage, 1);

  img->device = device;
  img->width = width;
  img->height = height;
  img->mip_levels = 1;
  img->samples = VK_SAMPLE_COUNT_1_BIT;
  img->format = format;
  img->cogl_format = cogl_format;
  img->layout = VK_IMAGE_LAYOUT_UNDEFINED;
  img->owns_memory = TRUE;
  return img;
}

static VkImageAspectFlags
aspect_from_vk_format (VkFormat format)
{
  switch (format)
    {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
      return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT:
      return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
      return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

void
cogl_vk_format_color_bits (VkFormat format,
                           int     *red,
                           int     *green,
                           int     *blue,
                           int     *alpha)
{
  int r = 8, g = 8, b = 8, a = 8;

  switch (format)
    {
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8_SRGB:
      r = 8; g = 0; b = 0; a = 0;
      break;
    case VK_FORMAT_R8G8_UNORM:
      r = 8; g = 8; b = 0; a = 0;
      break;
    case VK_FORMAT_R8G8B8_UNORM:
    case VK_FORMAT_B8G8R8_UNORM:
    case VK_FORMAT_R8G8B8_SRGB:
    case VK_FORMAT_B8G8R8_SRGB:
      r = 8; g = 8; b = 8; a = 0;
      break;
    case VK_FORMAT_R5G6B5_UNORM_PACK16:
    case VK_FORMAT_B5G6R5_UNORM_PACK16:
      r = 5; g = 6; b = 5; a = 0;
      break;
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
      r = 16; g = 16; b = 16; a = 16;
      break;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
      r = 10; g = 10; b = 10; a = 2;
      break;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    default:
      r = 8; g = 8; b = 8; a = 8;
      break;
    }

  if (red)
    *red = r;
  if (green)
    *green = g;
  if (blue)
    *blue = b;
  if (alpha)
    *alpha = a;
}

void
cogl_vk_format_depth_stencil_bits (VkFormat format,
                                   int     *depth,
                                   int     *stencil)
{
  int d = 0, s = 0;

  switch (format)
    {
    case VK_FORMAT_D16_UNORM:
      d = 16;
      break;
    case VK_FORMAT_D16_UNORM_S8_UINT:
      d = 16;
      s = 8;
      break;
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
      d = 24;
      if (format == VK_FORMAT_D24_UNORM_S8_UINT)
        s = 8;
      break;
    case VK_FORMAT_D32_SFLOAT:
      d = 32;
      break;
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
      d = 32;
      s = 8;
      break;
    case VK_FORMAT_S8_UINT:
      s = 8;
      break;
    default:
      break;
    }

  if (depth)
    *depth = d;
  if (stencil)
    *stencil = s;
}

static gboolean
format_supports_depth_stencil (CoglVkDevice *device,
                               VkFormat      format)
{
  VkFormatProperties props;

  vkGetPhysicalDeviceFormatProperties (device->physical_device, format, &props);
  return (props.optimalTilingFeatures &
          VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
}

static gboolean create_image_view (CoglVkImage              *img,
                                   VkSamplerYcbcrConversion  ycbcr_conversion,
                                   GError                  **error);

static CoglVkImage *
new_depth_stencil_format (CoglVkDevice          *device,
                          uint32_t               width,
                          uint32_t               height,
                          VkFormat               format,
                          VkSampleCountFlagBits  samples,
                          GError               **error)
{
  VkImageUsageFlags usage =
    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

  if (samples <= VK_SAMPLE_COUNT_1_BIT)
    {
      CoglVkImage *img;

      img = cogl_vk_image_new_optimal (device, width, height, format,
                                       COGL_PIXEL_FORMAT_ANY, usage, error);
      if (!img)
        return NULL;
      /* Define before any LOAD RP; prefer allocate-time oneshot so
       * begin_render rarely oneshots while a frame CB is recording.
       */
      {
        g_autoptr (GError) clear_err = NULL;
        if (!cogl_vk_image_oneshot_clear_to_depth_stencil_attachment (img,
                                                                      &clear_err))
          {
            if (error && clear_err)
              g_propagate_error (error, g_steal_pointer (&clear_err));
            cogl_vk_image_free (img);
            return NULL;
          }
      }
      return img;
    }

  /* MS packed DS: same sample count as MS color; no color-usage bits. */
  {
    CoglVkImage *img;
    VkImageCreateInfo image_info;
    VkMemoryRequirements reqs;
    VkMemoryAllocateInfo alloc_info;
    int mem_type;
    VkResult res;

    img = image_alloc_common (device, width, height, format,
                              COGL_PIXEL_FORMAT_ANY);
    img->samples = samples;

    memset (&image_info, 0, sizeof (image_info));
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent.width = width;
    image_info.extent.height = height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = samples;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    res = vkCreateImage (device->device, &image_info, NULL, &img->image);
    if (res != VK_SUCCESS)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "vkCreateImage (MS depth/stencil) failed (%d)", (int) res);
        g_free (img);
        return NULL;
      }
    vkGetImageMemoryRequirements (device->device, img->image, &reqs);
    mem_type = find_memory_type (device, reqs.memoryTypeBits,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type < 0)
      {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "No DEVICE_LOCAL memory for MS depth/stencil");
        cogl_vk_image_free (img);
        return NULL;
      }
    memset (&alloc_info, 0, sizeof (alloc_info));
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = reqs.size;
    alloc_info.memoryTypeIndex = (uint32_t) mem_type;
    res = vkAllocateMemory (device->device, &alloc_info, NULL, &img->memories[0]);
    if (res != VK_SUCCESS)
      {
        cogl_vk_image_free (img);
        return NULL;
      }
    img->n_memories = 1;
    res = vkBindImageMemory (device->device, img->image, img->memories[0], 0);
    if (res != VK_SUCCESS)
      {
        cogl_vk_device_note_vk_result (device, res);
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "vkBindImageMemory (MS depth/stencil) failed (%d)", (int) res);
        cogl_vk_image_free (img);
        return NULL;
      }
    if (!create_image_view (img, VK_NULL_HANDLE, error))
      {
        cogl_vk_image_free (img);
        return NULL;
      }
    return img;
  }
}

CoglVkImage *
cogl_vk_image_new_depth_stencil (CoglVkDevice          *device,
                                 uint32_t               width,
                                 uint32_t               height,
                                 VkSampleCountFlagBits  samples,
                                 GError               **error)
{
  static const VkFormat candidates[] = {
    VK_FORMAT_D24_UNORM_S8_UINT,
    VK_FORMAT_D32_SFLOAT_S8_UINT,
    VK_FORMAT_D16_UNORM_S8_UINT,
  };
  unsigned int i;

  g_return_val_if_fail (device != NULL, NULL);

  if (samples == 0)
    samples = VK_SAMPLE_COUNT_1_BIT;

  for (i = 0; i < G_N_ELEMENTS (candidates); i++)
    {
      if (!format_supports_depth_stencil (device, candidates[i]))
        continue;

      return new_depth_stencil_format (device, width, height, candidates[i],
                                       samples, error);
    }

  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "No packed depth/stencil VkFormat available");
  return NULL;
}

/* Depth-only RB — GL COGL_OFFSCREEN_ALLOCATE_FLAG_DEPTH (D16). */
CoglVkImage *
cogl_vk_image_new_depth (CoglVkDevice          *device,
                         uint32_t               width,
                         uint32_t               height,
                         VkSampleCountFlagBits  samples,
                         GError               **error)
{
  static const VkFormat candidates[] = {
    VK_FORMAT_D16_UNORM,
    VK_FORMAT_D32_SFLOAT,
    VK_FORMAT_X8_D24_UNORM_PACK32,
  };
  unsigned int i;

  g_return_val_if_fail (device != NULL, NULL);

  if (samples == 0)
    samples = VK_SAMPLE_COUNT_1_BIT;

  for (i = 0; i < G_N_ELEMENTS (candidates); i++)
    {
      if (!format_supports_depth_stencil (device, candidates[i]))
        continue;

      return new_depth_stencil_format (device, width, height, candidates[i],
                                       samples, error);
    }

  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "No depth-only VkFormat available");
  return NULL;
}

/* Stencil-only RB — GL COGL_OFFSCREEN_ALLOCATE_FLAG_STENCIL (S8). */
CoglVkImage *
cogl_vk_image_new_stencil (CoglVkDevice          *device,
                           uint32_t               width,
                           uint32_t               height,
                           VkSampleCountFlagBits  samples,
                           GError               **error)
{
  g_return_val_if_fail (device != NULL, NULL);

  if (samples == 0)
    samples = VK_SAMPLE_COUNT_1_BIT;

  if (!format_supports_depth_stencil (device, VK_FORMAT_S8_UINT))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "No stencil-only VkFormat available");
      return NULL;
    }

  return new_depth_stencil_format (device, width, height, VK_FORMAT_S8_UINT,
                                   samples, error);
}

static VkImageAspectFlagBits
mem_plane_aspect (unsigned i)
{
  switch (i)
    {
    case 0:
      return VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
    case 1:
      return VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT;
    case 2:
      return VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT;
    case 3:
      return VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT;
    default:
      g_assert_not_reached ();
      return VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
    }
}

/* Planes backed by different DMA-BUF objects need disjoint memory binding. */
static gboolean
is_dmabuf_disjoint (int        n_planes,
                    const int *fds)
{
  struct stat first_stat;
  int i;

  if (n_planes <= 1)
    return FALSE;

  if (fstat (fds[0], &first_stat) != 0)
    return TRUE;

  for (i = 1; i < n_planes; i++)
    {
      struct stat plane_stat;

      if (fds[i] < 0)
        continue;
      if (fstat (fds[i], &plane_stat) != 0)
        return TRUE;
      if (first_stat.st_ino != plane_stat.st_ino ||
          first_stat.st_dev != plane_stat.st_dev)
        return TRUE;
    }

  return FALSE;
}

static gboolean
create_image_view (CoglVkImage              *img,
                   VkSamplerYcbcrConversion  ycbcr_conversion,
                   GError                  **error)
{
  VkSamplerYcbcrConversionInfo ycbcr_info = {
    .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
    .conversion = ycbcr_conversion,
  };
  VkImageViewCreateInfo view_info = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
    .image = img->image,
    .viewType = VK_IMAGE_VIEW_TYPE_2D,
    .format = img->format,
    .subresourceRange = {
      .aspectMask = aspect_from_vk_format (img->format),
      .baseMipLevel = 0,
      /* Sample/FB views only expose level 0. Higher mips stay UNDEFINED
       * until generate_mipmap finishes; a full-chain view trips
       * VUID-vkCmdDraw-None-09600 (wrap-modes). Sampler maxLod already
       * clamps to base level for the default hot-path sampler.
       */
      .levelCount = 1,
      .layerCount = 1,
    },
  };
  VkResult res;

  /* Dmabuf / unset cogl_format: identity (fourcc matches VkFormat). */
  if (img->drm_fourcc)
    cogl_vk_format_swizzle_from_drm (img->drm_fourcc, &view_info.components);
  else if (img->cogl_format != COGL_PIXEL_FORMAT_ANY)
    cogl_vk_format_swizzle_from_cogl (img->cogl_format, &view_info.components);
  else
    {
      view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
      view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
      view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
      view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    }

  if (ycbcr_conversion != VK_NULL_HANDLE)
    {
      view_info.pNext = &ycbcr_info;
      img->is_ycbcr = TRUE;
    }

  res = vkCreateImageView (img->device->device, &view_info, NULL, &img->view);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkCreateImageView failed (%d)", (int) res);
      return FALSE;
    }
  cogl_vk_debug_log ("tex",
                     "image_view created img=%p %ux%u format=%u image=%p view=%p",
                     (void *) img, img->width, img->height, img->format,
                     (void *) (uintptr_t) img->image,
                     (void *) (uintptr_t) img->view);
  return TRUE;
}

gboolean
cogl_vk_image_ensure_attachment_view (CoglVkImage *image,
                                      GError     **error)
{
  VkImageViewCreateInfo info = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
    .image = image->image,
    .viewType = VK_IMAGE_VIEW_TYPE_2D,
    .format = image->format,
    .subresourceRange = {
      .aspectMask = aspect_from_vk_format (image->format),
      .levelCount = 1,
      .layerCount = 1,
    },
  };
  VkResult result;

  if (image->attachment_view != VK_NULL_HANDLE)
    return TRUE;

  /* Sampling may reorder channels; framebuffer attachments require
   * identity component mappings (VUID-pAttachments-00884).
   */
  result = vkCreateImageView (image->device->device, &info, NULL,
                              &image->attachment_view);
  if (result != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkCreateImageView (attachment) failed (%d)", result);
      return FALSE;
    }
  return TRUE;
}

static CoglVkImage *
image_new_optimal_mips_full (CoglVkDevice            *device,
                             uint32_t                 width,
                             uint32_t                 height,
                             uint32_t                 mip_levels,
                             VkFormat                 format,
                             CoglPixelFormat          cogl_format,
                             VkImageUsageFlags        usage,
                             VkSamplerYcbcrConversion conversion,
                             GError                 **error)
{
  CoglVkImage *img;
  VkImageCreateInfo image_info;
  VkMemoryRequirements reqs;
  VkMemoryAllocateInfo alloc_info;
  int mem_type;
  VkResult res;

  g_return_val_if_fail (device != NULL, NULL);
  g_return_val_if_fail (format != VK_FORMAT_UNDEFINED, NULL);

  if (mip_levels < 1)
    mip_levels = 1;

  img = image_alloc_common (device, width, height, format, cogl_format);
  img->mip_levels = mip_levels;

  memset (&image_info, 0, sizeof (image_info));
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent.width = width;
  image_info.extent.height = height;
  image_info.extent.depth = 1;
  image_info.mipLevels = mip_levels;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = usage;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  res = vkCreateImage (device->device, &image_info, NULL, &img->image);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkCreateImage failed (%d)", (int) res);
      g_free (img);
      return NULL;
    }

  vkGetImageMemoryRequirements (device->device, img->image, &reqs);
  mem_type = find_memory_type (device, reqs.memoryTypeBits,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (mem_type < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No DEVICE_LOCAL memory type for VkImage");
      cogl_vk_image_free (img);
      return NULL;
    }

  memset (&alloc_info, 0, sizeof (alloc_info));
  alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc_info.allocationSize = reqs.size;
  alloc_info.memoryTypeIndex = (uint32_t) mem_type;

  res = vkAllocateMemory (device->device, &alloc_info, NULL, &img->memories[0]);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkAllocateMemory failed (%d)", (int) res);
      cogl_vk_image_free (img);
      return NULL;
    }
  img->n_memories = 1;

  res = vkBindImageMemory (device->device, img->image, img->memories[0], 0);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkBindImageMemory failed (%d)", (int) res);
      cogl_vk_image_free (img);
      return NULL;
    }

  if (!create_image_view (img, conversion, error))
    {
      cogl_vk_image_free (img);
      return NULL;
    }

  return img;
}

CoglVkImage *
cogl_vk_image_new_optimal_mips (CoglVkDevice     *device,
                                uint32_t          width,
                                uint32_t          height,
                                uint32_t          mip_levels,
                                VkFormat          format,
                                CoglPixelFormat   cogl_format,
                                VkImageUsageFlags usage,
                                GError          **error)
{
  return image_new_optimal_mips_full (device, width, height, mip_levels, format,
                                      cogl_format, usage, VK_NULL_HANDLE, error);
}

CoglVkImage *
cogl_vk_image_new_nv12 (CoglVkDevice            *device,
                        uint32_t                 width,
                        uint32_t                 height,
                        VkSamplerYcbcrConversion conversion,
                        GError                 **error)
{
  return image_new_optimal_mips_full (
    device, width, height, 1, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
    COGL_PIXEL_FORMAT_ANY,
    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    conversion, error);
}

gboolean
cogl_vk_image_set_ycbcr_conversion (CoglVkImage             *image,
                                    VkSamplerYcbcrConversion conversion,
                                    GError                 **error)
{
  CoglVkImage replacement = *image;

  replacement.view = VK_NULL_HANDLE;
  if (!create_image_view (&replacement, conversion, error))
    return FALSE;
  vkDestroyImageView (image->device->device, image->view, NULL);
  image->view = replacement.view;
  return TRUE;
}

CoglVkImage *
cogl_vk_image_new_optimal (CoglVkDevice     *device,
                           uint32_t          width,
                           uint32_t          height,
                           VkFormat          format,
                           CoglPixelFormat   cogl_format,
                           VkImageUsageFlags usage,
                           GError          **error)
{
  return cogl_vk_image_new_optimal_samples (device, width, height, format,
                                            cogl_format, usage,
                                            VK_SAMPLE_COUNT_1_BIT, error);
}

CoglVkImage *
cogl_vk_image_new_optimal_samples (CoglVkDevice          *device,
                                   uint32_t               width,
                                   uint32_t               height,
                                   VkFormat               format,
                                   CoglPixelFormat        cogl_format,
                                   VkImageUsageFlags      usage,
                                   VkSampleCountFlagBits  samples,
                                   GError               **error)
{
  CoglVkImage *img;

  if (samples <= VK_SAMPLE_COUNT_1_BIT)
    {
      return cogl_vk_image_new_optimal_mips (device, width, height, 1,
                                             format, cogl_format, usage,
                                             error);
    }

  /* MSAA color: no mipmaps; color attachment + transfer for resolve. */
  img = cogl_vk_image_new_optimal_mips (device, width, height, 1,
                                        format, cogl_format,
                                        usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                        error);
  if (!img)
    return NULL;

  /* Recreate with real sample count — optimal_mips always uses 1 sample.
   * Free and allocate properly:
   */
  {
    CoglVkDevice *dev = device;
    VkImageCreateInfo image_info;
    VkMemoryRequirements reqs;
    VkMemoryAllocateInfo alloc_info;
    int mem_type;
    VkResult res;

    cogl_vk_image_free (img);
    img = image_alloc_common (dev, width, height, format, cogl_format);
    img->samples = samples;

    memset (&image_info, 0, sizeof (image_info));
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent.width = width;
    image_info.extent.height = height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = samples;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    res = vkCreateImage (dev->device, &image_info, NULL, &img->image);
    if (res != VK_SUCCESS)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "vkCreateImage (MSAA) failed (%d)", (int) res);
        g_free (img);
        return NULL;
      }
    vkGetImageMemoryRequirements (dev->device, img->image, &reqs);
    mem_type = find_memory_type (dev, reqs.memoryTypeBits,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type < 0)
      {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "No DEVICE_LOCAL memory for MSAA image");
        cogl_vk_image_free (img);
        return NULL;
      }
    memset (&alloc_info, 0, sizeof (alloc_info));
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = reqs.size;
    alloc_info.memoryTypeIndex = (uint32_t) mem_type;
    res = vkAllocateMemory (dev->device, &alloc_info, NULL, &img->memories[0]);
    if (res != VK_SUCCESS)
      {
        cogl_vk_image_free (img);
        return NULL;
      }
    img->n_memories = 1;
    vkBindImageMemory (dev->device, img->image, img->memories[0], 0);
    if (!create_image_view (img, VK_NULL_HANDLE, error))
      {
        cogl_vk_image_free (img);
        return NULL;
      }
  }
  return img;
}

gboolean
cogl_vk_image_recreate_view (CoglVkImage *image,
                             GError     **error)
{
  g_return_val_if_fail (image != NULL, FALSE);

  if (image->view != VK_NULL_HANDLE)
    {
      /* Destroying a view still bound in a recording CB invalidates it. */
      if (image->device && image->device->owner)
        cogl_driver_vk_flush_current_pass (image->device->owner);
      if (image->device && image->device->graphics_queue != VK_NULL_HANDLE)
        vkQueueWaitIdle (image->device->graphics_queue);
      vkDestroyImageView (image->device->device, image->view, NULL);
      image->view = VK_NULL_HANDLE;
    }

  return create_image_view (image, VK_NULL_HANDLE, error);
}

CoglVkImage *
cogl_vk_image_new_host_visible (CoglVkDevice   *device,
                                uint32_t        width,
                                uint32_t        height,
                                VkFormat        format,
                                CoglPixelFormat cogl_format,
                                const uint8_t  *pixels,
                                uint32_t        rowstride,
                                GError        **error)
{
  CoglVkImage *img;
  VkImageCreateInfo image_info;
  VkMemoryRequirements reqs;
  VkMemoryAllocateInfo alloc_info;
  int mem_type;
  void *mapped = NULL;
  VkResult res;
  uint32_t y;
  uint32_t bpp = 4;

  g_return_val_if_fail (device != NULL, NULL);

  if (cogl_format != COGL_PIXEL_FORMAT_ANY)
    bpp = cogl_pixel_format_get_bytes_per_pixel (cogl_format, 0);

  /* RGB_888 maps to R8G8B8A8 (sampleable); expand 3→4 on upload. */
  {
    uint32_t vk_bpp = 4;

    switch (format)
      {
      case VK_FORMAT_R8_UNORM:
      case VK_FORMAT_R8_SRGB:
        vk_bpp = 1;
        break;
      case VK_FORMAT_R8G8_UNORM:
        vk_bpp = 2;
        break;
      case VK_FORMAT_R8G8B8_UNORM:
      case VK_FORMAT_B8G8R8_UNORM:
      case VK_FORMAT_R8G8B8_SRGB:
      case VK_FORMAT_B8G8R8_SRGB:
        vk_bpp = 3;
        break;
      default:
        vk_bpp = 4;
        break;
      }

    if (bpp == 3 && vk_bpp == 4)
      {
        cogl_vk_debug_log (
          "tex",
          "upload expand RGB888→RGBA8 %ux%u vk_fmt=%d cogl_fmt=%d "
          "sampled_lin=%d",
          width, height, (int) format, (int) cogl_format,
          vk_format_sampled_linear (device, format));
      }
    else if (pixels && !vk_format_sampled_linear (device, format))
      {
        cogl_vk_debug_log (
          "tex",
          "WARN host image vk_fmt=%d not SAMPLED on linear tiling "
          "(cogl_fmt=%d) — sampling may be undefined",
          (int) format, (int) cogl_format);
      }
  }

  img = image_alloc_common (device, width, height, format, cogl_format);

  memset (&image_info, 0, sizeof (image_info));
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent.width = width;
  image_info.extent.height = height;
  image_info.extent.depth = 1;
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_LINEAR;
  image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (vk_format_color_attachment_linear (device, format))
    {
      image_info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      cogl_vk_debug_log (
        "tex",
        "host_visible COLOR_ATTACHMENT added vk_fmt=%d",
        (int) format);
    }
  else
    {
      cogl_vk_debug_log (
        "tex",
        "host_visible COLOR_ATTACHMENT skipped vk_fmt=%d "
        "(linear feature missing)",
        (int) format);
    }
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;

  res = vkCreateImage (device->device, &image_info, NULL, &img->image);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkCreateImage (linear) failed (%d)", (int) res);
      g_free (img);
      return NULL;
    }

  vkGetImageMemoryRequirements (device->device, img->image, &reqs);
  mem_type = find_memory_type (device, reqs.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (mem_type < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No HOST_VISIBLE memory type for linear VkImage");
      cogl_vk_image_free (img);
      return NULL;
    }

  memset (&alloc_info, 0, sizeof (alloc_info));
  alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc_info.allocationSize = reqs.size;
  alloc_info.memoryTypeIndex = (uint32_t) mem_type;

  res = vkAllocateMemory (device->device, &alloc_info, NULL, &img->memories[0]);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkAllocateMemory failed (%d)", (int) res);
      cogl_vk_image_free (img);
      return NULL;
    }
  img->n_memories = 1;

  res = vkBindImageMemory (device->device, img->image, img->memories[0], 0);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkBindImageMemory failed (%d)", (int) res);
      cogl_vk_image_free (img);
      return NULL;
    }

  if (pixels)
    {
      VkImageSubresource sub = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      };
      VkSubresourceLayout layout;

      vkGetImageSubresourceLayout (device->device, img->image, &sub, &layout);
      res = vkMapMemory (device->device, img->memories[0], 0, reqs.size, 0,
                         &mapped);
      if (res != VK_SUCCESS)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "vkMapMemory failed (%d)", (int) res);
          cogl_vk_image_free (img);
          return NULL;
        }

      for (y = 0; y < height; y++)
        {
          uint8_t *dst_row =
            (uint8_t *) mapped + layout.offset + y * layout.rowPitch;
          const uint8_t *src_row = pixels + y * rowstride;

          if (bpp == 3 &&
              (format == VK_FORMAT_R8G8B8A8_UNORM ||
               format == VK_FORMAT_R8G8B8A8_SRGB ||
               format == VK_FORMAT_B8G8R8A8_UNORM ||
               format == VK_FORMAT_B8G8R8A8_SRGB))
            {
              uint32_t x;

              for (x = 0; x < width; x++)
                {
                  dst_row[x * 4 + 0] = src_row[x * 3 + 0];
                  dst_row[x * 4 + 1] = src_row[x * 3 + 1];
                  dst_row[x * 4 + 2] = src_row[x * 3 + 2];
                  dst_row[x * 4 + 3] = 0xff;
                }
            }
          else
            {
              memcpy (dst_row, src_row, width * bpp);
            }
        }
      vkUnmapMemory (device->device, img->memories[0]);
    }

  img->layout = VK_IMAGE_LAYOUT_PREINITIALIZED;
  img->host_visible = TRUE;

  if (!create_image_view (img, VK_NULL_HANDLE, error))
    {
      cogl_vk_image_free (img);
      return NULL;
    }

  return img;
}

static gboolean
dmabuf_linear_bind_offset (const VkSubresourceLayout *layouts,
                           int                        n_planes,
                           const uint32_t            *strides,
                           const uint32_t            *offsets,
                           VkDeviceSize              *bind_offset,
                           GError                   **error)
{
  if (offsets[0] < layouts[0].offset)
    goto incompatible;

  *bind_offset = offsets[0] - layouts[0].offset;
  for (int i = 0; i < n_planes; i++)
    if (strides[i] != layouts[i].rowPitch ||
        offsets[i] < layouts[i].offset ||
        offsets[i] - layouts[i].offset != *bind_offset)
      goto incompatible;
  return TRUE;

incompatible:
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "dma-buf layout does not match Vulkan linear image layout");
  return FALSE;
}

CoglVkImage *
cogl_vk_image_import_dmabuf (CoglVkDevice            *device,
                             uint32_t                 width,
                             uint32_t                 height,
                             uint32_t                 drm_fourcc,
                             int                      n_planes,
                             const int               *fds,
                             const uint32_t          *strides,
                             const uint32_t          *offsets,
                             const uint64_t          *modifiers,
                             VkSamplerYcbcrConversion ycbcr_conversion,
                             GError                 **error)
{
  CoglVkImage *img;
  VkFormat format;
  uint64_t modifier;
  gboolean disjoint;
  gboolean want_ycbcr;
  VkImageDrmFormatModifierExplicitCreateInfoEXT mod_info;
  VkExternalMemoryImageCreateInfo ext_img = {
    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
  };
  VkImageCreateInfo image_info;
  VkSubresourceLayout plane_layouts[4];
  VkBindImageMemoryInfo bind_infos[4];
  VkBindImagePlaneMemoryInfo plane_infos[4];
  unsigned mem_count;
  unsigned i;
  VkResult res;
  VkDeviceSize linear_bind_offset = 0;

  g_return_val_if_fail (device != NULL, NULL);
  g_return_val_if_fail (n_planes > 0 && n_planes <= 4, NULL);
  g_return_val_if_fail (fds != NULL && strides != NULL && offsets != NULL, NULL);

  if (!device->has_dma_buf || !device->vkGetMemoryFdPropertiesKHR)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Vulkan dmabuf import unavailable");
      return NULL;
    }

  format = cogl_vk_format_from_drm_fourcc (drm_fourcc);
  if (format == VK_FORMAT_UNDEFINED)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Unsupported DRM fourcc 0x%x for Vulkan import", drm_fourcc);
      return NULL;
    }

  want_ycbcr = (format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
  if (want_ycbcr && n_planes != 2)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "NV12 requires two dma-buf planes");
      return NULL;
    }
  if (want_ycbcr && ycbcr_conversion == VK_NULL_HANDLE)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "NV12 import requires a sampler YCbCr conversion");
      return NULL;
    }

  modifier = modifiers ? modifiers[0] : DRM_FORMAT_MOD_INVALID;
  if (modifier == DRM_FORMAT_MOD_INVALID)
    modifier = DRM_FORMAT_MOD_LINEAR;

  if (!device->has_drm_modifiers && modifier != DRM_FORMAT_MOD_LINEAR)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "DRM format modifiers required but unavailable");
      return NULL;
    }

  /* Bind plane memory separately when DMA-BUF planes use different objects. */
  disjoint = is_dmabuf_disjoint (n_planes, fds);
  if (disjoint && !device->has_drm_modifiers)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "disjoint dmabuf requires DRM format modifiers");
      return NULL;
    }

  /* Imported views use DRM channel order instead of a Cogl upload format. */
  img = image_alloc_common (device, width, height, format, 0);
  img->is_dmabuf = TRUE;
  img->drm_fourcc = drm_fourcc;
  /* External producers hand off dma-bufs in GENERAL. This is distinct from
   * the initialLayout used when creating the importing Vulkan image.
   */
  img->layout = VK_IMAGE_LAYOUT_GENERAL;

  memset (plane_layouts, 0, sizeof (plane_layouts));
  for (i = 0; i < (unsigned) n_planes; i++)
    {
      plane_layouts[i].offset = offsets[i];
      plane_layouts[i].rowPitch = strides[i];
    }

  memset (&mod_info, 0, sizeof (mod_info));
  mod_info.sType =
    VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
  mod_info.drmFormatModifier = modifier;
  mod_info.drmFormatModifierPlaneCount = (uint32_t) n_planes;
  mod_info.pPlaneLayouts = plane_layouts;
  mod_info.pNext = &ext_img;

  memset (&image_info, 0, sizeof (image_info));
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.pNext = device->has_drm_modifiers
                     ? (const void *) &mod_info
                     : (const void *) &ext_img;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent.width = width;
  image_info.extent.height = height;
  image_info.extent.depth = 1;
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = device->has_drm_modifiers
                      ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT
                      : VK_IMAGE_TILING_LINEAR;
  image_info.usage = cogl_vk_dmabuf_image_usage (drm_fourcc);
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  /* External-memory images require UNDEFINED at creation (01443). Preserve
   * the producer's contents by acquiring from GENERAL, never UNDEFINED.
   */
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (disjoint)
    image_info.flags = VK_IMAGE_CREATE_DISJOINT_BIT;

  res = vkCreateImage (device->device, &image_info, NULL, &img->image);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkCreateImage (dmabuf) failed (%d)", (int) res);
      g_free (img);
      return NULL;
    }

  if (!device->has_drm_modifiers)
    {
      VkSubresourceLayout layouts[4];

      for (i = 0; i < (unsigned) n_planes; i++)
        {
          VkImageSubresource subresource = {
            .aspectMask = want_ycbcr ? VK_IMAGE_ASPECT_PLANE_0_BIT << i :
                          VK_IMAGE_ASPECT_COLOR_BIT,
          };
          vkGetImageSubresourceLayout (device->device, img->image,
                                       &subresource, &layouts[i]);
        }
      if (!dmabuf_linear_bind_offset (layouts, n_planes, strides, offsets,
                                      &linear_bind_offset, error))
        {
          cogl_vk_image_free (img);
          return NULL;
        }
    }

  mem_count = disjoint ? (unsigned) n_planes : 1u;
  memset (bind_infos, 0, sizeof (bind_infos));
  memset (plane_infos, 0, sizeof (plane_infos));

  for (i = 0; i < mem_count; i++)
    {
      VkMemoryFdPropertiesKHR fd_props = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
      };
      VkImageMemoryRequirementsInfo2 mem_req_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = img->image,
      };
      VkImagePlaneMemoryRequirementsInfo plane_req = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO,
      };
      VkMemoryDedicatedRequirements dedicated_reqs = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
      };
      VkMemoryRequirements2 mem_reqs2 = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = &dedicated_reqs,
      };
      VkImportMemoryFdInfoKHR import_info;
      VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      };
      VkMemoryAllocateInfo alloc_info;
      int mem_type;
      int dup_fd;
      int plane_fd = fds[disjoint ? i : 0];

      dup_fd = fcntl (plane_fd, F_DUPFD_CLOEXEC, 0);
      if (dup_fd < 0)
        dup_fd = dup (plane_fd);
      if (dup_fd < 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Failed to dup dmabuf fd");
          cogl_vk_image_free (img);
          return NULL;
        }

      res = device->vkGetMemoryFdPropertiesKHR (
        device->device,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        dup_fd,
        &fd_props);
      if (res != VK_SUCCESS)
        {
          close (dup_fd);
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "vkGetMemoryFdPropertiesKHR failed (%d)", (int) res);
          cogl_vk_image_free (img);
          return NULL;
        }

      if (disjoint)
        {
          plane_req.planeAspect = mem_plane_aspect (i);
          mem_req_info.pNext = &plane_req;
        }

      vkGetImageMemoryRequirements2 (device->device, &mem_req_info, &mem_reqs2);
      if (linear_bind_offset &&
          (dedicated_reqs.requiresDedicatedAllocation ||
           linear_bind_offset % mem_reqs2.memoryRequirements.alignment != 0))
        {
          close (dup_fd);
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                               "dma-buf plane offset cannot be bound to this Vulkan image");
          cogl_vk_image_free (img);
          return NULL;
        }
      mem_type = find_memory_type (
        device,
        mem_reqs2.memoryRequirements.memoryTypeBits & fd_props.memoryTypeBits,
        0);
      if (mem_type < 0)
        {
          close (dup_fd);
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "No compatible memory type for dmabuf plane %u", i);
          cogl_vk_image_free (img);
          return NULL;
        }

      memset (&import_info, 0, sizeof (import_info));
      import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
      import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
      import_info.fd = dup_fd;

      dedicated.image = img->image;
      dedicated.pNext = &import_info;

      memset (&alloc_info, 0, sizeof (alloc_info));
      alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      alloc_info.pNext = linear_bind_offset ? (void *) &import_info : (void *) &dedicated;
      alloc_info.allocationSize = linear_bind_offset + mem_reqs2.memoryRequirements.size;
      alloc_info.memoryTypeIndex = (uint32_t) mem_type;

      res = vkAllocateMemory (device->device, &alloc_info, NULL,
                              &img->memories[i]);
      if (res != VK_SUCCESS)
        {
          close (dup_fd);
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "vkAllocateMemory (import dmabuf) failed (%d)",
                       (int) res);
          cogl_vk_image_free (img);
          return NULL;
        }
      img->n_memories = i + 1;

      bind_infos[i].sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO;
      bind_infos[i].image = img->image;
      bind_infos[i].memory = img->memories[i];
      bind_infos[i].memoryOffset = linear_bind_offset;
      if (disjoint)
        {
          plane_infos[i].sType = VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO;
          plane_infos[i].planeAspect = mem_plane_aspect (i);
          bind_infos[i].pNext = &plane_infos[i];
        }
    }

  res = vkBindImageMemory2 (device->device, mem_count, bind_infos);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "vkBindImageMemory2 (dmabuf) failed (%d)", (int) res);
      cogl_vk_image_free (img);
      return NULL;
    }

  if (!create_image_view (img,
                          want_ycbcr ? ycbcr_conversion : VK_NULL_HANDLE,
                          error))
    {
      cogl_vk_image_free (img);
      return NULL;
    }

  {
    static int n_import_log;
    struct stat st;

    if (n_import_log < 8 && fds && fstat (fds[0], &st) == 0)
      {
        n_import_log++;
        g_printerr ("VKIMPORT ino=%lu size=%ld vkfmt=%d mod=0x%llx %ux%u planes=%d\n",
                    (unsigned long) st.st_ino, (long) st.st_size,
                    (int) format, (unsigned long long) modifier,
                    width, height, n_planes);
        img->debug_dmabuf_ino = (uint64_t) st.st_ino;
      }
  }

  return img;
}

static void
access_for_layout (VkImageLayout          layout,
                   VkPipelineStageFlags  *out_stage,
                   VkAccessFlags         *out_access);

static void
note_recorded_image_state (CoglVkImage    *image,
                           VkCommandBuffer command_buffer)
{
  CoglDriverVk *driver = image->device->owner;
  if (driver && driver->current_pass &&
      driver->current_pass->command_buffer == command_buffer)
    cogl_vk_pass_note_image_state (driver->current_pass, image);
}

void
cogl_vk_image_cmd_barrier (CoglVkImage         *image,
                           VkCommandBuffer      cmd,
                           VkImageLayout        new_layout,
                           VkPipelineStageFlags src_stage,
                           VkPipelineStageFlags dst_stage,
                           VkAccessFlags        src_access,
                           VkAccessFlags        dst_access)
{
  VkImageMemoryBarrier barrier;

  g_return_if_fail (image != NULL);

  image->gpu_used = TRUE;

  /* Imported scanout/client dmabufs stay owned by the display until the
   * graphics queue does a FOREIGN acquire. A plain barrier leaves eDP
   * reading zeros.
   */
  if (image->is_dmabuf &&
      image->device->has_queue_family_foreign &&
      !image->dmabuf_graphics_owned)
    {
      cogl_vk_image_cmd_foreign_acquire (image, cmd,
                                         image->device->graphics_queue_family,
                                         new_layout);
      return;
    }

  cogl_vk_debug_log (
    "barrier",
    "img_barrier img=%p %ux%u layout %d→%d src_stage=0x%x dst_stage=0x%x "
    "src_access=0x%x dst_access=0x%x",
    (void *) image, image->width, image->height,
    (int) image->layout, (int) new_layout,
    (unsigned) src_stage, (unsigned) dst_stage,
    (unsigned) src_access, (unsigned) dst_access);

  note_recorded_image_state (image, cmd);

  /* Even when layout is unchanged we still need an execution/memory
   * dependency (e.g. color LOAD after a prior submit's STORE).
   */
  memset (&barrier, 0, sizeof (barrier));
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = dst_access;
  barrier.oldLayout = image->layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image->image;
  barrier.subresourceRange.aspectMask = aspect_from_vk_format (image->format);
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = MAX (1u, image->mip_levels);
  barrier.subresourceRange.layerCount = 1;

  vkCmdPipelineBarrier (cmd, src_stage, dst_stage, 0,
                        0, NULL, 0, NULL, 1, &barrier);
  image->layout = new_layout;
  image->shader_read_synced = FALSE;
}

static void
access_for_layout (VkImageLayout          layout,
                   VkPipelineStageFlags  *out_stage,
                   VkAccessFlags         *out_access)
{
  switch (layout)
    {
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
      *out_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
      *out_access = VK_ACCESS_TRANSFER_READ_BIT;
      break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
      *out_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
      *out_access = VK_ACCESS_TRANSFER_WRITE_BIT;
      break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
      *out_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      *out_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
      break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    default:
      *out_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      *out_access = VK_ACCESS_SHADER_READ_BIT;
      break;
    }
}

void
cogl_vk_image_cmd_foreign_acquire (CoglVkImage    *image,
                                   VkCommandBuffer cmd,
                                   uint32_t        graphics_queue_family,
                                   VkImageLayout   new_layout)
{
  VkImageMemoryBarrier barrier;
  VkImageLayout src_layout;
  VkPipelineStageFlags dst_stage;
  VkAccessFlags dst_access;

  g_return_if_fail (image != NULL);
  note_recorded_image_state (image, cmd);

  access_for_layout (new_layout, &dst_stage, &dst_access);

  if (!image->is_dmabuf || !image->device->has_queue_family_foreign)
    {
      cogl_vk_image_cmd_barrier (image, cmd, new_layout,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 dst_stage, 0, dst_access);
      return;
    }

  /* Both the first import and later external uses arrive in GENERAL. */
  src_layout = VK_IMAGE_LAYOUT_GENERAL;

  memset (&barrier, 0, sizeof (barrier));
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
  barrier.dstQueueFamilyIndex = graphics_queue_family;
  barrier.image = image->image;
  barrier.oldLayout = src_layout;
  barrier.newLayout = new_layout;
  barrier.srcAccessMask = 0;
  barrier.dstAccessMask = dst_access;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = MAX (1u, image->mip_levels);
  barrier.subresourceRange.layerCount = 1;

  vkCmdPipelineBarrier (cmd,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        dst_stage,
                        0, 0, NULL, 0, NULL, 1, &barrier);
  image->layout = new_layout;
  image->shader_read_synced = FALSE;
  image->dmabuf_graphics_owned = TRUE;
}

void
cogl_vk_image_cmd_foreign_release (CoglVkImage    *image,
                                   VkCommandBuffer cmd,
                                   uint32_t        graphics_queue_family,
                                   VkImageLayout   new_layout)
{
  VkImageMemoryBarrier barrier;

  g_return_if_fail (image != NULL);
  note_recorded_image_state (image, cmd);

  if (!image->is_dmabuf || !image->device->has_queue_family_foreign)
    return;

  if (!image->dmabuf_graphics_owned)
    return;

  {
    VkPipelineStageFlags src_stage;
    VkAccessFlags src_access;

    access_for_layout (image->layout, &src_stage, &src_access);

    /* EndRenderPass publishes color stores at BOTTOM_OF_PIPE (the external
     * subpass dependency). A release sourced only at COLOR_ATTACHMENT_OUTPUT
     * does not chain with that, so the FOREIGN layout transition drops the
     * stores and the scanout dma-buf stays the zeros GBM allocated.
     * vkCmdClearColorImage survived because its release source stage is
     * TRANSFER, which matches the clear. ALL_COMMANDS chains with both.
     */
    src_stage |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    src_access |= VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;

    memset (&barrier, 0, sizeof (barrier));
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = graphics_queue_family;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    barrier.image = image->image;
    barrier.oldLayout = image->layout;
    barrier.newLayout = new_layout;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = 0;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier (cmd,
                          src_stage,
                          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                          0, 0, NULL, 0, NULL, 1, &barrier);
  }
  image->layout = new_layout;
  image->shader_read_synced = FALSE;
  image->dmabuf_graphics_owned = FALSE;
}

void
cogl_vk_image_free (CoglVkImage *image)
{
  if (!image)
    return;

  /* Destroying a VkImage still referenced by a recording/submitted CB
   * invalidates that CB (validation: commandBuffer-recording) and leads to
   * ANV DEVICE_LOST. Hand it to the submit that proves it unused instead of
   * draining the queue here: an animated offscreen is reallocated every
   * frame, and a drain per free serialized CPU and GPU (size-change
   * animation ~5 drains per frame). Imported dma-bufs too: a client that
   * reallocates its buffers on resize frees one import per old buffer. The
   * import holds the memory until destroyed, and a recording pass's foreign
   * release still sees the image, which stays allocated until then. */
  if (image->device && image->device->owner)
    {
      CoglVkRetireList *retire;
      CoglVkRetireResult result =
        cogl_driver_vk_retire_list (image->device->owner, &retire);

      if (result == COGL_VK_RETIRE_DEFERRED)
        {
          cogl_vk_debug_log ("tex", "image_free retired img=%p %ux%u",
                             (void *) image, image->width, image->height);
          cogl_vk_retire_list_add_image (retire, image);
          return;
        }
      if (result == COGL_VK_RETIRE_COMPLETE)
        {
          cogl_vk_image_destroy_now (image);
          return;
        }
    }

  /* WaitIdle alone is not enough while the CB is still recording —
   * submit+free the driver's current pass first. */
  if (image->device && image->device->owner)
    cogl_driver_vk_flush_current_pass (image->device->owner);

  if (image->device && image->device->graphics_queue != VK_NULL_HANDLE)
    {
      cogl_vk_debug_log (
        "tex",
        "image_free WaitIdle before destroy img=%p %ux%u view=%p",
        (void *) image, image->width, image->height,
        (void *) (uintptr_t) image->view);
      vkQueueWaitIdle (image->device->graphics_queue);
    }

  cogl_vk_image_destroy_now (image);
}

void
cogl_vk_image_destroy_now (CoglVkImage *image)
{
  uint32_t i;

  g_clear_pointer (&image->cold_rgba, cogl_vk_image_destroy_now);

  if (image->framebuffer != VK_NULL_HANDLE)
    vkDestroyFramebuffer (image->device->device, image->framebuffer, NULL);
  if (image->attachment_view != VK_NULL_HANDLE)
    vkDestroyImageView (image->device->device, image->attachment_view, NULL);
  if (image->view != VK_NULL_HANDLE)
    vkDestroyImageView (image->device->device, image->view, NULL);
  if (image->image != VK_NULL_HANDLE)
    vkDestroyImage (image->device->device, image->image, NULL);
  if (image->owns_memory)
    {
      for (i = 0; i < image->n_memories; i++)
        {
          if (image->memories[i] != VK_NULL_HANDLE)
            vkFreeMemory (image->device->device, image->memories[i], NULL);
        }
    }
  g_free (image);
}

gboolean
cogl_vk_image_oneshot_clear_to_color_attachment (CoglVkImage *image,
                                                 GError     **error)
{
  CoglVkDevice *device;
  VkCommandBufferAllocateInfo cb_alloc = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1,
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
  VkClearColorValue clear = { .float32 = { 0.f, 0.f, 0.f, 0.f } };
  VkImageSubresourceRange range = {
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .levelCount = 1,
    .layerCount = 1,
  };
  VkResult res;

  g_return_val_if_fail (image && image->device, FALSE);
  device = image->device;
  if (image->layout != VK_IMAGE_LAYOUT_UNDEFINED)
    return TRUE;

  {
    VkCommandPool pool = device->oneshot_command_pool != VK_NULL_HANDLE
      ? device->oneshot_command_pool
      : device->command_pool;

    cb_alloc.commandPool = pool;
    range.levelCount = MAX (1u, image->mip_levels);

    res = vkAllocateCommandBuffers (device->device, &cb_alloc, &cmd);
    if (res != VK_SUCCESS)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "oneshot clear AllocateCB failed (%d)", (int) res);
        return FALSE;
      }
    res = vkBeginCommandBuffer (cmd, &begin_info);
    if (res != VK_SUCCESS)
      goto fail_color;

    cogl_vk_image_cmd_barrier (image, cmd,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdClearColorImage (cmd, image->image, image->layout, &clear, 1, &range);
    cogl_vk_image_cmd_barrier (image, cmd,
                               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_ACCESS_TRANSFER_WRITE_BIT,
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);

    /* A dmabuf scanout BO enters here still owned by the display. The first
     * barrier above acquired it to the graphics queue; without a matching
     * release the clear never publishes and KMS keeps scanning out the
     * uninitialized GBM allocation (a full-screen garbage pattern at
     * startup, until later frames overwrite it). Release back to FOREIGN so
     * the clear reaches the display; the next frame's begin_render acquires
     * again. Non-dmabuf images no-op here (foreign release is guarded).
     */
    if (image->is_dmabuf && image->device->has_queue_family_foreign)
      cogl_vk_image_cmd_foreign_release (image, cmd,
                                         image->device->graphics_queue_family,
                                         VK_IMAGE_LAYOUT_GENERAL);

    res = vkEndCommandBuffer (cmd);
    if (res != VK_SUCCESS)
      goto fail_color;
    res = vkCreateFence (device->device, &fence_info, NULL, &fence);
    if (res != VK_SUCCESS)
      goto fail_color;
    memset (&submit_info, 0, sizeof (submit_info));
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;
    res = vkQueueSubmit (device->graphics_queue, 1, &submit_info, fence);
    if (res != VK_SUCCESS)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "oneshot clear QueueSubmit failed (%d)", (int) res);
        goto fail_color;
      }

    /* Queue order makes the clear visible to every later submit, including
     * the frame that first draws to this image, so the CPU need not wait;
     * an animated offscreen is allocated every frame. A dma-buf scanout BO
     * keeps the wait: the display may read it without a later submit. */
    if (!image->is_dmabuf)
      {
        cogl_vk_device_defer_oneshot (device, fence, cmd, pool);
        cogl_vk_debug_log ("tex",
                           "oneshot_clear queued img=%p %ux%u → COLOR_ATTACHMENT",
                           (void *) image, image->width, image->height);
        return TRUE;
      }

    res = vkWaitForFences (device->device, 1, &fence, VK_TRUE, UINT64_MAX);
    if (res != VK_SUCCESS)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "oneshot clear WaitFence failed (%d)", (int) res);
        goto fail_color;
      }

    cogl_vk_debug_log (
      "tex",
      "oneshot_clear OK img=%p %ux%u → COLOR_ATTACHMENT",
      (void *) image, image->width, image->height);

    vkDestroyFence (device->device, fence, NULL);
    vkFreeCommandBuffers (device->device, pool, 1, &cmd);
    return TRUE;

fail_color:
    if (fence != VK_NULL_HANDLE)
      vkDestroyFence (device->device, fence, NULL);
    if (cmd != VK_NULL_HANDLE)
      vkFreeCommandBuffers (device->device, pool, 1, &cmd);
    return FALSE;
  }
}

gboolean
cogl_vk_image_oneshot_clear_to_depth_stencil_attachment (CoglVkImage *image,
                                                         GError     **error)
{
  CoglVkDevice *device;
  VkCommandBufferAllocateInfo cb_alloc = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1,
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
  VkClearDepthStencilValue clear = { .depth = 1.f, .stencil = 0 };
  VkImageSubresourceRange range = {
    .aspectMask = 0,
    .levelCount = 1,
    .layerCount = 1,
  };
  VkResult res;
  int depth_bits = 0, stencil_bits = 0;

  g_return_val_if_fail (image && image->device, FALSE);
  device = image->device;
  if (image->layout != VK_IMAGE_LAYOUT_UNDEFINED)
    return TRUE;

  cogl_vk_format_depth_stencil_bits (image->format, &depth_bits, &stencil_bits);
  if (depth_bits > 0)
    range.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
  if (stencil_bits > 0)
    range.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
  if (range.aspectMask == 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "oneshot DS clear: format has no depth/stencil");
      return FALSE;
    }

  {
    VkCommandPool pool = device->oneshot_command_pool != VK_NULL_HANDLE
      ? device->oneshot_command_pool
      : device->command_pool;

    cb_alloc.commandPool = pool;
    range.levelCount = MAX (1u, image->mip_levels);

    res = vkAllocateCommandBuffers (device->device, &cb_alloc, &cmd);
    if (res != VK_SUCCESS)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "oneshot DS clear AllocateCB failed (%d)", (int) res);
        return FALSE;
      }
    res = vkBeginCommandBuffer (cmd, &begin_info);
    if (res != VK_SUCCESS)
      goto fail_ds;

    cogl_vk_image_cmd_barrier (image, cmd,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdClearDepthStencilImage (cmd, image->image, image->layout,
                                 &clear, 1, &range);
    cogl_vk_image_cmd_barrier (image, cmd,
                               VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                               VK_ACCESS_TRANSFER_WRITE_BIT,
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    res = vkEndCommandBuffer (cmd);
    if (res != VK_SUCCESS)
      goto fail_ds;
    res = vkCreateFence (device->device, &fence_info, NULL, &fence);
    if (res != VK_SUCCESS)
      goto fail_ds;
    memset (&submit_info, 0, sizeof (submit_info));
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;
    res = vkQueueSubmit (device->graphics_queue, 1, &submit_info, fence);
    if (res != VK_SUCCESS)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "oneshot DS clear QueueSubmit failed (%d)", (int) res);
        goto fail_ds;
      }

    /* Later submits are ordered after this clear; retain its fence and
     * command buffer until completion instead of blocking the CPU. */
    cogl_vk_device_defer_oneshot (device, fence, cmd, pool);
    cogl_vk_debug_log (
      "tex",
      "oneshot_clear queued img=%p %ux%u → DEPTH_STENCIL_ATTACHMENT",
      (void *) image, image->width, image->height);
    return TRUE;

fail_ds:
    if (fence != VK_NULL_HANDLE)
      vkDestroyFence (device->device, fence, NULL);
    if (cmd != VK_NULL_HANDLE)
      vkFreeCommandBuffers (device->device, pool, 1, &cmd);
    return FALSE;
  }
}
