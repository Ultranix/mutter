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
#include <math.h>
#include <string.h>
#include "tests/cogl-test-utils.h"
#include "cogl/cogl-offscreen-private.h"
#include "cogl/driver/vk/cogl-driver-vk-private.h"
#include "cogl/driver/vk/cogl-framebuffer-vk-private.h"
#include "cogl/driver/vk/cogl-texture-driver-vk-private.h"

static CoglVkDevice *device;

typedef struct {
  VkBuffer buffer;
  VkDeviceMemory memory;
  void *map;
} HostBuffer;

static HostBuffer
host_buffer_new (size_t size)
{
  HostBuffer buffer = { 0 };
  VkMemoryRequirements req;
  VkPhysicalDeviceMemoryProperties props;
  VkBufferCreateInfo info = {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size = size,
    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
  };
  VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
  const VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  unsigned int i;

  g_assert_cmpint (vkCreateBuffer (device->device, &info, NULL, &buffer.buffer), ==, VK_SUCCESS);
  vkGetBufferMemoryRequirements (device->device, buffer.buffer, &req);
  vkGetPhysicalDeviceMemoryProperties (device->physical_device, &props);
  for (i = 0; i < props.memoryTypeCount; i++)
    if ((req.memoryTypeBits & (1u << i)) &&
        (props.memoryTypes[i].propertyFlags & required) == required)
      break;
  g_assert_cmpuint (i, <, props.memoryTypeCount);
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = i;
  g_assert_cmpint (vkAllocateMemory (device->device, &alloc, NULL, &buffer.memory), ==, VK_SUCCESS);
  g_assert_cmpint (vkBindBufferMemory (device->device, buffer.buffer, buffer.memory, 0), ==, VK_SUCCESS);
  g_assert_cmpint (vkMapMemory (device->device, buffer.memory, 0, size, 0, &buffer.map), ==, VK_SUCCESS);
  return buffer;
}

static void
host_buffer_free (HostBuffer *buffer)
{
  vkUnmapMemory (device->device, buffer->memory);
  vkDestroyBuffer (device->device, buffer->buffer, NULL);
  vkFreeMemory (device->device, buffer->memory, NULL);
}

static VkCommandBuffer
begin_commands (void)
{
  VkCommandBuffer cmd;
  VkCommandBufferAllocateInfo alloc = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool = device->oneshot_command_pool,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1,
  };
  VkCommandBufferBeginInfo begin = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  g_assert_cmpint (vkAllocateCommandBuffers (device->device, &alloc, &cmd), ==, VK_SUCCESS);
  g_assert_cmpint (vkBeginCommandBuffer (cmd, &begin), ==, VK_SUCCESS);
  return cmd;
}

static void
finish_commands (VkCommandBuffer cmd)
{
  VkSubmitInfo submit = {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount = 1, .pCommandBuffers = &cmd,
  };
  g_assert_cmpint (vkEndCommandBuffer (cmd), ==, VK_SUCCESS);
  g_assert_cmpint (vkQueueSubmit (device->graphics_queue, 1, &submit, VK_NULL_HANDLE), ==, VK_SUCCESS);
  g_assert_cmpint (vkQueueWaitIdle (device->graphics_queue), ==, VK_SUCCESS);
  vkFreeCommandBuffers (device->device, device->oneshot_command_pool, 1, &cmd);
}

static void
transfer_barrier (CoglVkImage    *image,
                  VkCommandBuffer cmd,
                  gboolean        write)
{
  cogl_vk_image_cmd_barrier (image, cmd,
                             write ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL :
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                             write ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT);
}

