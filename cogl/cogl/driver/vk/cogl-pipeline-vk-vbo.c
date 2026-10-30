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
#include "cogl/driver/vk/cogl-buffer-impl-vk-private.h"
#include "cogl/driver/vk/cogl-driver-vk-private.h"
#include "cogl/driver/vk/cogl-pipeline-vk-glsl-private.h"
#include "cogl/driver/vk/cogl-pipeline-vk-assemble-private.h"
#include "cogl/driver/vk/cogl-texture-driver-vk-private.h"
#include "cogl/driver/vk/cogl-vk-debug-private.h"

#include "cogl/cogl-attribute-buffer.h"
#include "cogl/cogl-attribute-private.h"
#include "cogl/cogl-buffer-private.h"
#include "cogl/cogl-color.h"
#include "cogl/cogl-context-private.h"
#include "cogl/cogl-index-buffer.h"
#include "cogl/cogl-indices-private.h"
#include "cogl/cogl-pipeline-layer-private.h"
#include "cogl/cogl-pipeline-private.h"
#include "cogl/cogl-pipeline-state.h"
#include "cogl/cogl-pipeline-state-private.h"

#include <graphene.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

typedef struct
{
  unsigned int has_position : 1;
  unsigned int has_color : 1;
  unsigned int has_point_size : 1;
  unsigned int has_normal : 1;
  unsigned int normalized_color : 1;
  unsigned int normalized_normal : 1;
  gboolean has_texcoord0;
  int n_texcoords; /* slot 0 always represents texcoord0 */
  int *texcoord_layers;
  int n_custom;
  int custom_base;
  int n_bindings;
  int position_components;
  int color_components;
  int normal_components;
  int point_size_components;
  int *texcoord_components;
  int *custom_components;
  CoglAttributeType position_type;
  CoglAttributeType color_type;
  CoglAttributeType normal_type;
  CoglAttributeType *texcoord_type;
  CoglAttributeType *custom_type;
  gboolean *custom_normalized;
  int *custom_name_index;
  uint32_t *strides;
} CoglVkVboMask;

static void
vbo_mask_clear (CoglVkVboMask *mask)
{
  g_free (mask->texcoord_layers);
  g_free (mask->texcoord_components);
  g_free (mask->texcoord_type);
  g_free (mask->custom_components);
  g_free (mask->custom_type);
  g_free (mask->custom_normalized);
  g_free (mask->custom_name_index);
  g_free (mask->strides);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (CoglVkVboMask, vbo_mask_clear)

typedef struct
{
  VkFormat format;
  VkSampleCountFlagBits samples;
  CoglVkPipeStateKey state;
  GBytes *vertex_layout;
  uint32_t mask_bits;
  uint32_t strides_hash;
  uint32_t assemble_fp_hash;
  char assemble_fingerprint[41]; /* full SHA-1 returned by the assembler */
  uint32_t wrap_st; /* wrap_s | (wrap_t << 16) — do not share REPEAT/CLAMP PSOs */
  gboolean opaque;
  gboolean textured;
  gboolean assemble;
  gboolean use_cold;
  unsigned int ycbcr;
} CoglVkVboPipeKey;

typedef struct
{
  CoglVkVboPipeKey key;
  VkPipeline pipeline;
} CoglVkVboPipeEntry;

static void
vbo_pipe_entry_free (gpointer data)
{
  CoglVkVboPipeEntry *entry = data;
  g_bytes_unref (entry->key.vertex_layout);
  g_free (entry);
}

void
cogl_vk_vbo_device_clear (CoglVkDevice *dev)
{
  /* Teardown only: the caller has waited for all device work to finish. */
  if (dev->vbo_pipes)
    {
      GHashTableIter iter;
      gpointer value;

      g_hash_table_iter_init (&iter, dev->vbo_pipes);
      while (g_hash_table_iter_next (&iter, NULL, &value))
        {
          CoglVkVboPipeEntry *entry = value;

          vkDestroyPipeline (dev->device, entry->pipeline, NULL);
        }
      g_clear_pointer (&dev->vbo_pipes, g_hash_table_unref);
    }
  if (dev->vbo_dummy_buf)
    vkDestroyBuffer (dev->device, dev->vbo_dummy_buf, NULL);
  if (dev->vbo_dummy_mem)
    vkFreeMemory (dev->device, dev->vbo_dummy_mem, NULL);
  if (dev->vbo_float_shadow_buf)
    vkDestroyBuffer (dev->device, dev->vbo_float_shadow_buf, NULL);
  if (dev->vbo_float_shadow_mem)
    vkFreeMemory (dev->device, dev->vbo_float_shadow_mem, NULL);
  if (dev->vbo_float_shadow_retired_buf)
    {
      for (unsigned int i = 0; i < dev->vbo_float_shadow_retired_buf->len; i++)
        {
          vkDestroyBuffer (dev->device, (VkBuffer) (uintptr_t)
                           g_ptr_array_index (dev->vbo_float_shadow_retired_buf, i), NULL);
          vkFreeMemory (dev->device, (VkDeviceMemory) (uintptr_t)
                        g_ptr_array_index (dev->vbo_float_shadow_retired_mem, i), NULL);
        }
    }
  g_clear_pointer (&dev->vbo_float_shadow_retired_buf, g_ptr_array_unref);
  g_clear_pointer (&dev->vbo_float_shadow_retired_mem, g_ptr_array_unref);
  dev->vbo_dummy_buf = VK_NULL_HANDLE;
  dev->vbo_dummy_mem = VK_NULL_HANDLE;
  dev->vbo_float_shadow_buf = VK_NULL_HANDLE;
  dev->vbo_float_shadow_mem = VK_NULL_HANDLE;
  dev->vbo_float_shadow_cap = 0;
  dev->vbo_float_shadow_used = 0;
}

static unsigned int
pipe_key_hash (gconstpointer p)
{
  const CoglVkVboPipeKey *k = p;
  return cogl_vk_pipe_state_key_hash (&k->state) ^ (unsigned int) k->format ^
         k->mask_bits ^ k->strides_hash ^ k->assemble_fp_hash ^ k->wrap_st ^
         (k->opaque ? 1u : 0) ^ (k->textured ? 2u : 0) ^
         (k->ycbcr << 16) ^ (k->assemble ? 4u : 0) ^ (k->use_cold ? 8u : 0) ^
         ((unsigned int) k->samples << 16);
}

static gboolean
pipe_key_equal (gconstpointer a,
                gconstpointer b)
{
  const CoglVkVboPipeKey *ka = a;
  const CoglVkVboPipeKey *kb = b;
  CoglVkVboPipeKey va = *ka, vb = *kb;

  va.vertex_layout = vb.vertex_layout = NULL;
  return memcmp (&va, &vb, sizeof (va)) == 0 &&
         g_bytes_equal (ka->vertex_layout, kb->vertex_layout);
}

static VkPrimitiveTopology
topology_from_mode (CoglVerticesMode mode)
{
  switch (mode)
    {
    case COGL_VERTICES_MODE_POINTS:
      return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case COGL_VERTICES_MODE_LINES:
      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case COGL_VERTICES_MODE_LINE_LOOP:
    case COGL_VERTICES_MODE_LINE_STRIP:
      return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case COGL_VERTICES_MODE_TRIANGLES:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case COGL_VERTICES_MODE_TRIANGLE_STRIP:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case COGL_VERTICES_MODE_TRIANGLE_FAN:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    default:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

static int
texcoord_slot (const CoglVkVboMask *mask,
               int                  layer)
{
  for (int i = 0; i < mask->n_texcoords; i++)
    if (mask->texcoord_layers[i] == layer)
      return i;
  return -1;
}

static int
texcoord_location (int slot)
{
  return slot == 0 ? COGL_VK_VBO_LOC_TEXCOORD0 :
         slot > 0 ? COGL_VK_VBO_LOC_TEXCOORD_BASE + slot - 1 : -1;
}

static VkFormat
vk_format_for_attr (CoglAttributeType type,
                    int               n,
                    gboolean          normalized)
{
  if (n < 1)
    n = 1;
  if (n > 4)
    n = 4;

  switch (type)
    {
    case COGL_ATTRIBUTE_TYPE_FLOAT:
      if (n == 1) return VK_FORMAT_R32_SFLOAT;
      if (n == 2) return VK_FORMAT_R32G32_SFLOAT;
      if (n == 3) return VK_FORMAT_R32G32B32_SFLOAT;
      return VK_FORMAT_R32G32B32A32_SFLOAT;

    case COGL_ATTRIBUTE_TYPE_BYTE:
    case COGL_ATTRIBUTE_TYPE_UNSIGNED_BYTE:
    case COGL_ATTRIBUTE_TYPE_SHORT:
    case COGL_ATTRIBUTE_TYPE_UNSIGNED_SHORT:
      /* Integer attrs are CPU-expanded to float (see
       * attr_needs_float_expand).
       * Normalized still expands to 0..1 floats — do not use UNORM VI on the
       * original buffer (ANV fetch was 0).
       */
      (void) normalized;
      if (n == 1) return VK_FORMAT_R32_SFLOAT;
      if (n == 2) return VK_FORMAT_R32G32_SFLOAT;
      if (n == 3) return VK_FORMAT_R32G32B32_SFLOAT;
      return VK_FORMAT_R32G32B32A32_SFLOAT;

    default:
      if (n == 1) return VK_FORMAT_R32_SFLOAT;
      if (n == 2) return VK_FORMAT_R32G32_SFLOAT;
      return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

static size_t
attr_type_size (CoglAttributeType type)
{
  switch (type)
    {
    case COGL_ATTRIBUTE_TYPE_BYTE:
    case COGL_ATTRIBUTE_TYPE_UNSIGNED_BYTE:
      return 1;
    case COGL_ATTRIBUTE_TYPE_SHORT:
    case COGL_ATTRIBUTE_TYPE_UNSIGNED_SHORT:
      return 2;
    case COGL_ATTRIBUTE_TYPE_FLOAT:
    default:
      return 4;
    }
}

static gboolean
attr_needs_float_expand (CoglAttribute *a)
{
  if (!a || !a->name_state)
    return FALSE;
  switch (a->type)
    {
    case COGL_ATTRIBUTE_TYPE_BYTE:
    case COGL_ATTRIBUTE_TYPE_UNSIGNED_BYTE:
    case COGL_ATTRIBUTE_TYPE_SHORT:
    case COGL_ATTRIBUTE_TYPE_UNSIGNED_SHORT:
      return TRUE;
    case COGL_ATTRIBUTE_TYPE_FLOAT:
      /* 3-float positions must be shadow-padded to vec4. R32G32 is the
       * required format but the spec fills z=0, and a perspective MVP
       * then produces clip.w = -z = 0, so the primitive is clipped away
       * (onscreen stage stays the clear color). RGBA32 keeps z.
       * CUSTOM FLOAT at loc≥5 fetched as 0 until shadow-copied.
       */
      if (a->n_components == 3 &&
          a->name_state->name_id == COGL_ATTRIBUTE_NAME_ID_POSITION_ARRAY)
        return TRUE;
      return a->name_state->name_id == COGL_ATTRIBUTE_NAME_ID_CUSTOM_ARRAY;
    default:
      return FALSE;
    }
}

static uint32_t
vi_stride_for_attr_mask (CoglAttributeType type,
                         int               n_components,
                         gboolean          normalized,
                         uint32_t          recorded_stride,
                         gboolean          is_custom_float)
{
  (void) normalized;
  if (type == COGL_ATTRIBUTE_TYPE_BYTE ||
      type == COGL_ATTRIBUTE_TYPE_UNSIGNED_BYTE ||
      type == COGL_ATTRIBUTE_TYPE_SHORT ||
      type == COGL_ATTRIBUTE_TYPE_UNSIGNED_SHORT ||
      is_custom_float)
    return (uint32_t) (n_components * sizeof (float));
  if (recorded_stride)
    return recorded_stride;
  return (uint32_t) (n_components * attr_type_size (type));
}

static float
read_attr_component_as_float (CoglAttribute *a,
                              const uint8_t *p)
{
  switch (a->type)
    {
    case COGL_ATTRIBUTE_TYPE_FLOAT:
      return *(const float *) p;
    case COGL_ATTRIBUTE_TYPE_BYTE:
      if (a->normalized)
        return (float) (*(const int8_t *) p) / 127.f;
      return (float) (*(const int8_t *) p);
    case COGL_ATTRIBUTE_TYPE_UNSIGNED_BYTE:
      if (a->normalized)
        return (float) (*p) / 255.f;
      return (float) (*p);
    case COGL_ATTRIBUTE_TYPE_SHORT:
      if (a->normalized)
        return (float) (*(const int16_t *) p) / 32767.f;
      return (float) (*(const int16_t *) p);
    case COGL_ATTRIBUTE_TYPE_UNSIGNED_SHORT:
      if (a->normalized)
        return (float) (*(const uint16_t *) p) / 65535.f;
      return (float) (*(const uint16_t *) p);
    default:
      return 0.f;
    }
}

static gboolean
ensure_float_shadow_capacity (CoglVkDevice *dev,
                              size_t        need,
                              GError      **error)
{
  VkBufferCreateInfo bi = {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
  };
  VkMemoryRequirements reqs;
  VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
  VkPhysicalDeviceMemoryProperties mp;
  uint32_t i;
  VkResult res;

  if (need <= dev->vbo_float_shadow_cap && dev->vbo_float_shadow_buf != VK_NULL_HANDLE)
    return TRUE;

  /* Do not flush_current_pass here. draw_common still holds that pass;
   * freeing it mid-resolve is a use-after-free (uniforms exit 245).
   * Keep the old buffer until nothing is recording, then WaitIdle.
   */
  if (dev->owner && !dev->owner->current_pass &&
      dev->vbo_float_shadow_retired_buf &&
      dev->vbo_float_shadow_retired_buf->len > 0)
    {
      unsigned int ri;

      vkDeviceWaitIdle (dev->device);
      for (ri = 0; ri < dev->vbo_float_shadow_retired_buf->len; ri++)
        {
          vkDestroyBuffer (dev->device,
                           (VkBuffer) (uintptr_t)
                           g_ptr_array_index (dev->vbo_float_shadow_retired_buf, ri),
                           NULL);
          vkFreeMemory (dev->device,
                        (VkDeviceMemory) (uintptr_t)
                        g_ptr_array_index (dev->vbo_float_shadow_retired_mem, ri),
                        NULL);
        }
      g_ptr_array_set_size (dev->vbo_float_shadow_retired_buf, 0);
      g_ptr_array_set_size (dev->vbo_float_shadow_retired_mem, 0);
    }

  if (dev->vbo_float_shadow_buf != VK_NULL_HANDLE)
    {
      if (!dev->vbo_float_shadow_retired_buf)
        {
          dev->vbo_float_shadow_retired_buf = g_ptr_array_new ();
          dev->vbo_float_shadow_retired_mem = g_ptr_array_new ();
        }
      g_ptr_array_add (dev->vbo_float_shadow_retired_buf,
                       (gpointer) (uintptr_t) dev->vbo_float_shadow_buf);
      g_ptr_array_add (dev->vbo_float_shadow_retired_mem,
                       (gpointer) (uintptr_t) dev->vbo_float_shadow_mem);
      dev->vbo_float_shadow_buf = VK_NULL_HANDLE;
      dev->vbo_float_shadow_mem = VK_NULL_HANDLE;
      dev->vbo_float_shadow_cap = 0;
      dev->vbo_float_shadow_used = 0;
    }

  /* Journal snapshots are one buffer per draw. 4K forces a grow (and
   * used to free the live pass) after a handful of quads.
   */
  bi.size = MAX (need, (size_t) (1024 * 1024));
  res = vkCreateBuffer (dev->device, &bi, NULL, &dev->vbo_float_shadow_buf);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "float shadow VkBuffer failed (%d)", (int) res);
      return FALSE;
    }
  vkGetBufferMemoryRequirements (dev->device, dev->vbo_float_shadow_buf, &reqs);
  vkGetPhysicalDeviceMemoryProperties (dev->physical_device, &mp);
  ai.allocationSize = reqs.size;
  for (i = 0; i < mp.memoryTypeCount; i++)
    {
      if ((reqs.memoryTypeBits & (1u << i)) &&
          (mp.memoryTypes[i].propertyFlags &
           (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
          (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        {
          ai.memoryTypeIndex = i;
          break;
        }
    }
  res = vkAllocateMemory (dev->device, &ai, NULL, &dev->vbo_float_shadow_mem);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "float shadow memory failed (%d)", (int) res);
      vkDestroyBuffer (dev->device, dev->vbo_float_shadow_buf, NULL);
      dev->vbo_float_shadow_buf = VK_NULL_HANDLE;
      return FALSE;
    }
  vkBindBufferMemory (dev->device, dev->vbo_float_shadow_buf,
                      dev->vbo_float_shadow_mem, 0);
  dev->vbo_float_shadow_cap = (size_t) bi.size;
  return TRUE;
}

static size_t
attr_vertex_count (CoglAttribute *a);

static gboolean
expand_attr_to_float_shadow (CoglAttribute *a,
                             CoglVkDevice  *dev,
                             VkBuffer      *out_buf,
                             VkDeviceSize  *out_off,
                             GError       **error)
{
  COGL_VK_PROFILE_SCOPE ("expand_attr_to_float_shadow");
  CoglBuffer *buf;
  const uint8_t *data;
  float *dst;
  void *mapped;
  size_t n_verts;
  size_t nbytes;
  size_t align_off;
  int c;
  size_t v;
  int comp_size;
  int out_comps;
  float w_pad = 0.f;

  buf = COGL_BUFFER (cogl_attribute_get_buffer (a));
  if (!buf || a->stride <= 0 || a->n_components < 1)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "float expand: bad attribute");
      return FALSE;
    }

  n_verts = attr_vertex_count (a);
  if (n_verts < 1)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "float expand: attribute exceeds buffer");
      return FALSE;
    }
  /* 3-comp pad to vec4 so the VI format is the required RGBA32, not RGB32. */
  out_comps = a->n_components == 3 ? 4 : a->n_components;
  if (a->n_components == 3 && a->name_state &&
      (a->name_state->name_id == COGL_ATTRIBUTE_NAME_ID_POSITION_ARRAY ||
       a->name_state->name_id == COGL_ATTRIBUTE_NAME_ID_TEXTURE_COORD_ARRAY))
    w_pad = 1.f;
  nbytes = n_verts * (size_t) out_comps * sizeof (float);
  align_off = (dev->vbo_float_shadow_used + 15u) & ~((size_t) 15u);

  if (!ensure_float_shadow_capacity (dev, align_off + nbytes, error))
    return FALSE;

  data = _cogl_buffer_map (buf, COGL_BUFFER_ACCESS_READ, 0, error);
  if (!data)
    return FALSE;

  if (vkMapMemory (dev->device, dev->vbo_float_shadow_mem, 0,
                   dev->vbo_float_shadow_cap, 0, &mapped) != VK_SUCCESS)
    {
      cogl_buffer_unmap (buf);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "float shadow map failed");
      return FALSE;
    }

  dst = (float *) ((uint8_t *) mapped + align_off);
  comp_size = (int) attr_type_size (a->type);
  for (v = 0; v < n_verts; v++)
    {
      const uint8_t *row = data + a->offset + v * a->stride;
      float *out = dst + v * (size_t) out_comps;

      for (c = 0; c < a->n_components; c++)
        out[c] = read_attr_component_as_float (a, row + c * comp_size);
      for (; c < out_comps; c++)
        out[c] = (c == 3) ? w_pad : 0.f;
    }

  {
    float v0x = n_verts ? dst[0] : 0.f;
    float v0y = (n_verts && a->n_components > 1) ? dst[1] : 0.f;

    vkUnmapMemory (dev->device, dev->vbo_float_shadow_mem);
    cogl_buffer_unmap (buf);

    *out_buf = dev->vbo_float_shadow_buf;
    *out_off = (VkDeviceSize) align_off;
    dev->vbo_float_shadow_used = align_off + nbytes;

    cogl_vk_debug_log (
      "vbo",
      "float expand name=%s type=%d comps=%d n_verts=%zu off=%zu "
      "v0=(%.1f,%.1f)",
      a->name_state && a->name_state->name ? a->name_state->name : "?",
      (int) a->type, a->n_components, n_verts, align_off, v0x, v0y);
  }

  /* Each draw appends to a fresh HOST_COHERENT range; it never overwrites
   * vertices referenced by an earlier draw. Queue submission performs the
   * host-to-device domain operation for these writes before vertex fetch.
   * Waiting for the entire device here adds no dependency for this range.
   */
  return TRUE;
}

