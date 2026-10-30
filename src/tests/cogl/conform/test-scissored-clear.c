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

#include <cogl/cogl.h>

#include "tests/cogl-test-utils.h"

static void
test_scissored_clear (void)
{
  g_autoptr (CoglTexture) texture = cogl_texture_2d_new_with_size (test_ctx, 32, 24);
  g_autoptr (CoglOffscreen) offscreen = cogl_offscreen_new_with_texture (texture);
  CoglFramebuffer *fb = COGL_FRAMEBUFFER (offscreen);
  MtkRectangle clip_rect = { 5, 7, 9, 6 };
  g_autoptr (MtkRegion) clip = mtk_region_create_rectangle (&clip_rect);
  uint8_t pixels[32 * 24 * 4];

  g_assert_true (cogl_framebuffer_allocate (fb, NULL));
  cogl_framebuffer_clear4f (fb, COGL_BUFFER_BIT_COLOR, 1, 0, 0, 1);
  cogl_framebuffer_push_region_clip (fb, clip);
  cogl_framebuffer_clear4f (fb, COGL_BUFFER_BIT_COLOR, 0, 1, 0, 1);
  cogl_framebuffer_pop_clip (fb);
  g_assert_true (cogl_framebuffer_read_pixels (fb, 0, 0, 32, 24,
                                               COGL_PIXEL_FORMAT_RGBA_8888_PRE,
                                               pixels));
  for (int y = 0; y < 24; y++)
    for (int x = 0; x < 32; x++)
      {
        gboolean inside = x >= 5 && x < 14 && y >= 7 && y < 13;
        uint8_t *pixel = pixels + (y * 32 + x) * 4;

        g_assert_cmpint (pixel[0], ==, inside ? 0 : 255);
        g_assert_cmpint (pixel[1], ==, inside ? 255 : 0);
        g_assert_cmpint (pixel[2], ==, 0);
        g_assert_cmpint (pixel[3], ==, 255);
      }
}

COGL_TEST_SUITE (
  g_test_add_func ("/scissored-clear", test_scissored_clear); )