static CoglTexture *
new_nv12_texture (void)
{
  CoglDriverVk *driver = (CoglDriverVk *) cogl_context_get_driver (test_ctx);
  g_autoptr (GError) error = NULL;
  CoglVkImage *image;
  HostBuffer buffer = host_buffer_new (24);
  VkCommandBuffer cmd = begin_commands ();
  VkBufferImageCopy copies[] = {
    { .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT, .layerCount = 1 },
      .imageExtent = { 4, 4, 1 } },
    { .bufferOffset = 16,
      .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT, .layerCount = 1 },
      .imageExtent = { 2, 2, 1 } },
  };

  memset (buffer.map, 128, 16);
  for (int i = 16; i < 24; i += 2)
    {
      ((uint8_t *) buffer.map)[i] = 96;
      ((uint8_t *) buffer.map)[i + 1] = 160;
    }
  image = cogl_vk_image_new_nv12 (device, 4, 4,
                                  cogl_vk_pipeline_cache_peek_ycbcr_conversion (driver->pipeline_cache, 0), &error);
  g_assert_no_error (error);
  g_assert_nonnull (image);
  transfer_barrier (image, cmd, TRUE);
  vkCmdCopyBufferToImage (cmd, buffer.buffer, image->image, image->layout, 2, copies);
  finish_commands (cmd);
  host_buffer_free (&buffer);
  return cogl_texture_2d_vk_new_from_image (test_ctx, image);
}

static void
check_yuv_pixel (const uint8_t *pixel,
                 CoglYuvMatrix  matrix,
                 CoglYuvRange   range)
{
  const double kr[] = { .2126, .299, .2627 };
  const double kb[] = { .0722, .114, .0593 };
  double y = range == COGL_YUV_RANGE_FULL ? 128.0 / 255 : 112.0 / 219;
  double cb = -32.0 / (range == COGL_YUV_RANGE_FULL ? 255 : 224);
  double cr = -cb;
  double r = y + 2 * (1 - kr[matrix]) * cr;
  double b = y + 2 * (1 - kb[matrix]) * cb;
  double g = (y - kr[matrix] * r - kb[matrix] * b) / (1 - kr[matrix] - kb[matrix]);
  double expected[] = { r, g, b };
  for (int c = 0; c < 3; c++)
    g_assert_cmpint (ABS ((int) pixel[c] - (int) lround (CLAMP (expected[c], 0, 1) * 255)), <=, 2);
  g_assert_cmpint (pixel[3], ==, 255);
}

static CoglFramebuffer *
new_framebuffer (void)
{
  g_autoptr (CoglTexture) texture = cogl_texture_2d_new_with_size (test_ctx, 32, 32);
  CoglFramebuffer *fb = COGL_FRAMEBUFFER (cogl_offscreen_new_with_texture (texture));
  g_autoptr (GError) error = NULL;
  g_assert_true (cogl_framebuffer_allocate (fb, &error));
  g_assert_no_error (error);
  cogl_framebuffer_orthographic (fb, 0, 0, 32, 32, -1, 1);
  return fb;
}