static gboolean
collect_mask (CoglAttribute **attributes,
              int             n_attributes,
              CoglVkVboMask  *out)
{
  int i;

  memset (out, 0, sizeof (*out));
  if (!attributes || n_attributes < 1)
    return FALSE;

  out->n_texcoords = 1;
  out->texcoord_layers = g_new0 (int, n_attributes + 1);
  out->texcoord_components = g_new0 (int, n_attributes + 1);
  out->texcoord_type = g_new0 (CoglAttributeType, n_attributes + 1);
  out->custom_components = g_new0 (int, n_attributes);
  out->custom_type = g_new0 (CoglAttributeType, n_attributes);
  out->custom_normalized = g_new0 (gboolean, n_attributes);
  out->custom_name_index = g_new0 (int, n_attributes);
  out->strides = g_new0 (uint32_t, n_attributes + 5);
  /* Logical indices can be sparse (e.g. 1000); only present attributes
   * consume locations. Discover all texcoords before assigning customs.
   */
  for (i = 0; i < n_attributes; i++)
    {
      CoglAttribute *a = attributes[i];
      if (a && a->name_state &&
          a->name_state->name_id == COGL_ATTRIBUTE_NAME_ID_TEXTURE_COORD_ARRAY &&
          a->name_state->layer_number >= 0 &&
          texcoord_slot (out, a->name_state->layer_number) < 0)
        out->texcoord_layers[out->n_texcoords++] = a->name_state->layer_number;
    }
  out->custom_base = COGL_VK_VBO_LOC_TEXCOORD_BASE + out->n_texcoords - 1;
  out->position_components = 2;
  out->color_components = 4;
  out->normal_components = 3;
  out->point_size_components = 1;
  out->position_type = COGL_ATTRIBUTE_TYPE_FLOAT;
  out->color_type = COGL_ATTRIBUTE_TYPE_FLOAT;
  out->normal_type = COGL_ATTRIBUTE_TYPE_FLOAT;
  for (i = 0; i < out->n_texcoords; i++)
    {
      out->texcoord_components[i] = 2;
      out->texcoord_type[i] = COGL_ATTRIBUTE_TYPE_FLOAT;
    }

  if (!attributes)
    return FALSE;

  for (i = 0; i < n_attributes; i++)
    {
      CoglAttribute *a = attributes[i];
      int layer;
      int loc;
      int c;

      if (!a || !a->name_state)
        continue;
      switch (a->name_state->name_id)
        {
        case COGL_ATTRIBUTE_NAME_ID_POSITION_ARRAY:
          out->has_position = TRUE;
          out->position_components = a->n_components;
          out->position_type = a->type;
          out->strides[COGL_VK_VBO_LOC_POSITION] = (uint32_t) a->stride;
          break;
        case COGL_ATTRIBUTE_NAME_ID_COLOR_ARRAY:
          out->has_color = TRUE;
          out->color_components = a->n_components;
          out->color_type = a->type;
          out->normalized_color = a->normalized ||
                                  (a->type == COGL_ATTRIBUTE_TYPE_UNSIGNED_BYTE);
          out->strides[COGL_VK_VBO_LOC_COLOR] = (uint32_t) a->stride;
          break;
        case COGL_ATTRIBUTE_NAME_ID_TEXTURE_COORD_ARRAY:
          layer = texcoord_slot (out, a->name_state->layer_number);
          if (layer < 0)
            break;
          loc = texcoord_location (layer);
          if (loc < 0)
            break;
          if (layer == 0)
            out->has_texcoord0 = TRUE;
          out->texcoord_components[layer] = a->n_components;
          out->texcoord_type[layer] = a->type;
          out->strides[loc] = (uint32_t) a->stride;
          break;
        case COGL_ATTRIBUTE_NAME_ID_POINT_SIZE_ARRAY:
          out->has_point_size = TRUE;
          out->point_size_components = a->n_components > 0 ? a->n_components : 1;
          out->strides[COGL_VK_VBO_LOC_POINT_SIZE] = (uint32_t) a->stride;
          break;
        case COGL_ATTRIBUTE_NAME_ID_NORMAL_ARRAY:
          out->has_normal = TRUE;
          out->normal_components = a->n_components > 0 ? a->n_components : 3;
          out->normal_type = a->type;
          out->normalized_normal = a->normalized ||
                                   a->name_state->normalized_default;
          out->strides[COGL_VK_VBO_LOC_NORMAL] = (uint32_t) a->stride;
          break;
        case COGL_ATTRIBUTE_NAME_ID_CUSTOM_ARRAY:
          c = out->n_custom++;
          out->custom_components[c] = a->n_components;
          out->custom_type[c] = a->type;
          out->custom_normalized[c] = a->normalized;
          out->custom_name_index[c] = a->name_state->name_index;
          out->strides[out->custom_base + c] = (uint32_t) a->stride;
          break;
        default:
          break;
        }
    }

  if (cogl_vk_debug_enabled ("vbo"))
    {
      cogl_vk_debug_log (
        "vbo",
        "collect_mask pos=%d color=%d texcoords=%u pt=%d nrm=%d n_custom=%d "
        "strides[pos,col,tex0]=[%u %u %u]",
        out->has_position, out->has_color, (unsigned) out->n_texcoords,
        out->has_point_size, out->has_normal, out->n_custom,
        out->strides[COGL_VK_VBO_LOC_POSITION],
        out->strides[COGL_VK_VBO_LOC_COLOR],
        out->strides[COGL_VK_VBO_LOC_TEXCOORD0]);
      for (i = 0; i < n_attributes; i++)
        {
          CoglAttribute *a = attributes[i];
          if (!a || !a->name_state)
            continue;
          cogl_vk_debug_log (
            "vbo",
            "  collect attr[%d] name=%s id=%d layer=%d comps=%d "
            "stride=%zu off=%zu",
            i,
            a->name_state->name ? a->name_state->name : "?",
            (int) a->name_state->name_id,
            a->name_state->layer_number,
            a->n_components,
            (size_t) a->stride,
            (size_t) a->offset);
        }
    }

  out->n_bindings = out->custom_base + out->n_custom;
  return out->has_position;
}

