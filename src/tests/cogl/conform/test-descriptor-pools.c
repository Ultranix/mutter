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
#include <cogl/cogl.h>

#ifdef HAVE_VULKAN
#include "cogl/cogl-context-private.h"
#include "cogl/driver/vk/cogl-driver-vk-private.h"
#include "cogl/driver/vk/cogl-pipeline-vk-private.h"
#endif

#include "tests/cogl-test-utils.h"

static void
test_descriptor_pools (void)
{
  const uint8_t white[] = { 255, 255, 255, 255 };
  g_autoptr (GPtrArray) retained = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr (CoglPipeline) cold = cogl_pipeline_new (test_ctx);
  g_autoptr (CoglSnippet) snippet =
    cogl_snippet_new (COGL_SNIPPET_HOOK_FRAGMENT, "uniform float red;", NULL);
  int red;

  cogl_snippet_set_replace (snippet, "cogl_color_out = vec4(red, 1.0-red, 0.0, 1.0);");
  cogl_pipeline_add_snippet (cold, snippet);
  red = cogl_pipeline_get_uniform_location (cold, "red");
  cogl_framebuffer_orthographic (test_fb, 0, 0,
                                 cogl_framebuffer_get_width (test_fb),
                                 cogl_framebuffer_get_height (test_fb), -1, 1);

  for (int frame = 0; frame < 40; frame++)
    {
      g_autoptr (CoglPipeline) hot = cogl_pipeline_new (test_ctx);
      CoglTexture *texture = cogl_texture_2d_new_from_data (test_ctx, 1, 1,
                                                            COGL_PIXEL_FORMAT_RGBA_8888_PRE,
                                                            4, white, NULL);

      g_assert_nonnull (texture);
      g_ptr_array_add (retained, texture);
      cogl_pipeline_set_layer_texture (hot, 0, texture);
      /* A newly cached texture must not pin this frame's temporary sets. */
      cogl_framebuffer_draw_rectangle (test_fb, hot, 0, 0, 32, 32);
      for (int draw = 0; draw < 800; draw++)
        {
          gboolean is_red = (draw % 2) == 0;

          cogl_pipeline_set_uniform_1f (cold, red, is_red ? 1.0f : 0.0f);
          cogl_framebuffer_draw_rectangle (test_fb, cold,
                                           is_red ? 0 : 16, 0,
                                           is_red ? 16 : 32, 32);
        }
      /* Readback also retires the submitted frame. The two colors detect
       * unsafe reuse of a descriptor before its earlier draws complete.
       */
      test_utils_check_pixel (test_fb, 8, 8, 0xff0000ff);
      test_utils_check_pixel (test_fb, 24, 8, 0x00ff00ff);
#ifdef HAVE_VULKAN
      CoglDriver *driver = cogl_context_get_driver (test_ctx);
      if (g_strcmp0 (g_getenv ("COGL_DRIVER"), "vulkan") == 0)
        {
          CoglVkPipelineCache *cache = ((CoglDriverVk *) driver)->pipeline_cache;

          /* One texture pool and at most two alternating transient pools. */
          g_assert_cmpuint (cache->ds_pool_slots->len, <=, 3);
        }
#endif
    }
}

COGL_TEST_SUITE (
  g_test_add_func ("/descriptor-pools", test_descriptor_pools); )