static void
test_nv12_colorimetry (void)
{
  device = ((CoglDriverVk *) cogl_context_get_driver (test_ctx))->vk_device;
  g_autoptr (CoglTexture) texture = new_nv12_texture ();
  g_autoptr (CoglTexture) other = new_nv12_texture ();
  g_autoptr (CoglPipeline) pipeline = cogl_pipeline_new (test_ctx);
  g_autoptr (CoglFramebuffer) fb = new_framebuffer ();
  g_autoptr (GError) error = NULL;
  uint8_t pixel[4], data[4 * 4 * 4];
  const unsigned int order[] = { 0, 1, 2, 3, 4, 5, 0, 5, 2 };
  const CoglVertexP2T2 vertices[] = {
    { 0, 0, 0, 0 }, { 32, 0, 1, 0 }, { 32, 32, 1, 1 },
    { 0, 0, 0, 0 }, { 32, 32, 1, 1 }, { 0, 32, 0, 1 },
  };
  g_autoptr (CoglPrimitive) primitive =
    cogl_primitive_new_p2t2 (test_ctx, COGL_VERTICES_MODE_TRIANGLES,
                             G_N_ELEMENTS (vertices), vertices);


  for (unsigned int j = 0; j < G_N_ELEMENTS (order); j++)
    {
      unsigned int i = order[j];
      g_assert_true (cogl_texture_2d_set_yuv_colorimetry (texture, i / 2, i % 2, &error));
      g_assert_no_error (error);
      cogl_pipeline_set_layer_texture (pipeline, 0, texture);
      cogl_framebuffer_draw_rectangle (fb, pipeline, 0, 0, 32, 32);
      cogl_framebuffer_read_pixels (fb, 8, 8, 1, 1, COGL_PIXEL_FORMAT_RGBA_8888, pixel);
      check_yuv_pixel (pixel, i / 2, i % 2);
      cogl_primitive_draw (primitive, fb, pipeline);
      cogl_framebuffer_read_pixels (fb, 8, 8, 1, 1, COGL_PIXEL_FORMAT_RGBA_8888, pixel);
      check_yuv_pixel (pixel, i / 2, i % 2);
      g_assert_cmpint (cogl_texture_get_data (texture, COGL_PIXEL_FORMAT_RGBA_8888, 16, data), >, 0);
      check_yuv_pixel (data, i / 2, i % 2);
      /* Cold shaders must use the same conversion and execute the snippet,
       * including when a second native NV12 layer uses another conversion.
       */
      for (int multilayer = 0; multilayer < 2; multilayer++)
        {
          g_autoptr (CoglPipeline) cold = cogl_pipeline_copy (pipeline);
          g_autoptr (CoglSnippet) snippet =
            cogl_snippet_new (COGL_SNIPPET_HOOK_FRAGMENT, NULL,
                              "cogl_color_out.rgb = cogl_color_out.bgr;");
          unsigned int expected = i;
          cogl_pipeline_add_snippet (cold, snippet);
          if (multilayer)
            {
              cogl_pipeline_set_layer_texture (cold, 1, other);
              g_assert_true (cogl_pipeline_set_layer_combine (
                               cold, 1, "RGBA = REPLACE (TEXTURE)", &error));
              g_assert_no_error (error);
              expected = 0;
            }
          for (int vbo = 0; vbo < 2; vbo++)
            {
              uint8_t reversed[4];
              if (vbo)
                cogl_primitive_draw (primitive, fb, cold);
              else
                cogl_framebuffer_draw_rectangle (fb, cold, 0, 0, 32, 32);
              cogl_framebuffer_read_pixels (fb, 8, 8, 1, 1,
                                            COGL_PIXEL_FORMAT_RGBA_8888, pixel);
              reversed[0] = pixel[2]; reversed[1] = pixel[1];
              reversed[2] = pixel[0]; reversed[3] = pixel[3];
              check_yuv_pixel (reversed, expected / 2, expected % 2);
            }
        }
      /* Keep a distinct interpretation alive in the same device/cache. */
      cogl_pipeline_set_layer_texture (pipeline, 0, other);
      cogl_framebuffer_draw_rectangle (fb, pipeline, 0, 0, 32, 32);
      cogl_framebuffer_read_pixels (fb, 8, 8, 1, 1, COGL_PIXEL_FORMAT_RGBA_8888, pixel);
      check_yuv_pixel (pixel, COGL_YUV_MATRIX_BT709, COGL_YUV_RANGE_NARROW);
    }
  /* Pending journal draws must retain the old interpretation. */
  g_assert_true (cogl_texture_2d_set_yuv_colorimetry (texture, 0, 0, &error));
  cogl_pipeline_set_layer_texture (pipeline, 0, texture);
  cogl_framebuffer_draw_rectangle (fb, pipeline, 0, 0, 16, 32);
  g_assert_true (cogl_texture_2d_set_yuv_colorimetry (texture, 2, 1, &error));
  cogl_framebuffer_draw_rectangle (fb, pipeline, 16, 0, 32, 32);
  cogl_framebuffer_read_pixels (fb, 8, 8, 1, 1, COGL_PIXEL_FORMAT_RGBA_8888, pixel);
  check_yuv_pixel (pixel, 0, 0);
  cogl_framebuffer_read_pixels (fb, 24, 8, 1, 1, COGL_PIXEL_FORMAT_RGBA_8888, pixel);
  check_yuv_pixel (pixel, 2, 1);
  g_assert_false (cogl_texture_2d_set_yuv_colorimetry (texture, 99, 0, &error));
  g_clear_error (&error);
  cogl_framebuffer_draw_rectangle (fb, pipeline, 0, 0, 32, 32);
  cogl_framebuffer_read_pixels (fb, 8, 8, 1, 1, COGL_PIXEL_FORMAT_RGBA_8888, pixel);
  check_yuv_pixel (pixel, 2, 1);
}