gboolean
cogl_vk_vbo_should_use_push_constant_fast_path (CoglVerticesMode  mode,
                                                int               n_vertices,
                                                CoglAttribute   **attributes,
                                                int               n_attributes,
                                                CoglPipeline     *pipeline)
{
  gboolean has_position = FALSE;

  /* Journal uses TRIANGLE_FAN quads. TRIANGLE_STRIP (e.g. map-buffer-range
   * NDC strip) must not take the AABB push path — vert0/vert2 collapse X.
   */
  if (mode != COGL_VERTICES_MODE_TRIANGLE_FAN)
    return FALSE;
  if (n_vertices != 4)
    return FALSE;
  for (int i = 0; i < n_attributes; i++)
    {
      CoglAttribute *attr = attributes[i];
      if (!attr || !attr->name_state)
        continue;
      if (attr->name_state->name_id == COGL_ATTRIBUTE_NAME_ID_POSITION_ARRAY)
        has_position = TRUE;
      else if (attr->name_state->name_id != COGL_ATTRIBUTE_NAME_ID_TEXTURE_COORD_ARRAY ||
               attr->name_state->layer_number != 0)
        return FALSE;
    }
  if (!has_position)
    return FALSE;
  if (!pipeline)
    return TRUE;
  if (cogl_pipeline_get_alpha_test_function (pipeline) !=
      COGL_PIPELINE_ALPHA_FUNC_ALWAYS)
    return FALSE;
  if (cogl_pipeline_get_point_size (pipeline) > 0.f ||
      cogl_pipeline_get_per_vertex_point_size (pipeline))
    return FALSE;
  /* get_layer_point_sprite_coords_enabled creates missing layers.
   * n_layers is a count, not a dense index range (layer-remove leaves
   * index 1 with n_layers==1; pipeline-shader-state sets layer 1 only).
   * Walk the real layers instead of 0..n-1.
   */
  {
    g_autofree CoglPipelineLayer **layers = NULL;
    int n_real = 0;
    int li;

    cogl_vk_pipeline_collect_layers (pipeline, &layers, &n_real);
    for (li = 0; li < n_real; li++)
      {
        if (!layers[li])
          continue;
        if (cogl_pipeline_get_layer_point_sprite_coords_enabled (
              pipeline, layers[li]->index))
          return FALSE;
      }
    if (n_real != cogl_pipeline_get_n_layers (pipeline))
      cogl_vk_debug_log ("draw",
                         "push-path layer count mismatch collect=%d n=%d",
                         n_real, cogl_pipeline_get_n_layers (pipeline));
  }
  if (_cogl_pipeline_has_vertex_snippets (pipeline))
    return FALSE;
  /* Push path expands a unit quad with Local×MVP. Only safe when the
   * current modelview is identity (journal CPU-transforms then loads
   * identity). CoglPrimitive + translate must use true VBO attributes.
   */
  if (pipeline->context)
    {
      CoglMatrixEntry *mv =
        cogl_context_get_current_modelview_entry (pipeline->context);
      if (mv != cogl_context_get_identity_entry (pipeline->context))
        return FALSE;
    }
  /* Fragment snippets / non-hot state need assemble → VBO cold path, not
   * solid push-constant cmd_draw_quad (which only uses hot white_pipe).
   */
  if (!cogl_vk_pipeline_is_hot_path (pipeline))
    return FALSE;
  return TRUE;
}

static gboolean
ensure_buf (CoglBuffer *buffer,
            GError    **error)
{
  if (!buffer || !buffer->impl || !COGL_IS_BUFFER_IMPL_VK (buffer->impl))
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR,
                           COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "Not a Vulkan buffer");
      return FALSE;
    }
  return cogl_buffer_impl_vk_ensure (COGL_BUFFER_IMPL_VK (buffer->impl),
                                     buffer, error);
}

static VkBuffer
peek_buf (CoglBuffer *buffer)
{
  return cogl_buffer_impl_vk_peek_buffer (COGL_BUFFER_IMPL_VK (buffer->impl));
}

static gboolean
ensure_dummy (CoglVkDevice *dev,
              GError      **error)
{
  VkBufferCreateInfo bi = {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size = 64,
    .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
  };
  VkMemoryRequirements reqs;
  VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
  VkPhysicalDeviceMemoryProperties mp;
  uint32_t i;
  void *mapped = NULL;
  /* color(16) + tex0(8) + point(4) + normal(12) defaults */
  float defaults[16] = {
    1.f, 1.f, 1.f, 1.f,
    0.f, 0.f,
    1.f,
    0.f, 0.f, 1.f,
    0.f, 0.f, 0.f, 0.f, 0.f, 0.f,
  };
  VkResult res;

  if (dev->vbo_dummy_buf != VK_NULL_HANDLE)
    return TRUE;

  res = vkCreateBuffer (dev->device, &bi, NULL, &dev->vbo_dummy_buf);
  if (res != VK_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "dummy VkBuffer failed (%d)", (int) res);
      return FALSE;
    }
  vkGetBufferMemoryRequirements (dev->device, dev->vbo_dummy_buf, &reqs);
  vkGetPhysicalDeviceMemoryProperties (dev->physical_device, &mp);
  ai.allocationSize = reqs.size;
  for (i = 0; i < mp.memoryTypeCount; i++)
    {
      if ((reqs.memoryTypeBits & (1u << i)) &&
          (mp.memoryTypes[i].propertyFlags &
           (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
          (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        {
          ai.memoryTypeIndex = i;
          break;
        }
    }
  res = vkAllocateMemory (dev->device, &ai, NULL, &dev->vbo_dummy_mem);
  if (res != VK_SUCCESS)
    return FALSE;
  vkBindBufferMemory (dev->device, dev->vbo_dummy_buf, dev->vbo_dummy_mem, 0);
  if (vkMapMemory (dev->device, dev->vbo_dummy_mem, 0, sizeof (defaults), 0,
                   &mapped) == VK_SUCCESS)
    {
      memcpy (mapped, defaults, sizeof (defaults));
      vkUnmapMemory (dev->device, dev->vbo_dummy_mem);
    }
  return TRUE;
}

static void
fill_mvp (CoglFramebuffer *fb,
          CoglPipeline    *pipeline,
          CoglVkVertPush  *out)
{
  /* Bakes y_flip into projection when no vertex snippets (GL
   * flip_uniform==-1).
   */
  cogl_vk_fill_vert_push_mvp (fb, pipeline, out);
}

static const char VBO_VERT[] =
  "#version 450\n"
  "layout(push_constant, row_major) uniform VertPC {\n"
  "  mat4 mvp;\n"
  "  vec2 point_size_pad;\n"
  "  vec2 uv_size;\n"
  "} data;\n"
  "layout(location = 0) in vec4 cogl_position_in;\n"
  "layout(location = 1) in vec4 cogl_color_in;\n"
  "layout(location = 2) in vec2 cogl_tex_coord0_in;\n"
  "layout(location = 3) in float cogl_point_size_in;\n"
  "layout(location = 4) in vec3 cogl_normal_in;\n"
  "layout(location = 0) out vec2 uv;\n"
  "layout(location = 1) out vec4 v_color;\n"
  "void main() {\n"
  "  vec4 pos = cogl_position_in;\n"
  "  if (pos.w == 0.0) pos.w = 1.0;\n"
  "  uv = cogl_tex_coord0_in;\n"
  "  v_color = cogl_color_in;\n"
  "  /* Cogl/GL clip z is [-1,1]; Vulkan is [0,1]. */\n"
  "  vec4 clip = data.mvp * pos;\n"
  "  clip.z = clip.z * 0.5 + clip.w * 0.5;\n"
  "  gl_Position = clip;\n"
  "  float ps = data.point_size_pad.x;\n"
  "  if (cogl_point_size_in > 0.0) ps = cogl_point_size_in;\n"
  "  gl_PointSize = max(ps, 1.0);\n"
  "}\n";

static const char VBO_FRAG_TEX[] =
  "#version 450\n"
  "layout(set = 0, binding = 0) uniform sampler2D tex;\n"
  "layout(location = 0) in vec2 uv;\n"
  "layout(location = 1) in vec4 v_color;\n"
  "layout(location = 0) out vec4 out_color;\n"
  "layout(push_constant) uniform FragPC {\n"
  "  layout(offset = 80) vec4 color_mul;\n"
  "  layout(offset = 96) float wrap_s;\n"
  "  layout(offset = 100) float wrap_t;\n"
  "  layout(offset = 104) float depth_kill;\n"
  "  layout(offset = 108) float out_order;\n"
  "} data;\n"
  "float wrap_coord(float t, float mode) {\n"
  "  if (mode < 0.5) return clamp(t, 0.0, 0.999999);\n"
  "  if (mode < 1.5) return t - floor(t);\n"
  "  float n = floor(t);\n"
  "  float f = t - n;\n"
  "  if (mod(n, 2.0) >= 1.0) return 1.0 - f;\n"
  "  return f;\n"
  "}\n"
  "vec4 swizzle_out(vec4 c, float order) {\n"
  "  /* R16G16B16A16 has no BGRA format. Match clear's memory order:\n"
  "   * 1 = BGR (B,G,R,A), 2 = AFIRST (A,R,G,B), 3 = both (A,B,G,R). */\n"
  "  if (order > 3.5) return vec4(c.g, c.r, c.a, c.b);\n"
  "  if (order > 2.5) return vec4(c.a, c.b, c.g, c.r);\n"
  "  if (order > 1.5) return vec4(c.a, c.r, c.g, c.b);\n"
  "  if (order > 0.5) return c.bgra;\n"
  "  return c;\n"
  "}\n"
  "void main() {\n"
  "  if (data.depth_kill > 0.5) discard;\n"
  "  vec2 u = vec2(wrap_coord(uv.x, data.wrap_s),\n"
  "                wrap_coord(uv.y, data.wrap_t));\n"
  "  out_color = swizzle_out(textureLod(tex, u, 0.0) * v_color * data.color_mul,\n"
  "                          data.out_order);\n"
  "}\n";

static const char VBO_FRAG_SOLID[] =
  "#version 450\n"
  "layout(location = 0) in vec2 uv;\n"
  "layout(location = 1) in vec4 v_color;\n"
  "layout(location = 0) out vec4 out_color;\n"
  "layout(push_constant) uniform FragPC {\n"
  "  layout(offset = 80) vec4 color_mul;\n"
  "  layout(offset = 108) float out_order;\n"
  "} data;\n"
  "vec4 swizzle_out(vec4 c, float order) {\n"
  "  if (order > 3.5) return vec4(c.g, c.r, c.a, c.b);\n"
  "  if (order > 2.5) return vec4(c.a, c.b, c.g, c.r);\n"
  "  if (order > 1.5) return vec4(c.a, c.r, c.g, c.b);\n"
  "  if (order > 0.5) return c.bgra;\n"
  "  return c;\n"
  "}\n"
  "void main() {\n"
  "  out_color = swizzle_out(v_color * data.color_mul, data.out_order);\n"
  "}\n";

static GBytes *
vertex_layout_key (const CoglVkVboMask *m)
{
  GByteArray *bytes = g_byte_array_new ();
  const int fixed[] = {
    m->has_position, m->has_color, m->has_point_size, m->has_normal,
    m->normalized_color, m->normalized_normal, m->has_texcoord0,
    m->position_components, m->color_components, m->normal_components,
    m->point_size_components, m->position_type, m->color_type, m->normal_type,
    m->n_texcoords, m->n_custom, m->custom_base, m->n_bindings
  };

  g_byte_array_append (bytes, (const uint8_t *) fixed, sizeof (fixed));
#define APPEND_FIELD(field, count) \
  g_byte_array_append (bytes, (const uint8_t *) m->field, \
                       sizeof (*m->field) * (count))
  APPEND_FIELD (texcoord_layers, m->n_texcoords);
  APPEND_FIELD (texcoord_components, m->n_texcoords);
  APPEND_FIELD (texcoord_type, m->n_texcoords);
  APPEND_FIELD (custom_components, m->n_custom);
  APPEND_FIELD (custom_type, m->n_custom);
  APPEND_FIELD (custom_normalized, m->n_custom);
  APPEND_FIELD (custom_name_index, m->n_custom);
  APPEND_FIELD (strides, m->n_bindings);
#undef APPEND_FIELD
  return g_byte_array_free_to_bytes (bytes);
}

static void
fill_binding (VkVertexInputBindingDescription   *b,
              VkVertexInputAttributeDescription *a,
              uint32_t                           loc,
              uint32_t                           stride,
              VkFormat                           format)
{
  b->binding = loc;
  b->stride = stride;
  b->inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  a->location = loc;
  a->binding = loc;
  a->format = format;
  a->offset = 0;
}

static void
build_vi (const CoglVkVboMask                 *mask,
          VkVertexInputBindingDescription     *bindings,
          VkVertexInputAttributeDescription   *attrs,
          uint32_t                            *n_bind,
          uint32_t                            *n_attr)
{
  uint32_t n = 0;
  int layer;
  int c;
  uint32_t stride;
  int pos_n;
  uint32_t pos_stride_rec;

  /* Fixed locations 0..4 always present so hot/assemble shaders stay valid.
   * Missing attrs bind the shared dummy buffer: stride MUST be 0 so every
   * vertex reads the same constant (white/uv0/…). Non-zero stride walked off
   * the 64-byte dummy and produced garbage colors (primitive #80bfbf,
   * map-buffer right side cleared by a≈0 blend).
   * Real 3-component attrs are shadow-padded to vec4: R32G32B32 is not a
   * required vertex format and ANV fetches it as 0. Do not apply that pad
   * to stride-0 dummies — their bind offset (normal @ 28) is not 16-aligned.
   */
  pos_n = mask->position_components;
  pos_stride_rec = mask->strides[0];
  if (pos_n == 3 && mask->position_type == COGL_ATTRIBUTE_TYPE_FLOAT)
    {
      /* resolve expands these to tightly packed vec4 (x,y,z,1). */
      stride = (uint32_t) (4 * sizeof (float));
      fill_binding (&bindings[n], &attrs[n], COGL_VK_VBO_LOC_POSITION, stride,
                    VK_FORMAT_R32G32B32A32_SFLOAT);
    }
  else
    {
      stride = vi_stride_for_attr_mask (mask->position_type, pos_n,
                                        FALSE, pos_stride_rec, FALSE);
      fill_binding (&bindings[n], &attrs[n], COGL_VK_VBO_LOC_POSITION, stride,
                    vk_format_for_attr (mask->position_type, pos_n, FALSE));
    }
  cogl_vk_debug_log ("vbo",
                     "  VI[0] loc=%u binding=%u format=%d stride=%u comps=%d",
                     COGL_VK_VBO_LOC_POSITION,
                     COGL_VK_VBO_LOC_POSITION,
                     (int) attrs[n].format,
                     stride, pos_n);
  n++;

  if (mask->has_color)
    {
      int col_n = mask->color_components;
      uint32_t col_stride = mask->strides[1];

      if (col_n == 3)
        {
          col_n = 4;
          col_stride = 16;
        }
      stride = vi_stride_for_attr_mask (mask->color_type, col_n,
                                        mask->normalized_color, col_stride,
                                        FALSE);
      fill_binding (&bindings[n], &attrs[n], COGL_VK_VBO_LOC_COLOR, stride,
                    vk_format_for_attr (mask->color_type, col_n,
                                        mask->normalized_color));
    }
  else
    {
      stride = 0;
      fill_binding (&bindings[n], &attrs[n], COGL_VK_VBO_LOC_COLOR, stride,
                    vk_format_for_attr (mask->color_type, mask->color_components,
                                        mask->normalized_color));
    }
  cogl_vk_debug_log ("vbo",
                     "  VI[1] loc=%u format=%d stride=%u comps=%d has=%d",
                     COGL_VK_VBO_LOC_COLOR,
                     (int) bindings[n].stride ? (int) attrs[n].format : 0,
                     stride, mask->color_components, mask->has_color);
  n++;

  if (mask->has_texcoord0)
    {
      int comps = mask->texcoord_components[0];
      if (comps == 3 && mask->texcoord_type[0] != COGL_ATTRIBUTE_TYPE_FLOAT)
        comps = 4;
      stride = vi_stride_for_attr_mask (mask->texcoord_type[0], comps, FALSE,
                                        mask->strides[2], FALSE);
    }
  else
    {
      stride = 0;
    }
  fill_binding (&bindings[n], &attrs[n], COGL_VK_VBO_LOC_TEXCOORD0, stride,
                vk_format_for_attr (mask->texcoord_type[0],
                                    mask->texcoord_components[0], FALSE));
  cogl_vk_debug_log ("vbo",
                     "  VI[2] loc=%u TEXCOORD0 format=%d stride=%u comps=%d has=%d",
                     COGL_VK_VBO_LOC_TEXCOORD0,
                     (int) vk_format_for_attr (mask->texcoord_type[0],
                                               mask->texcoord_components[0],
                                               FALSE),
                     stride, mask->texcoord_components[0],
                     (mask->has_texcoord0) != 0);
  n++;

  stride = mask->has_point_size
    ? (mask->strides[3] ? mask->strides[3] : 4)
    : 0;
  fill_binding (&bindings[n], &attrs[n], COGL_VK_VBO_LOC_POINT_SIZE, stride,
                VK_FORMAT_R32_SFLOAT);
  n++;

  if (mask->has_normal)
    {
      int nrm_n = mask->normal_components;
      uint32_t nrm_stride = mask->strides[4] ? mask->strides[4] :
                            (uint32_t) (mask->normal_components * attr_type_size (mask->normal_type));

      /* Same as position: 3-float normal cannot use R32G32B32 on ANV. */
      if (nrm_n == 3 && mask->normal_type == COGL_ATTRIBUTE_TYPE_FLOAT)
        {
          stride = nrm_stride ? nrm_stride : (uint32_t) (3 * sizeof (float));
          fill_binding (&bindings[n], &attrs[n], COGL_VK_VBO_LOC_NORMAL, stride,
                        VK_FORMAT_R32G32_SFLOAT);
        }
      else
        {
          stride = vi_stride_for_attr_mask (mask->normal_type, nrm_n,
                                            mask->normalized_normal, nrm_stride,
                                            FALSE);
          fill_binding (&bindings[n], &attrs[n], COGL_VK_VBO_LOC_NORMAL, stride,
                        vk_format_for_attr (mask->normal_type, nrm_n,
                                            mask->normalized_normal));
        }
    }
  else
    {
      stride = 0;
      fill_binding (&bindings[n], &attrs[n], COGL_VK_VBO_LOC_NORMAL, stride,
                    vk_format_for_attr (mask->normal_type, mask->normal_components,
                                        mask->normalized_normal));
    }
  n++;

  for (layer = 1; layer < mask->n_texcoords; layer++)
    {
      int loc = texcoord_location (layer);
      int comps = mask->texcoord_components[layer];
      if (comps == 3 && mask->texcoord_type[layer] != COGL_ATTRIBUTE_TYPE_FLOAT)
        comps = 4;
      stride = vi_stride_for_attr_mask (mask->texcoord_type[layer], comps, FALSE,
                                        mask->strides[loc], FALSE);
      fill_binding (&bindings[n], &attrs[n], (uint32_t) loc, stride,
                    vk_format_for_attr (mask->texcoord_type[layer],
                                        comps,
                                        FALSE));
      n++;
    }

  for (c = 0; c < mask->n_custom; c++)
    {
      int loc = mask->custom_base + c;
      VkFormat cfmt;
      int cc = mask->custom_components[c];
      uint32_t cs = mask->strides[loc];

      if (cc == 3)
        {
          cc = 4;
          cs = 16;
        }
      stride = vi_stride_for_attr_mask (mask->custom_type[c], cc,
                                        mask->custom_normalized[c],
                                        cs, TRUE);
      cfmt = vk_format_for_attr (mask->custom_type[c], cc,
                                 mask->custom_normalized[c]);
      fill_binding (&bindings[n], &attrs[n], (uint32_t) loc, stride, cfmt);
      cogl_vk_debug_log (
        "vbo",
        "  VI[CUSTOM%d] loc=%d binding=%d format=%d stride=%u comps=%d "
        "type=%u norm=%d",
        c, loc, loc, (int) cfmt, stride, mask->custom_components[c],
        (unsigned) mask->custom_type[c], mask->custom_normalized[c]);
      n++;
    }

  *n_bind = n;
  /* Location 5 is TEXCOORD layer 1 on the assemble path. Hot VBO_VERT
   * does not use it, so a 3-float position can put z there.
   */
  if (pos_n == 3 &&
      mask->position_type == COGL_ATTRIBUTE_TYPE_FLOAT &&
      !mask->has_normal &&
      mask->n_custom == 0 &&
      mask->n_texcoords == 1)
    {
      /* Same binding as position, +8 bytes. A batched journal draw has
       * a different z per quad; one push constant cannot carry that.
       * R32_SFLOAT is a required vertex format.
       */
      attrs[n].location = 5;
      attrs[n].binding = COGL_VK_VBO_LOC_POSITION;
      attrs[n].format = VK_FORMAT_R32_SFLOAT;
      attrs[n].offset = 2 * (uint32_t) sizeof (float);
      *n_attr = n + 1;
    }
  else
    *n_attr = n;
}

/* Fixed VBO_VERT only wires pos/color/texcoord0/point_size; it voids
 * normal and never declares CUSTOM or texcoord layers > 0. Force cold
 * VERT_VBO assemble whenever those attrs are present.
 */
static gboolean
vbo_mask_needs_assemble (const CoglVkVboMask *mask)
{
  if (!mask)
    return FALSE;
  if (mask->has_normal || mask->n_custom > 0)
    return TRUE;
  if (mask->n_texcoords > 1)
    return TRUE;
  return FALSE;
}

static gboolean
want_assemble (CoglPipeline         *pipeline,
               const CoglVkVboMask  *mask)
{
  if (vbo_mask_needs_assemble (mask))
    return TRUE;
  return pipeline &&
         (_cogl_pipeline_has_vertex_snippets (pipeline) ||
          !cogl_vk_pipeline_is_hot_path (pipeline));
}

/* Public wrap getters call _cogl_pipeline_get_layer(), which creates a
 * missing index. layer-remove and pipeline-shader-state keep a layer at
 * index 1 with n_layers==1. Creating index 0 from inside the draw flushes
 * the journal and re-enters draw_attributes. Read the first real layer.
 */
static CoglPipelineLayer *
peek_first_layer (CoglPipeline *pipeline)
{
  g_autofree CoglPipelineLayer **layers = NULL;
  int n_layers = 0;

  if (!pipeline || cogl_pipeline_get_n_layers (pipeline) < 1)
    return NULL;

  cogl_vk_pipeline_collect_layers (pipeline, &layers, &n_layers);
  if (n_layers < 1)
    return NULL;
  return layers[0];
}

static void
peek_first_layer_wrap (CoglPipeline             *pipeline,
                       CoglSamplerCacheWrapMode *wrap_s,
                       CoglSamplerCacheWrapMode *wrap_t)
{
  CoglPipelineLayer *layer = peek_first_layer (pipeline);

  *wrap_s = COGL_SAMPLER_CACHE_WRAP_MODE_CLAMP_TO_EDGE;
  *wrap_t = COGL_SAMPLER_CACHE_WRAP_MODE_CLAMP_TO_EDGE;
  if (layer)
    _cogl_pipeline_layer_get_wrap_modes (layer, wrap_s, wrap_t);
}

static gboolean
is_hot_tex_pipe (CoglVkRenderFormatSetup *setup,
                 VkPipeline               pipe)
{
  for (unsigned int i = 0; i < COGL_VK_YCBCR_VARIANTS; i++)
    if (pipe == setup->tex_pipe_ycbcr[i] ||
        pipe == setup->tex_pipe_ycbcr_opaque[i])
      return TRUE;
  return pipe == setup->tex_pipe || pipe == setup->tex_pipe_opaque;
}

static gboolean
validate_vertex_input_capacity (const VkPhysicalDeviceLimits *limits,
                                uint32_t                      n_bindings,
                                uint32_t                      n_attributes,
                                GError                      **error)
{
  if (n_bindings <= limits->maxVertexInputBindings &&
      n_attributes <= limits->maxVertexInputAttributes)
    return TRUE;

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "Vulkan vertex input needs %u bindings / %u attributes; "
               "device supports %u / %u", n_bindings, n_attributes,
               limits->maxVertexInputBindings, limits->maxVertexInputAttributes);
  return FALSE;
}

static VkPipeline
get_vbo_pipeline (CoglVkPipelineCache     *cache,
                  CoglVkRenderFormatSetup *setup,
                  CoglPipeline            *pipeline,
                  const CoglVkVboMask     *mask,
                  VkPrimitiveTopology      topology,
                  gboolean                 opaque,
                  gboolean                 textured,
                  gboolean                 use_cold,
                  unsigned int             ycbcr,
                  GError                 **error)
{
  COGL_VK_PROFILE_SCOPE ("get_vbo_pipeline");
  CoglVkDevice *dev = cache->device;
  CoglVkVboPipeKey key;
  CoglVkVboPipeEntry *entry;
  CoglVkTexPipeExtras extras;
  CoglVkAssembleResult assembled = {0};
  g_autoptr (GError) assemble_err = NULL;
  g_autofree char *vert_glsl = NULL;
  g_autofree char *frag_glsl = NULL;
  g_autofree char *assemble_fp = NULL;
  g_autofree char *vert_key = NULL;
  g_autofree char *frag_key = NULL;
  VkShaderModule vert, frag;
  VkPipeline pipe = VK_NULL_HANDLE;
  VkPipelineLayout layout;
  g_autofree VkVertexInputBindingDescription *bindings =
    g_new0 (VkVertexInputBindingDescription, mask->n_bindings + 1);
  g_autofree VkVertexInputAttributeDescription *attrs =
    g_new0 (VkVertexInputAttributeDescription, mask->n_bindings + 1);
  g_autoptr (GBytes) vertex_layout = vertex_layout_key (mask);
  uint32_t n_bind = 0, n_attr = 0;
  VkPipelineVertexInputStateCreateInfo vi = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
  };
  gboolean use_assemble = FALSE;
  CoglDriver *drv;
  VkFrontFace ff = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  uint32_t vi_mask;

  if (!dev->vbo_pipes)
    dev->vbo_pipes = g_hash_table_new_full (pipe_key_hash, pipe_key_equal,
                                            NULL, vbo_pipe_entry_free);

  memset (&key, 0, sizeof (key));
  key.format = setup->format;
  key.samples = setup->samples ? setup->samples : VK_SAMPLE_COUNT_1_BIT;
  key.vertex_layout = vertex_layout;
  key.mask_bits = g_bytes_hash (vertex_layout);
  /* The complete vertex layout already includes strides and attribute types. */
  key.strides_hash = 0;
  key.opaque = opaque;
  key.textured = textured;
  key.use_cold = use_cold;
  key.ycbcr = ycbcr;
  if (pipeline && cogl_pipeline_get_n_layers (pipeline) > 0)
    {
      CoglSamplerCacheWrapMode ws, wt;

      peek_first_layer_wrap (pipeline, &ws, &wt);
      key.wrap_st = (uint32_t) ws | ((uint32_t) wt << 16);
    }

  use_assemble = want_assemble (pipeline, mask);
  if (use_assemble)
    {
      key.assemble = TRUE;
      key.use_cold = TRUE;
      assemble_fp = cogl_vk_assemble_pipeline_fingerprint (pipeline);
      key.assemble_fp_hash = g_str_hash (assemble_fp);
      g_strlcpy (key.assemble_fingerprint, assemble_fp,
                 sizeof (key.assemble_fingerprint));
    }

  drv = pipeline ? cogl_context_get_driver (pipeline->context) : NULL;
  if (drv && COGL_IS_DRIVER_VK (drv))
    ff = COGL_DRIVER_VK (drv)->pending_front_face;

  vi_mask = key.mask_bits ^ key.strides_hash;
  cogl_vk_pipe_state_key_from_pipeline (
    &key.state, pipeline, ff, !opaque, key.samples,
    setup->ds_format != VK_FORMAT_UNDEFINED,
    TRUE, topology, vi_mask);

  entry = g_hash_table_lookup (dev->vbo_pipes, &key);
  if (entry)
    {
      cogl_vk_debug_log (
        "tex",
        "vbo pipe HIT wrap_st=0x%x assemble=%d fp_hash=%u",
        key.wrap_st, key.assemble, key.assemble_fp_hash);
      cogl_vk_assemble_result_clear (&assembled);
      return entry->pipeline;
    }
  cogl_vk_debug_log (
    "tex",
    "vbo pipe MISS wrap_st=0x%x assemble=%d fp_hash=%u",
    key.wrap_st, key.assemble, key.assemble_fp_hash);

  /* Generate shader source only when the complete pipeline key misses. */
  if (want_assemble (pipeline, mask))
    {
      CoglVkAssembleVboAttrs vbo_attrs;
      g_autofree int *custom_components = g_new0 (int, mask->n_custom);
      g_autofree const char **custom_names = g_new0 (const char *, mask->n_custom);
      g_autofree gboolean *custom_as_int = g_new0 (gboolean, mask->n_custom);
      g_autofree gboolean *custom_unsigned = g_new0 (gboolean, mask->n_custom);
      int c;

      memset (&vbo_attrs, 0, sizeof (vbo_attrs));
      /* A point's attributes are constant over its covered fragments. In
       * particular, disabling point sprites must not interpolate its UVs.
       */
      vbo_attrs.flat_varyings = topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      vbo_attrs.n_texcoords = mask->n_texcoords;
      vbo_attrs.texcoord_layers = mask->texcoord_layers;
      vbo_attrs.custom_base = mask->custom_base;
      vbo_attrs.custom_components = custom_components;
      vbo_attrs.custom_names = custom_names;
      vbo_attrs.custom_as_int = custom_as_int;
      vbo_attrs.custom_unsigned = custom_unsigned;
      vbo_attrs.has_normal = mask->has_normal;
      vbo_attrs.normal_components =
        mask->normal_components > 0 ? mask->normal_components : 3;
      /* SSCALED + float `in` — do not use ivec path (see vk_format_for_attr).
       */
      vbo_attrs.position_as_int = FALSE;
      vbo_attrs.position_unsigned = FALSE;
      vbo_attrs.position_components =
        mask->position_components > 0 ? mask->position_components : 2;
      vbo_attrs.n_custom = mask->n_custom;
      if (pipeline && pipeline->context &&
          cogl_context_get_attribute_name_index_map (pipeline->context))
        {
          GArray *name_map =
            cogl_context_get_attribute_name_index_map (pipeline->context);

          for (c = 0; c < mask->n_custom; c++)
            {
              int ni = mask->custom_name_index[c];
              CoglAttributeNameState *ns;

              vbo_attrs.custom_components[c] =
                mask->custom_components[c] > 0 ? mask->custom_components[c] : 4;
              /* SSCALED/USCALED → float `in`; no ivec custom path. */
              vbo_attrs.custom_as_int[c] = FALSE;
              vbo_attrs.custom_unsigned[c] = FALSE;
              if (ni < 0 || ni >= (int) name_map->len)
                continue;
              ns = g_array_index (name_map, CoglAttributeNameState *, ni);
              if (!ns || !ns->name)
                continue;
              vbo_attrs.custom_names[c] = ns->name;
            }
        }

      if (cogl_vk_assemble_pipeline_glsl (pipeline,
                                          COGL_VK_ASSEMBLE_VERT_VBO,
                                          &vbo_attrs,
                                          &assembled, &assemble_err))
        {
          use_assemble = TRUE;
        }
      else
        {
          /* Unsupported limits or snippets must not silently lose layers. */
          if (assemble_err)
            g_propagate_error (error, g_steal_pointer (&assemble_err));
          else
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                 "VBO VERT_VBO assemble required for "
                                 "CUSTOM/NORMAL/multi-layer texcoord");
          return VK_NULL_HANDLE;
        }
    }


  build_vi (mask, bindings, attrs, &n_bind, &n_attr);
  if (!validate_vertex_input_capacity (&dev->limits, n_bind, n_attr, error))
    {
      cogl_vk_assemble_result_clear (&assembled);
      return VK_NULL_HANDLE;
    }
  for (uint32_t i = 0; i < n_bind; i++)
    if (bindings[i].stride > dev->limits.maxVertexInputBindingStride)
      {
        cogl_vk_assemble_result_clear (&assembled);
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                     "Vulkan vertex stride %u exceeds device limit %u",
                     bindings[i].stride, dev->limits.maxVertexInputBindingStride);
        return VK_NULL_HANDLE;
      }
  vi.vertexBindingDescriptionCount = n_bind;
  vi.pVertexBindingDescriptions = bindings;
  vi.vertexAttributeDescriptionCount = n_attr;
  vi.pVertexAttributeDescriptions = attrs;

  if (use_assemble)
    {
      g_autofree char *vert_final = NULL;
      g_autofree char *frag_final = NULL;

      vert_final = cogl_vk_glsl_finalize_cold_vert (cache, pipeline,
                                                    assembled.vert_glsl,
                                                    error);
      if (!vert_final)
        {
          cogl_vk_assemble_result_clear (&assembled);
          return VK_NULL_HANDLE;
        }

      frag_final = cogl_vk_glsl_finalize_cold_frag (cache, pipeline,
                                                    assembled.frag_glsl,
                                                    error);
      cogl_vk_assemble_result_clear (&assembled);
      if (!frag_final)
        return VK_NULL_HANDLE;

      vert_glsl = g_steal_pointer (&vert_final);
      frag_glsl = g_steal_pointer (&frag_final);
      if (mask->n_custom > 0 && cogl_vk_debug_enabled ("vbo"))
        {
          g_autoptr (GError) dump_err = NULL;
          char *vpath = g_strdup_printf ("/tmp/cogl-vk-custom-vert-%d.glsl",
                                         getpid ());
          char *fpath = g_strdup_printf ("/tmp/cogl-vk-custom-frag-%d.glsl",
                                         getpid ());
          if (!g_file_set_contents (vpath, vert_glsl, -1, &dump_err))
            cogl_vk_debug_log ("vbo", "  dump vert FAIL: %s",
                               dump_err->message);
          else
            cogl_vk_debug_log ("vbo", "  dumped vert GLSL → %s (%zu bytes)",
                               vpath, strlen (vert_glsl));
          g_clear_error (&dump_err);
          if (!g_file_set_contents (fpath, frag_glsl, -1, &dump_err))
            cogl_vk_debug_log ("vbo", "  dump frag FAIL: %s",
                               dump_err->message);
          else
            cogl_vk_debug_log ("vbo", "  dumped frag GLSL → %s (%zu bytes)",
                               fpath, strlen (frag_glsl));
          g_free (vpath);
          g_free (fpath);
        }
    }
  else
    {
      cogl_vk_assemble_result_clear (&assembled);
      /* VI demotes float P3 → R32G32; use vec4 vert (z=0,w=1). */
      vert_glsl = g_strdup (VBO_VERT);
      frag_glsl = g_strdup (textured ? VBO_FRAG_TEX : VBO_FRAG_SOLID);
    }

  if (!cache->spirv_cache)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "SPIR-V cache required for VBO draw");
      return VK_NULL_HANDLE;
    }

  /* A 32-bit layout hash is not a shader identity: two different custom
   * layouts must not retrieve each other's module from the SPIR-V cache.
   */
  {
    g_autofree char *vert_digest =
      g_compute_checksum_for_string (G_CHECKSUM_SHA256, vert_glsl, -1);
    g_autofree char *frag_digest =
      g_compute_checksum_for_string (G_CHECKSUM_SHA256, frag_glsl, -1);
    vert_key = g_strdup_printf ("vbo-v|%s", vert_digest);
    frag_key = g_strdup_printf ("vbo-f|%s", frag_digest);
  }
  vert = cogl_vk_spirv_cache_get_module (cache->spirv_cache, vert_key,
                                         VK_SHADER_STAGE_VERTEX_BIT,
                                         vert_glsl, error);
  if (vert == VK_NULL_HANDLE)
    return VK_NULL_HANDLE;
  frag = cogl_vk_spirv_cache_get_module (cache->spirv_cache, frag_key,
                                         VK_SHADER_STAGE_FRAGMENT_BIT,
                                         frag_glsl, error);
  if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE)
    return VK_NULL_HANDLE;

  cogl_vk_tex_pipe_extras_from_pipeline (pipeline, ff, &extras);
  extras.samples = key.samples;
  extras.has_topology = TRUE;
  extras.topology = topology;
  extras.vertex_input = &vi;
  extras.dynamic_blend_constants = TRUE;
  if (setup->ds_format != VK_FORMAT_UNDEFINED)
    {
      /* A/B: stencil_equal_keep ON/OFF did not change custom-attributes 245
       * (WaitIdle still died). Keep OFF; clip paths set this explicitly.
       */
      extras.stencil_equal_keep = FALSE;
      cogl_vk_debug_log ("vbo",
                         "  stencil_equal_keep OFF (ds_fmt=%d)",
                         (int) setup->ds_format);
    }

  cogl_vk_debug_log (
    "vbo",
    "  pipe_extras depth_test=%d depth_write=%d depth_cmp=%d stencil_eq=%d "
    "cull=0x%x front_face=%s ds_fmt=%d samples=%u "
    "n_vi_bind=%u n_vi_attr=%u pos_fmt_comps=%d",
    extras.depth_test_enable, extras.depth_write_enable,
    (int) extras.depth_compare_op,
    extras.stencil_equal_keep,
    (unsigned) extras.cull_mode,
    extras.front_face == VK_FRONT_FACE_CLOCKWISE ? "CW" : "CCW",
    (int) setup->ds_format,
    (unsigned) extras.samples, (unsigned) n_bind, (unsigned) n_attr,
    mask->position_components);

  layout = key.use_cold ? cache->pipe_layout_cold :
           (ycbcr ? cache->pipe_layout_ycbcr[ycbcr - 1] : cache->pipe_layout);
  if (layout == VK_NULL_HANDLE)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "VBO pipeline layout unavailable");
      return VK_NULL_HANDLE;
    }

  if (!cogl_vk_create_tex_pipeline (cache, setup->render_pass,
                                    vert, frag, layout,
                                    !opaque, &extras, &pipe, error))
    return VK_NULL_HANDLE;

  entry = g_new0 (CoglVkVboPipeEntry, 1);
  entry->key = key;
  entry->key.vertex_layout = g_bytes_ref (vertex_layout);
  entry->pipeline = pipe;
  g_hash_table_insert (dev->vbo_pipes, &entry->key, entry);
  return pipe;
}