static void
clear_source (CoglVkImage       *image,
              VkImageAspectFlags aspect,
              float              depth,
              uint32_t           stencil)
{
  VkCommandBuffer cmd = begin_commands ();
  VkClearDepthStencilValue value = { depth, stencil };
  VkImageSubresourceRange range = { .aspectMask = aspect, .levelCount = 1, .layerCount = 1 };
  transfer_barrier (image, cmd, TRUE);
  vkCmdClearDepthStencilImage (cmd, image->image, image->layout, &value, 1, &range);
  finish_commands (cmd);
}

static uint8_t
read_stencil (CoglVkImage *image)
{
  HostBuffer buffer = host_buffer_new (32 * 32);
  VkCommandBuffer cmd = begin_commands ();
  VkBufferImageCopy region = {
    .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT, .layerCount = 1 },
    .imageExtent = { 32, 32, 1 },
  };
  uint8_t value;
  transfer_barrier (image, cmd, FALSE);
  vkCmdCopyImageToBuffer (cmd, image->image, image->layout, buffer.buffer, 1, &region);
  finish_commands (cmd);
  value = ((uint8_t *) buffer.map)[16 * 32 + 16];
  host_buffer_free (&buffer);
  return value;
}

static uint32_t
read_depth (CoglVkImage *image)
{
  HostBuffer buffer = host_buffer_new (32 * 32 * 4);
  VkCommandBuffer cmd = begin_commands ();
  VkBufferImageCopy region = {
    .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .layerCount = 1 },
    .imageExtent = { 32, 32, 1 },
  };
  uint32_t value;
  transfer_barrier (image, cmd, FALSE);
  vkCmdCopyImageToBuffer (cmd, image->image, image->layout, buffer.buffer, 1, &region);
  finish_commands (cmd);
  value = ((uint32_t *) buffer.map)[16 * 32 + 16];
  host_buffer_free (&buffer);
  return value;
}