/* Journal attributes share one pooled buffer. a->offset is the byte
 * offset of this draw's first vertex, not 0. Counting buf->size/stride
 * vertices from that offset reads past the mapping (drag a window →
 * SIGSEGV in read_attr_component_as_float, session drops back to GDM).
 */
static size_t
attr_vertex_count (CoglAttribute *a)
{
  CoglBuffer *buf;
  size_t avail;
  size_t element_size;

  buf = COGL_BUFFER (cogl_attribute_get_buffer (a));
  if (!buf || a->stride <= 0 || a->n_components < 1)
    return 0;
  if ((size_t) a->offset >= buf->size)
    return 0;
  avail = buf->size - (size_t) a->offset;
  element_size = (size_t) a->n_components * attr_type_size (a->type);
  if (element_size == 0 || element_size > avail)
    return 0;
  /* The last interleaved attribute only needs its own components, not
   * another full stride after its offset. Dropping it interpolates the
   * final vertex color/UV against uninitialized shadow storage.
   */
  return 1 + (avail - element_size) / (size_t) a->stride;
}

static size_t
shadow_bytes_for_attr (CoglAttribute *a)
{
  size_t n_verts;
  int out_comps;

  n_verts = attr_vertex_count (a);
  if (n_verts < 1)
    return 0;
  out_comps = a->n_components == 3 ? 4 : a->n_components;
  return n_verts * (size_t) out_comps * sizeof (float);
}

/* Journal rewrites the same CoglBuffer before vkQueueSubmit. A draw that
 * binds it directly paints every earlier quad with the last vertices
 * (pipeline-uniforms (5,5) stayed the clear). Copy the bytes into the
 * shadow ring; keep stride/format so the VI does not change.
 */
static size_t
raw_snapshot_bytes (CoglAttribute *a)
{
  CoglBuffer *buf;

  if (!a || attr_needs_float_expand (a))
    return 0;
  if (g_getenv ("COGL_VK_AB_NO_FLOAT_EXPAND"))
    return 0;
  buf = COGL_BUFFER (cogl_attribute_get_buffer (a));
  if (!buf || buf->size < 1)
    return 0;
  return buf->size;
}

/* Grow once before the resolve loop. Growing mid-loop destroys the VkBuffer
 * earlier expands already stored (ANV GPF in the next barrier/bind).
 */
static gboolean
preflight_float_shadow (CoglAttribute **attributes,
                        int             n_attributes,
                        CoglVkDevice   *dev,
                        GError        **error)
{
  size_t used = dev->vbo_float_shadow_used;
  int i;

  for (i = 0; i < n_attributes; i++)
    {
      CoglAttribute *a = attributes[i];

      if (!a)
        continue;
      if (g_getenv ("COGL_VK_AB_NO_FLOAT_EXPAND"))
        continue;
      if (attr_needs_float_expand (a))
        {
          used = (used + 15u) & ~((size_t) 15u);
          used += shadow_bytes_for_attr (a);
        }
      else if (raw_snapshot_bytes (a) > 0)
        {
          used = (used + 15u) & ~((size_t) 15u);
          used += raw_snapshot_bytes (a);
        }
    }
  return ensure_float_shadow_capacity (dev, used, error);
}

/* Byte copy of a journal VB. Stride and component count stay the same
 * so build_vi's R32G32 position path is unchanged.
 */
static gboolean
snapshot_raw_attr_to_shadow (CoglAttribute *a,
                             CoglVkDevice  *dev,
                             VkBuffer      *out_buf,
                             VkDeviceSize  *out_off,
                             GError       **error)
{
  CoglBuffer *buf;
  const uint8_t *data;
  void *mapped;
  size_t nbytes;
  size_t align_off;

  buf = COGL_BUFFER (cogl_attribute_get_buffer (a));
  if (!buf || buf->size < 1)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "raw snapshot: empty attribute buffer");
      return FALSE;
    }

  nbytes = buf->size;
  align_off = (dev->vbo_float_shadow_used + 15u) & ~((size_t) 15u);
  if (!ensure_float_shadow_capacity (dev, align_off + nbytes, error))
    return FALSE;

  data = _cogl_buffer_map (buf, COGL_BUFFER_ACCESS_READ, 0, error);
  if (!data)
    return FALSE;

  if (vkMapMemory (dev->device, dev->vbo_float_shadow_mem, 0,
                   dev->vbo_float_shadow_cap, 0, &mapped) != VK_SUCCESS)
    {
      cogl_buffer_unmap (buf);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "raw snapshot map failed");
      return FALSE;
    }

  memcpy ((uint8_t *) mapped + align_off, data, nbytes);
  vkUnmapMemory (dev->device, dev->vbo_float_shadow_mem);
  cogl_buffer_unmap (buf);

  *out_buf = dev->vbo_float_shadow_buf;
  *out_off = (VkDeviceSize) (align_off + (size_t) a->offset);
  dev->vbo_float_shadow_used = align_off + nbytes;
  return TRUE;
}

static gboolean
resolve_attrs (CoglAttribute      **attributes,
               int                  n_attributes,
               CoglVkDevice        *dev,
               const CoglVkVboMask *mask,
               VkBuffer            *out_bufs,
               VkDeviceSize        *out_offs,
               GError             **error)
{
  COGL_VK_PROFILE_SCOPE ("resolve_attrs");
  int i;
  gboolean have_pos = FALSE;

  if (!ensure_dummy (dev, error))
    return FALSE;

  if (!preflight_float_shadow (attributes, n_attributes, dev, error))
    return FALSE;

  /* Do not reset dev->vbo_float_shadow_used here — an open pass CB may still
   * reference earlier shadow ranges. Append until capacity forces realloc
   * (WaitIdle + free in ensure_float_shadow_capacity).
   */

  for (i = 0; i < mask->n_bindings; i++)
    {
      out_bufs[i] = dev->vbo_dummy_buf;
      out_offs[i] = 0;
    }
  out_offs[COGL_VK_VBO_LOC_TEXCOORD0] = 16;
  out_offs[COGL_VK_VBO_LOC_POINT_SIZE] = 24;
  out_offs[COGL_VK_VBO_LOC_NORMAL] = 28;

  for (i = 0; i < n_attributes; i++)
    {
      CoglAttribute *a = attributes[i];
      CoglBuffer *buf;
      VkBuffer vkbuf;
      int loc = -1;
      int layer;

      if (!a || !a->name_state)
        continue;
      switch (a->name_state->name_id)
        {
        case COGL_ATTRIBUTE_NAME_ID_POSITION_ARRAY:
          loc = COGL_VK_VBO_LOC_POSITION;
          have_pos = TRUE;
          break;
        case COGL_ATTRIBUTE_NAME_ID_COLOR_ARRAY:
          loc = COGL_VK_VBO_LOC_COLOR;
          break;
        case COGL_ATTRIBUTE_NAME_ID_TEXTURE_COORD_ARRAY:
          layer = a->name_state->layer_number;
          loc = texcoord_location (texcoord_slot (mask, layer));
          break;
        case COGL_ATTRIBUTE_NAME_ID_POINT_SIZE_ARRAY:
          loc = COGL_VK_VBO_LOC_POINT_SIZE;
          break;
        case COGL_ATTRIBUTE_NAME_ID_NORMAL_ARRAY:
          loc = COGL_VK_VBO_LOC_NORMAL;
          break;
        case COGL_ATTRIBUTE_NAME_ID_CUSTOM_ARRAY:
          {
            int slot = 0;
            for (int j = 0; j < i; j++)
              if (attributes[j] && attributes[j]->name_state &&
                  attributes[j]->name_state->name_id == COGL_ATTRIBUTE_NAME_ID_CUSTOM_ARRAY)
                slot++;
            loc = mask->custom_base + slot;
          }
          break;
        default:
          break;
        }
      if (loc < 0 || loc >= mask->n_bindings)
        continue;
      /* A/B gate: COGL_VK_AB_NO_FLOAT_EXPAND skips CPU expand. */
      if (!g_getenv ("COGL_VK_AB_NO_FLOAT_EXPAND") &&
          attr_needs_float_expand (a))
        {
          cogl_vk_debug_log (
            "vbo",
            "resolve EXPAND loc=%d name=%s type=%d norm=%d",
            loc,
            a->name_state->name ? a->name_state->name : "?",
            (int) a->type, a->normalized);
          if (!expand_attr_to_float_shadow (a, dev, &vkbuf, &out_offs[loc],
                                            error))
            return FALSE;
          out_bufs[loc] = vkbuf;
          continue;
        }
      cogl_vk_debug_log (
        "vbo",
        "resolve RAW loc=%d name=%s type=%d need_expand=%d",
        loc,
        a->name_state->name ? a->name_state->name : "?",
        (int) a->type, attr_needs_float_expand (a));
      buf = COGL_BUFFER (cogl_attribute_get_buffer (a));
      if (!ensure_buf (buf, error))
        return FALSE;
      /* Journal recycles this CoglBuffer before submit. Bind a copy.
       * AB skips the copy and keeps the live buffer (old black-pixel bug).
       */
      if (g_getenv ("COGL_VK_AB_NO_FLOAT_EXPAND"))
        {
          vkbuf = peek_buf (buf);
          out_offs[loc] = (VkDeviceSize) a->offset;
        }
      else if (!snapshot_raw_attr_to_shadow (a, dev, &vkbuf, &out_offs[loc],
                                             error))
        return FALSE;
      out_bufs[loc] = vkbuf;
    }

  if (!have_pos)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "VBO draw requires POSITION");
      return FALSE;
    }

  if (cogl_vk_debug_enabled ("vbo"))
    {
      int loc;
      for (loc = 0; loc < 5; loc++)
        {
          cogl_vk_debug_log (
            "vbo",
            "resolve loc=%d vkbuf=%p off=%llu dummy=%d",
            loc,
            (void *) (uintptr_t) out_bufs[loc],
            (unsigned long long) out_offs[loc],
            out_bufs[loc] == dev->vbo_dummy_buf);
        }
      /* Dump POSITION buffer (custom-attributes SHORT→black A/B). */
      for (i = 0; i < n_attributes; i++)
        {
          CoglAttribute *a = attributes[i];
          CoglBuffer *buf;
          const uint8_t *data;
          const int16_t *s;
          const float *f;

          if (!a || !a->name_state ||
              a->name_state->name_id != COGL_ATTRIBUTE_NAME_ID_POSITION_ARRAY)
            continue;
          buf = COGL_BUFFER (cogl_attribute_get_buffer (a));
          data = _cogl_buffer_map (buf, COGL_BUFFER_ACCESS_READ, 0, NULL);
          if (!data)
            {
              cogl_vk_debug_log ("vbo", "  pos dump map FAIL");
              break;
            }
          if (a->type == COGL_ATTRIBUTE_TYPE_FLOAT)
            {
              const float *p0 = (const float *) (data + a->offset);
              const float *p1 =
                (const float *) (data + a->offset + a->stride);
              int nc = a->n_components;

              const float *p2 =
                (const float *) (data + a->offset + a->stride * 2);
              const float *p3 =
                (const float *) (data + a->offset + a->stride * 3);

              cogl_vk_debug_log (
                "vbo",
                "  pos FLOAT comps=%d stride=%zu off=%zu "
                "v0=(%.5g,%.5g,%.5g) v1=(%.5g,%.5g,%.5g) "
                "v2=(%.5g,%.5g,%.5g) v3=(%.5g,%.5g,%.5g)",
                nc, (size_t) a->stride, (size_t) a->offset,
                p0[0], nc > 1 ? p0[1] : 0.f, nc > 2 ? p0[2] : 0.f,
                p1[0], nc > 1 ? p1[1] : 0.f, nc > 2 ? p1[2] : 0.f,
                p2[0], nc > 1 ? p2[1] : 0.f, nc > 2 ? p2[2] : 0.f,
                p3[0], nc > 1 ? p3[1] : 0.f, nc > 2 ? p3[2] : 0.f);
            }
          else
            {
              s = (const int16_t *) (data + a->offset);
              f = (const float *) (data + a->offset + 4);
              cogl_vk_debug_log (
                "vbo",
                "  pos dump type=%d comps=%d stride=%zu off=%zu "
                "v0_xy=(%d,%d) color0=(%.3f,%.3f,%.3f,%.3f) v1_xy=(%d,%d)",
                (int) a->type, a->n_components, (size_t) a->stride,
                (size_t) a->offset,
                (int) s[0], (int) s[1],
                f[0], f[1], f[2], f[3],
                (int) ((const int16_t *) (data + a->offset + a->stride))[0],
                (int) ((const int16_t *) (data + a->offset + a->stride))[1]);
            }
          cogl_buffer_unmap (buf);
          break;
        }
      /* If texcoord0 is real, dump first vertex UV floats (class-1 root
       * cause).
       */
      for (i = 0; i < n_attributes; i++)
        {
          CoglAttribute *a = attributes[i];
          CoglBuffer *buf;
          const uint8_t *data;
          const float *uv;

          if (!a || !a->name_state ||
              a->name_state->name_id !=
              COGL_ATTRIBUTE_NAME_ID_TEXTURE_COORD_ARRAY ||
              a->name_state->layer_number != 0)
            continue;
          buf = COGL_BUFFER (cogl_attribute_get_buffer (a));
          data = _cogl_buffer_map (buf, COGL_BUFFER_ACCESS_READ, 0, NULL);
          if (!data)
            {
              cogl_vk_debug_log ("vbo", "  tex0 UV map FAIL");
              break;
            }
          uv = (const float *) (data + a->offset);
          cogl_vk_debug_log (
            "vbo",
            "  tex0 UV v0=(%g,%g) stride=%zu off=%zu "
            "hyp: UV==1.0+REPEAT→texel0; UV unused→dummy(0,0)→white",
            uv[0], a->n_components > 1 ? uv[1] : 0.f,
            (size_t) a->stride, (size_t) a->offset);
          cogl_buffer_unmap (buf);
          break;
        }
    }

  return TRUE;
}