static void
test_separate_depth_stencil (gconstpointer data)
{
  gboolean msaa = GPOINTER_TO_INT (data);
  CoglDriverVk *driver = (CoglDriverVk *) cogl_context_get_driver (test_ctx);
  unsigned int old_flags = driver->last_offscreen_allocate_flags;
  gboolean old_have_flags = driver->have_last_offscreen_allocate_flags;
  CoglFramebuffer *a, *b;
  CoglVkImage *depth, *stencil;
  VkImageView depth_view, stencil_view;
  CoglRenderbuffers *rb;
  CoglDepthState state;
  CoglColor color;
  g_autoptr (CoglPipeline) pipeline = cogl_pipeline_new (test_ctx);

  device = driver->vk_device;
  if (msaa)
    g_setenv ("COGL_VK_MSAA", "4", TRUE);
  driver->have_last_offscreen_allocate_flags = TRUE;
  driver->last_offscreen_allocate_flags = COGL_OFFSCREEN_ALLOCATE_FLAG_DEPTH |
                                          COGL_OFFSCREEN_ALLOCATE_FLAG_STENCIL;
  a = new_framebuffer ();
  rb = cogl_framebuffer_peek_renderbuffers (a);
  cogl_framebuffer_vk_peek_depth_stencil_images ((CoglFramebufferVk *) cogl_framebuffer_get_driver (a),
                                                 &depth, &stencil,
                                                 &depth_view, &stencil_view);
  g_assert_nonnull (depth);
  g_assert_nonnull (stencil);
  g_assert_true (depth_view != VK_NULL_HANDLE);
  g_assert_true (stencil_view != VK_NULL_HANDLE);
  g_assert_true (depth_view != stencil_view);
  g_assert_cmpint (depth->samples, ==, msaa ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT);
  cogl_framebuffer_finish (a);
  clear_source (depth, VK_IMAGE_ASPECT_DEPTH_BIT, .25, 0);
  clear_source (stencil, VK_IMAGE_ASPECT_STENCIL_BIT, 1, 7);
  uint32_t initial_depth = msaa ? 0 : read_depth (depth);
  cogl_depth_state_init (&state);
  cogl_depth_state_set_test_enabled (&state, TRUE);
  cogl_depth_state_set_test_function (&state, COGL_DEPTH_TEST_FUNCTION_LESS);
  cogl_depth_state_set_write_enabled (&state, FALSE);
  g_assert_true (cogl_pipeline_set_depth_state (pipeline, &state, NULL));
  cogl_color_init_from_4f (&color, 1, 0, 0, 1);
  cogl_pipeline_set_color (pipeline, &color);
  cogl_framebuffer_clear4f (a, COGL_BUFFER_BIT_COLOR, 0, 0, 0, 1);
  cogl_framebuffer_draw_rectangle (a, pipeline, 0, 0, 32, 32);
  cogl_framebuffer_finish (a);
  if (!msaa)
    g_assert_cmphex (read_depth (depth), ==, initial_depth);
  test_utils_check_pixel (a, 16, 16, 0x000000ff);
  cogl_framebuffer_finish (a);
  if (!msaa)
    g_assert_cmpint (read_stencil (stencil), ==, 7);
  cogl_framebuffer_clear4f (a, COGL_BUFFER_BIT_DEPTH, 0, 0, 0, 0);
  cogl_framebuffer_draw_rectangle (a, pipeline, 0, 0, 32, 32);
  test_utils_check_pixel (a, 16, 16, 0xff0000ff);
  cogl_framebuffer_finish (a);
  if (!msaa)
    g_assert_cmpint (read_stencil (stencil), ==, 7);

  /* Bind the independent storage to a framebuffer with its own previous DS. */
  driver->last_offscreen_allocate_flags = COGL_OFFSCREEN_ALLOCATE_FLAG_DEPTH_STENCIL;
  b = new_framebuffer ();
  cogl_framebuffer_bind_renderbuffers (b, rb);
  cogl_framebuffer_clear4f (b, COGL_BUFFER_BIT_COLOR | COGL_BUFFER_BIT_STENCIL, 0, 0, 0, 1);
  cogl_framebuffer_draw_rectangle (b, pipeline, 0, 0, 32, 32);
  test_utils_check_pixel (b, 16, 16, 0xff0000ff);
  cogl_framebuffer_finish (b);
  if (!msaa)
    g_assert_cmpint (read_stencil (stencil), ==, 0);
  /* A later allocation's preferred flags must not replace A's shared set. */
  cogl_framebuffer_draw_rectangle (a, pipeline, 0, 0, 32, 32);
  test_utils_check_pixel (a, 16, 16, 0xff0000ff);
  g_assert_true (cogl_framebuffer_peek_renderbuffers (a) == rb);
  g_object_unref (a);
  cogl_color_init_from_4f (&color, 0, 1, 0, 1);
  cogl_pipeline_set_color (pipeline, &color);
  cogl_framebuffer_draw_rectangle (b, pipeline, 0, 0, 32, 32);
  test_utils_check_pixel (b, 16, 16, 0x00ff00ff);
  g_object_unref (b);
  driver->last_offscreen_allocate_flags = old_flags;
  driver->have_last_offscreen_allocate_flags = old_have_flags;
  if (msaa)
    g_unsetenv ("COGL_VK_MSAA");
}

static void
check_cached_pipeline (CoglFramebuffer *fb,
                       CoglPipeline    *pipeline,
                       CoglPrimitive   *primitive,
                       uint32_t         color)
{
  /* Exercise both caches twice so mutation checks follow cache hits. */
  for (int i = 0; i < 2; i++)
    {
      cogl_framebuffer_draw_rectangle (fb, pipeline, 0, 0, 32, 32);
      test_utils_check_pixel (fb, 16, 16, color);
      cogl_primitive_draw (primitive, fb, pipeline);
      test_utils_check_pixel (fb, 16, 16, color);
    }
}