static gboolean
upload_indices (CoglVkPass   *pass,
                const void   *idx,
                size_t        size,
                VkBuffer     *out_buffer,
                GError      **error)
{
  CoglVkDevice *dev = pass->device;
  CoglVkTransientBuffer buffer = { 0 };
  VkBufferCreateInfo bi = {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size = size,
    .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
  };
  VkMemoryAllocateInfo ai = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
  };
  VkMemoryRequirements reqs;
  VkPhysicalDeviceMemoryProperties mp;
  VkMappedMemoryRange range = {
    .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
    .size = VK_WHOLE_SIZE,
  };
  VkResult res;
  uint32_t mi;
  void *mapped = NULL;

  g_return_val_if_fail (size > 0, FALSE);
  res = vkCreateBuffer (dev->device, &bi, NULL, &buffer.buffer);
  if (res != VK_SUCCESS)
    goto fail;
  vkGetBufferMemoryRequirements (dev->device, buffer.buffer, &reqs);
  vkGetPhysicalDeviceMemoryProperties (dev->physical_device, &mp);
  for (mi = 0; mi < mp.memoryTypeCount; mi++)
    if ((reqs.memoryTypeBits & (1u << mi)) &&
        (mp.memoryTypes[mi].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
      break;
  if (mi == mp.memoryTypeCount)
    {
      vkDestroyBuffer (dev->device, buffer.buffer, NULL);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "No host-visible memory for converted indices");
      return FALSE;
    }
  ai.memoryTypeIndex = mi;
  ai.allocationSize = reqs.size;
  res = vkAllocateMemory (dev->device, &ai, NULL, &buffer.memory);
  if (res != VK_SUCCESS)
    goto fail;
  res = vkBindBufferMemory (dev->device, buffer.buffer, buffer.memory, 0);
  if (res != VK_SUCCESS)
    goto fail;
  res = vkMapMemory (dev->device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &mapped);
  if (res != VK_SUCCESS)
    goto fail;
  memcpy (mapped, idx, bi.size);
  if (!(mp.memoryTypes[mi].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
    {
      range.memory = buffer.memory;
      res = vkFlushMappedMemoryRanges (dev->device, 1, &range);
    }
  vkUnmapMemory (dev->device, buffer.memory);
  if (res != VK_SUCCESS)
    goto fail;

  if (!pass->fence->transient_buffers)
    pass->fence->transient_buffers =
      g_array_new (FALSE, FALSE, sizeof (CoglVkTransientBuffer));
  g_array_append_val (pass->fence->transient_buffers, buffer);
  *out_buffer = buffer.buffer;
  return TRUE;

fail:
  if (buffer.buffer != VK_NULL_HANDLE)
    vkDestroyBuffer (dev->device, buffer.buffer, NULL);
  if (buffer.memory != VK_NULL_HANDLE)
    vkFreeMemory (dev->device, buffer.memory, NULL);
  cogl_vk_device_note_vk_result (dev, res);
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "Converted index upload failed (%d)", (int) res);
  return FALSE;
}

/* Check in index units before multiplying by the element size. This also
 * covers direct u16/u32 draws, which never map the buffer on the CPU.
 */
static gboolean
validate_index_range (size_t    buffer_size,
                      size_t    index_size,
                      int       first,
                      int       count,
                      GError  **error)
{
  if (index_size == 0 || first < 0 || count < 0 ||
      (size_t) first > buffer_size / index_size ||
      (size_t) count > buffer_size / index_size - (size_t) first)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Index draw range exceeds the index buffer");
      return FALSE;
    }

  return TRUE;
}

static gboolean
draw_converted_indices (CoglVkPass       *pass,
                        CoglBuffer       *source,
                        CoglIndicesType   type,
                        int               first,
                        int               count,
                        gboolean          line_loop,
                        GError          **error)
{
  g_autofree void *indices = NULL;
  gboolean wide = source ? type == COGL_INDICES_TYPE_UNSIGNED_INT : count > 65536;
  size_t index_size = wide ? sizeof (uint32_t) : sizeof (uint16_t);
  size_t out_count;
  VkBuffer buffer;

  if (first < 0 || count < 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Negative index draw range");
      return FALSE;
    }
  if (source &&
      !validate_index_range (cogl_buffer_get_size (source),
                             cogl_indices_type_get_size (type),
                             first, count, error))
    return FALSE;
  if (count == 0 || (line_loop && count < 2))
    return TRUE;

  /* count is signed, but a closing index may take the result past INT_MAX. */
  out_count = (size_t) count + (line_loop ? 1 : 0);
  indices = g_try_malloc_n (out_count, index_size);
  if (!indices)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                           "Failed to allocate converted indices");
      return FALSE;
    }

  if (source)
    {
      const uint8_t *mapped;
      size_t source_size = cogl_indices_type_get_size (type);
      gboolean already_mapped = source->flags & COGL_BUFFER_FLAG_MAPPED;

      /* The framebuffer caller maps before acquiring this draw's pass. */
      mapped = already_mapped ? source->data :
               _cogl_buffer_map (source, COGL_BUFFER_ACCESS_READ, 0, error);
      if (!mapped)
        return FALSE;
      mapped += (size_t) first * source_size;
      if (type == COGL_INDICES_TYPE_UNSIGNED_BYTE)
        {
          uint16_t *out = indices;

          for (int k = 0; k < count; k++)
            out[k] = mapped[k];
        }
      else
        memcpy (indices, mapped, (size_t) count * source_size);
      if (!already_mapped)
        cogl_buffer_unmap (source);
    }
  else if (wide)
    {
      uint32_t *out = indices;

      for (int k = 0; k < count; k++)
        out[k] = k;
    }
  else
    {
      uint16_t *out = indices;

      for (int k = 0; k < count; k++)
        out[k] = k;
    }

  if (line_loop)
    memcpy ((uint8_t *) indices + (size_t) count * index_size,
            indices, index_size);
  if (!upload_indices (pass, indices, out_count * index_size, &buffer, error))
    return FALSE;

  /* Preserve glDrawArrays' first through vertexOffset; indexed draws have
   * already applied first to the source index range.
   */
  vkCmdBindIndexBuffer (pass->command_buffer, buffer, 0,
                        wide ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
  vkCmdDrawIndexed (pass->command_buffer, (uint32_t) out_count, 1, 0,
                    source ? 0 : first, 0);
  return TRUE;
}

static gboolean
draw_line_loop (CoglVkPass  *pass,
                int          first,
                int          count,
                GError     **error)
{
  return draw_converted_indices (pass, NULL, COGL_INDICES_TYPE_UNSIGNED_SHORT,
                                 first, count, TRUE, error);
}

/* Half-float is always R16G16B16A16, and AFIRST 8888 is a packed
 * BGRA/RGBA VkFormat whose channels are not Cogl address order.
 * Clear already writes that order; fragment output has to match or
 * paint-fp16 / paint-rgb8 read red out of another channel.
 */
static float
color_store_order (CoglVkImage *dst)
{
  CoglPixelFormat fmt;

  if (!dst)
    return 0.f;

  fmt = dst->cogl_format;

  if (dst->format == VK_FORMAT_R16G16B16A16_SFLOAT)
    {
      if ((fmt & COGL_BGR_BIT) && (fmt & COGL_AFIRST_BIT))
        return 3.f;
      if (fmt & COGL_AFIRST_BIT)
        return 2.f;
      if (fmt & COGL_BGR_BIT)
        return 1.f;
      return 0.f;
    }

  /* XRGB/ARGB → B8G8R8A8. Memory A,R,G,B needs shader (G, R, A, B). */
  if ((fmt & COGL_AFIRST_BIT) &&
      (dst->format == VK_FORMAT_B8G8R8A8_UNORM ||
       dst->format == VK_FORMAT_B8G8R8A8_SRGB))
    return 4.f;

  /* XBGR/ABGR → R8G8B8A8. Memory A,B,G,R needs shader (A, B, G, R). */
  if ((fmt & COGL_AFIRST_BIT) &&
      (dst->format == VK_FORMAT_R8G8B8A8_UNORM ||
       dst->format == VK_FORMAT_R8G8B8A8_SRGB))
    return 3.f;

  return 0.f;
}

static gboolean
draw_common (CoglVkPass              *pass,
             CoglVkPipelineCache     *cache,
             CoglVkRenderFormatSetup *setup,
             CoglVkImage             *dst,
             CoglVkImage             *src,
             VkDescriptorSet          src_ds,
             CoglPipeline            *pipeline,
             CoglFramebuffer         *framebuffer,
             CoglVerticesMode         mode,
             CoglAttribute          **attributes,
             int                      n_attributes,
             gboolean                 opaque,
             float                    alpha,
             gboolean                 indexed,
             CoglIndices             *indices,
             int                      first,
             int                      count,
             VkPipeline               pipe_override,
             GError                 **error)
{
  CoglVkDevice *dev = cache->device;
  g_auto (CoglVkVboMask) mask = {0};
  CoglVkVertPush vert_push;
  float color_mul[4];
  float wrap_pc[2];
  VkPipeline pipe;
  VkPipelineLayout layout;
  g_autofree VkBuffer *vbufs = NULL;
  g_autofree VkDeviceSize *voffs = NULL;
  VkDescriptorSet bind_ds = src_ds;
  VkDescriptorSet cold_ds = VK_NULL_HANDLE;
  VkDescriptorSet transient_ds = VK_NULL_HANDLE;
  gboolean pushed = FALSE;
  gboolean textured = (src != NULL && src_ds != VK_NULL_HANDLE);
  gboolean use_cold;
  gboolean cold_override;
  VkPrimitiveTopology topology = topology_from_mode (mode);
  gboolean line_loop = (mode == COGL_VERTICES_MODE_LINE_LOOP);

  if (first < 0 || count < 0 || (indexed && !indices))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Invalid VBO draw range or missing indices");
      return FALSE;
    }
  if (count == 0 || (line_loop && count < 2))
    return TRUE;
  if (indexed &&
      !validate_index_range (cogl_buffer_get_size (
                               COGL_BUFFER (cogl_indices_get_buffer (indices))),
                             cogl_indices_type_get_size (
                               cogl_indices_get_indices_type (indices)),
                             first, count, error))
    return FALSE;

  if (!collect_mask (attributes, n_attributes, &mask))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Invalid VBO attributes");
      return FALSE;
    }

  if (!validate_vertex_input_capacity (&dev->limits, mask.n_bindings,
                                       mask.n_bindings, error))
    return FALSE;
  vbufs = g_new0 (VkBuffer, mask.n_bindings);
  voffs = g_new0 (VkDeviceSize, mask.n_bindings);
  if (!resolve_attrs (attributes, n_attributes, cache->device, &mask,
                      vbufs, voffs, error))
    return FALSE;

  cold_override = (pipe_override != VK_NULL_HANDLE &&
                   !is_hot_tex_pipe (setup, pipe_override));
  /* Cold layout = sampler[N]+UBO. Solid+snippet uses 1×1 white for layer0.
   * CUSTOM/NORMAL/texcoord>0 also force cold so fixed VBO_VERT is never used.
   */
  use_cold = (cache->pipe_layout_cold != VK_NULL_HANDLE &&
              (cold_override || want_assemble (pipeline, &mask)));

  /* Solid (no layer tex): 1×1 white + DS so we use VBO_FRAG_TEX
   * (texture * v_color * alpha), matching textured draws that already work.
   */
  gboolean used_dummy_white = FALSE;
  if (!src)
    {
      if (!cogl_vk_pipeline_ensure_dummy_white (cache, error))
        return FALSE;
      src = cache->dummy_white;
      if (bind_ds == VK_NULL_HANDLE)
        bind_ds = cache->dummy_white_ds;
      textured = TRUE;
      used_dummy_white = TRUE;
    }

  if (use_cold &&
      !cogl_vk_pipeline_prepare_cold_images (pass, cache, pipeline, &src, error))
    return FALSE;

  if (textured || use_cold)
    {
      cogl_vk_pass_end_render (pass);
      cogl_vk_pipeline_barrier_layer_textures (pass, pipeline, src, use_cold);
    }

  if (!cogl_vk_pass_begin_render (pass, cache, dst, setup,
                                  dst->width, dst->height, error))
    return FALSE;

  if (pipeline)
    cogl_vk_pass_apply_depth_range (pass, pipeline);

  /* Honor pipe_override when it is a hot tex pipe alias. Cold overrides from
   * the PUSH_QUAD assemble cache are not VI-compatible with VBO — build a
   * VERT_VBO pipe instead, but still take the cold DS/layout path.
   * Never reuse a hot tex pipe when attrs need VERT_VBO assemble.
   */
  if (pipe_override != VK_NULL_HANDLE && is_hot_tex_pipe (setup, pipe_override) &&
      !vbo_mask_needs_assemble (&mask) && !src->is_ycbcr)
    {
      pipe = pipe_override;
      layout = cache->pipe_layout;
      use_cold = FALSE;
    }
  else
    {
      pipe = get_vbo_pipeline (cache, setup, pipeline, &mask, topology,
                               opaque, textured || use_cold, use_cold,
                               src->is_ycbcr ? src->ycbcr_index + 1 : 0, error);
      if (pipe == VK_NULL_HANDLE)
        return FALSE;
      layout = use_cold ? cache->pipe_layout_cold :
               (src->is_ycbcr ? cache->pipe_layout_ycbcr[src->ycbcr_index] : cache->pipe_layout);
    }

  if (layout == VK_NULL_HANDLE)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "VBO layout unavailable");
      return FALSE;
    }

  if (use_cold && src)
    {
      /* Root-cause log: cold VBO + vertex snippets read UBO flip_vector.
       * Diff vs OK primitive: cold=0 / WaitIdle done; custom: cold=1 / hang.
       */
      cogl_vk_debug_log (
        "vbo",
        "  prepare_cold_ds (hyp: UBO stageFlags must include VERTEX for "
        "flip_vector; n_custom=%d)",
        mask.n_custom);
      if (!cogl_vk_pipeline_prepare_cold_ds (cache, pipeline, src,
                                             &cold_ds, error))
        return FALSE;
      bind_ds = cold_ds;
      textured = TRUE;
    }
  else if (textured && pipeline && bind_ds != VK_NULL_HANDLE &&
           !(src && src->is_ycbcr) &&
           !used_dummy_white &&
           cogl_pipeline_get_n_layers (pipeline) > 0)
    {
      CoglDriver *drv = cogl_context_get_driver (pipeline->context);
      CoglPipelineLayer *layer = peek_first_layer (pipeline);
      if (COGL_IS_DRIVER_VK (drv) && layer)
        {
          const CoglSamplerCacheEntry *entry =
            _cogl_pipeline_layer_get_sampler_state (layer);
          VkSampler samp =
            cogl_driver_vk_peek_sampler (COGL_DRIVER_VK (drv), entry);
          if (cogl_vk_debug_enabled ("vbo") || cogl_vk_debug_enabled ("tex"))
            {
              cogl_vk_debug_log (
                "vbo",
                "sampler layer0 entry=%p wrap_s=%d wrap_t=%d samp=%p "
                "default_cache_samp=%p (AUTOMATIC/CLAMP→clamp; GL_REPEAT→repeat)",
                (void *) entry,
                entry ? (int) entry->wrap_mode_s : -1,
                entry ? (int) entry->wrap_mode_t : -1,
                (void *) (uintptr_t) samp,
                (void *) (uintptr_t) cache->sampler);
            }
          /* Push descriptor when available (no per-draw set); see
           * cmd_draw_texture. Otherwise a fresh per-draw set.
           */
          if (cache->device->has_push_descriptor)
            {
              cogl_vk_pipeline_push_texture_ds (cache, pass->command_buffer,
                                                src->view, samp);
              pushed = TRUE;
            }
          else
            {
              if (!cogl_vk_pipeline_alloc_texture_ds (cache, FALSE,
                                                      &transient_ds, error))
                return FALSE;
              cogl_vk_pipeline_update_texture_ds (cache, transient_ds,
                                                  src->view, FALSE, samp);
              bind_ds = transient_ds;
            }
        }
    }
  else if (used_dummy_white && cache->device->has_push_descriptor)
    {
      /* Solid draw with push descriptors: the shared dummy_white_ds was not
       * allocated (its layout is push-only), so push the 1×1 white view with
       * the default sampler straight into the CB.
       */
      cogl_vk_pipeline_push_texture_ds (cache, pass->command_buffer,
                                        cache->dummy_white->view,
                                        VK_NULL_HANDLE);
      pushed = TRUE;
    }

  fill_mvp (framebuffer, pipeline, &vert_push);

  /* GL: missing color attr → pipeline constant color as cogl_color_in.
   * has_color → vertex already holds full RGBA (journal bakes pipeline
   * color into the attribute). Do NOT fold pipeline alpha again into
   * color_mul.a — that double-multiplied journal 0.2×0.2 → near-black
   * (and with stale host readback looked like white).
   */
  if (mask.has_color || !pipeline)
    {
      color_mul[0] = color_mul[1] = color_mul[2] = color_mul[3] = 1.f;
      (void) alpha;
    }
  else
    {
      CoglColor pipe_color;

      cogl_pipeline_get_color (pipeline, &pipe_color);
      color_mul[0] = cogl_color_get_red (&pipe_color);
      color_mul[1] = cogl_color_get_green (&pipe_color);
      color_mul[2] = cogl_color_get_blue (&pipe_color);
      color_mul[3] = cogl_color_get_alpha (&pipe_color);
    }

  wrap_pc[0] = 0.f;
  wrap_pc[1] = 0.f;
  if (pipeline && cogl_pipeline_get_n_layers (pipeline) > 0)
    {
      CoglSamplerCacheWrapMode ws, wt;

      peek_first_layer_wrap (pipeline, &ws, &wt);
      wrap_pc[0] = cogl_vk_shader_wrap_mode ((int) ws);
      wrap_pc[1] = cogl_vk_shader_wrap_mode ((int) wt);
      cogl_vk_debug_log (
        "tex",
        "vbo wrap_pc=(%.0f,%.0f) enum=(%d,%d) first=%d",
        wrap_pc[0], wrap_pc[1], (int) ws, (int) wt, first);
    }

  cogl_vk_debug_vbo_draw (pass, framebuffer, pipeline, mode, first, count,
                          opaque, alpha, textured, use_cold,
                          used_dummy_white, src, dst, &vert_push,
                          attributes, n_attributes,
                          mask.has_color,
                          mask.has_texcoord0,
                          (unsigned int) mask.color_type,
                          (unsigned int) mask.color_components,
                          mask.normalized_color, pipe, bind_ds,
                          VK_CULL_MODE_NONE,
                          VK_FRONT_FACE_COUNTER_CLOCKWISE,
                          topology);

  cogl_vk_debug_log ("vbo",
                     "  color_mul=(%.4f %.4f %.4f %.4f) pos_comps=%d "
                     "strides=[%u %u %u %u %u]",
                     color_mul[0], color_mul[1], color_mul[2], color_mul[3],
                     mask.position_components,
                     mask.strides[0], mask.strides[1], mask.strides[2],
                     mask.strides[3], mask.strides[4]);

  vkCmdBindPipeline (pass->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                     pipe);
  if (pipeline)
    cogl_vk_pass_apply_blend_constants (pass, pipeline);

  if (textured && !pushed && !use_cold && src && !src->is_ycbcr &&
      cache->device->has_push_descriptor)
    {
      cogl_vk_pipeline_push_texture_ds (cache, pass->command_buffer,
                                        src->view, VK_NULL_HANDLE);
      pushed = TRUE;
    }

  /* Push descriptors already embedded the sampler in the CB; the cold and
   * fallback paths still bind a set object here.
   */
  if (textured && !pushed)
    {
      vkCmdBindDescriptorSets (pass->command_buffer,
                               VK_PIPELINE_BIND_POINT_GRAPHICS,
                               layout, 0, 1, &bind_ds, 0, NULL);
    }

  /* Locations are packed, so no unused gaps need binding. */
  vkCmdBindVertexBuffers (pass->command_buffer, 0, mask.n_bindings,
                          vbufs, voffs);
  vkCmdPushConstants (pass->command_buffer, layout,
                      VK_SHADER_STAGE_VERTEX_BIT, 0,
                      sizeof (vert_push), &vert_push);
  {
    float frag_pc[8] = { 0 };

    memcpy (frag_pc, color_mul, sizeof (color_mul));
    frag_pc[4] = wrap_pc[0];
    frag_pc[5] = wrap_pc[1];
    /* VK_COMPARE_OP_NEVER is 0. ANV still shades that draw (pixel stays
     * the last color). Kill it here; other compare ops stay on the PSO.
     */
    if (pipeline)
      {
        CoglPipeline *depth_auth =
          _cogl_pipeline_get_authority (pipeline, COGL_PIPELINE_STATE_DEPTH);

        if (depth_auth && depth_auth->has_big_state &&
            depth_auth->big_state &&
            depth_auth->big_state->depth_state.test_enabled &&
            depth_auth->big_state->depth_state.test_function ==
            COGL_DEPTH_TEST_FUNCTION_NEVER)
          frag_pc[6] = 1.f;
      }
    frag_pc[7] = color_store_order (dst);
    vkCmdPushConstants (pass->command_buffer, layout,
                        VK_SHADER_STAGE_FRAGMENT_BIT, sizeof (vert_push),
                        sizeof (frag_pc), frag_pc);
  }

  if (line_loop && !indexed)
    {
      if (!draw_line_loop (pass, first, count, error))
        {
          if (transient_ds != VK_NULL_HANDLE)
            cogl_vk_pipeline_free_texture_ds (cache, FALSE, transient_ds);
          if (cold_ds != VK_NULL_HANDLE)
            cogl_vk_pipeline_free_cold_ds (cache, cold_ds);
          return FALSE;
        }
    }
  else if (indexed)
    {
      CoglBuffer *ibuf = COGL_BUFFER (cogl_indices_get_buffer (indices));
      CoglIndicesType itype = cogl_indices_get_indices_type (indices);
      size_t isize = cogl_indices_type_get_size (itype);
      gboolean success;

      if (itype == COGL_INDICES_TYPE_UNSIGNED_BYTE || line_loop)
        success = draw_converted_indices (pass, ibuf, itype, first, count,
                                          line_loop, error);
      else
        {
          success = ensure_buf (ibuf, error);
          if (success)
            {
              VkIndexType vk_itype = itype == COGL_INDICES_TYPE_UNSIGNED_INT
                ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
              VkDeviceSize ioff = (VkDeviceSize) isize * (VkDeviceSize) first;

              vkCmdBindIndexBuffer (pass->command_buffer, peek_buf (ibuf),
                                    ioff, vk_itype);
              vkCmdDrawIndexed (pass->command_buffer, (uint32_t) count,
                                1, 0, 0, 0);
            }
        }
      if (!success)
        {
          if (transient_ds != VK_NULL_HANDLE)
            cogl_vk_pipeline_free_texture_ds (cache, FALSE, transient_ds);
          if (cold_ds != VK_NULL_HANDLE)
            cogl_vk_pipeline_free_cold_ds (cache, cold_ds);
          return FALSE;
        }
    }
  else
    {
      vkCmdDraw (pass->command_buffer, (uint32_t) count, 1,
                 (uint32_t) first, 0);
      cogl_vk_debug_log ("vbo",
                         "  vkCmdDraw first=%d count=%d indexed=0",
                         first, count);
    }

  if (transient_ds != VK_NULL_HANDLE)
    cogl_vk_pipeline_free_texture_ds (cache, FALSE, transient_ds);
  if (cold_ds != VK_NULL_HANDLE)
    cogl_vk_pipeline_free_cold_ds (cache, cold_ds);

  return TRUE;
}

gboolean
cogl_vk_cmd_draw_vbo (CoglVkPass              *pass,
                      CoglVkPipelineCache     *cache,
                      CoglVkRenderFormatSetup *setup,
                      CoglVkImage             *dst,
                      CoglVkImage             *src,
                      VkDescriptorSet          src_ds,
                      CoglPipeline            *pipeline,
                      CoglFramebuffer         *framebuffer,
                      CoglVerticesMode         mode,
                      int                      first_vertex,
                      int                      n_vertices,
                      CoglAttribute          **attributes,
                      int                      n_attributes,
                      gboolean                 opaque,
                      float                    alpha,
                      VkPipeline               pipe_override,
                      GError                 **error)
{
  return draw_common (pass, cache, setup, dst, src, src_ds, pipeline,
                      framebuffer, mode, attributes, n_attributes,
                      opaque, alpha, FALSE, NULL,
                      first_vertex, n_vertices, pipe_override, error);
}

gboolean
cogl_vk_cmd_draw_indexed_vbo (CoglVkPass              *pass,
                              CoglVkPipelineCache     *cache,
                              CoglVkRenderFormatSetup *setup,
                              CoglVkImage             *dst,
                              CoglVkImage             *src,
                              VkDescriptorSet          src_ds,
                              CoglPipeline            *pipeline,
                              CoglFramebuffer         *framebuffer,
                              CoglVerticesMode         mode,
                              int                      first_index,
                              int                      n_indices,
                              CoglIndices             *indices,
                              CoglAttribute          **attributes,
                              int                      n_attributes,
                              gboolean                 opaque,
                              float                    alpha,
                              VkPipeline               pipe_override,
                              GError                 **error)
{
  return draw_common (pass, cache, setup, dst, src, src_ds, pipeline,
                      framebuffer, mode, attributes, n_attributes,
                      opaque, alpha, TRUE, indices,
                      first_index, n_indices, pipe_override, error);
}