static void
test_shader_cache_mutation (void)
{
  const uint8_t red[] = { 255, 0, 0, 255 };
  const CoglVertexP2T2 vertices[] = {
    { 0, 0, 0, 0 }, { 32, 0, 1, 0 }, { 32, 32, 1, 1 },
    { 0, 0, 0, 0 }, { 32, 32, 1, 1 }, { 0, 32, 0, 1 },
  };
  g_autoptr (GError) error = NULL;
  g_autoptr (CoglFramebuffer) fb = new_framebuffer ();
  g_autoptr (CoglTexture) texture = cogl_texture_2d_new_from_data (
    test_ctx, 1, 1, COGL_PIXEL_FORMAT_RGBA_8888, 4, red, &error);
  g_autoptr (CoglPipeline) pipeline = cogl_pipeline_new (test_ctx);
  g_autoptr (CoglPipeline) child = NULL;
  g_autoptr (CoglSnippet) identity = cogl_snippet_new (
    COGL_SNIPPET_HOOK_FRAGMENT, NULL, "cogl_color_out += vec4(0.0);");
  g_autoptr (CoglSnippet) swap = cogl_snippet_new (
    COGL_SNIPPET_HOOK_FRAGMENT, NULL, "cogl_color_out.rgb = cogl_color_out.bgr;");
  g_autoptr (CoglPrimitive) primitive = cogl_primitive_new_p2t2 (
    test_ctx, COGL_VERTICES_MODE_TRIANGLES, G_N_ELEMENTS (vertices), vertices);
  CoglColor green;

  g_assert_no_error (error);
  cogl_pipeline_set_layer_texture (pipeline, 0, texture);
  cogl_pipeline_add_snippet (pipeline, identity);
  check_cached_pipeline (fb, pipeline, primitive, 0xff0000ff);
  child = cogl_pipeline_copy (pipeline);
  check_cached_pipeline (fb, child, primitive, 0xff0000ff);
  cogl_color_init_from_4f (&green, 128.f / 255.f, 1, 1, 1);
  cogl_pipeline_set_color (child, &green);
  check_cached_pipeline (fb, child, primitive, 0x800000ff);
  cogl_color_init_from_4f (&green, 1, 1, 1, 1);
  cogl_pipeline_set_color (child, &green);
  check_cached_pipeline (fb, child, primitive, 0xff0000ff);

  cogl_color_init_from_4f (&green, 0, 1, 0, 1);
  cogl_pipeline_set_layer_combine_constant (pipeline, 0, &green);
  g_assert_true (cogl_pipeline_set_layer_combine (
                   pipeline, 0, "RGBA = REPLACE (CONSTANT)", &error));
  g_assert_no_error (error);
  check_cached_pipeline (fb, pipeline, primitive, 0x00ff00ff);
  check_cached_pipeline (fb, child, primitive, 0xff0000ff);

  g_assert_true (cogl_pipeline_set_layer_combine (
                   pipeline, 0, "RGBA = REPLACE (TEXTURE)", &error));
  g_assert_no_error (error);
  check_cached_pipeline (fb, pipeline, primitive, 0xff0000ff);
  cogl_pipeline_add_snippet (pipeline, swap);
  check_cached_pipeline (fb, pipeline, primitive, 0x0000ffff);
  check_cached_pipeline (fb, child, primitive, 0xff0000ff);
}

COGL_TEST_SUITE (
  g_test_add_func ("/vk-low-level/shader-cache-mutation", test_shader_cache_mutation);
  g_test_add_func ("/vk-low-level/nv12-colorimetry", test_nv12_colorimetry);
  g_test_add_data_func ("/vk-low-level/separate-depth-stencil", GINT_TO_POINTER (0), test_separate_depth_stencil);
  g_test_add_data_func ("/vk-low-level/separate-depth-stencil-msaa", GINT_TO_POINTER (1), test_separate_depth_stencil); )
