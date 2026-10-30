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
#include <string.h>
#include "tests/cogl-test-utils.h"

static CoglTexture *
make_texture (void)
{
  static const uint8_t pixels[] = { 255, 0, 0, 255, 0, 255, 0, 255 };
  g_autoptr (GError) error = NULL;
  CoglTexture *texture = cogl_texture_2d_new_from_data (
    test_ctx, 2, 1, COGL_PIXEL_FORMAT_RGBA_8888_PRE, 8, pixels, &error);
  g_assert_no_error (error);
  return texture;
}

static void
test_sparse_texcoords (void)
{
  static const int indices[] = { 8, 17, 1000 };
  static const float vertices[][6] = {
    { -1, -1, .25, .5, .75, .5 }, { 1, -1, .25, .5, .75, .5 },
    { -1,  1, .25, .5, .75, .5 }, { 1,  1, .25, .5, .75, .5 },
  };
  g_autoptr (CoglTexture) texture = make_texture ();
  g_autoptr (CoglAttributeBuffer) buffer =
    cogl_attribute_buffer_new (test_ctx, sizeof (vertices), vertices);

  for (unsigned i = 0; i < G_N_ELEMENTS (indices); i++)
    {
      g_autoptr (CoglPipeline) pipeline = cogl_pipeline_new (test_ctx);
      g_autoptr (CoglPrimitive) primitive = NULL;
      g_autofree char *name = g_strdup_printf ("cogl_tex_coord%d_in", indices[i]);
      g_autoptr (CoglAttribute) position = cogl_attribute_new (
        buffer, "cogl_position_in", sizeof (vertices[0]), 0, 2, COGL_ATTRIBUTE_TYPE_FLOAT);
      g_autoptr (CoglAttribute) uv0 = cogl_attribute_new (
        buffer, "cogl_tex_coord0_in", sizeof (vertices[0]), 2 * sizeof (float),
        2, COGL_ATTRIBUTE_TYPE_FLOAT);
      g_autoptr (CoglAttribute) uv = cogl_attribute_new (
        buffer, name, sizeof (vertices[0]), 4 * sizeof (float), 2, COGL_ATTRIBUTE_TYPE_FLOAT);
      CoglAttribute *attributes[] = { position, uv0, uv };

      cogl_pipeline_set_layer_texture (pipeline, indices[i], texture);
      cogl_pipeline_set_layer_filters (pipeline, indices[i],
                                       COGL_PIPELINE_FILTER_NEAREST,
                                       COGL_PIPELINE_FILTER_NEAREST);
      primitive = cogl_primitive_new_with_attributes (
        COGL_VERTICES_MODE_TRIANGLE_STRIP, 4, attributes, G_N_ELEMENTS (attributes));
      cogl_primitive_draw (primitive, test_fb, pipeline);
      test_utils_check_pixel (test_fb, 8, 8, 0x00ff00ff);

      /* Expanded integer coordinates must use float strides, also at sparse
       * locations. Different U/V values expose an accidental half stride.
       */
      {
        static const uint16_t integer_uv[][2] = {
          { 52428, 3277 }, { 52428, 3277 }, { 52428, 3277 }, { 52428, 3277 },
        };
        g_autoptr (CoglAttributeBuffer) integer_buffer =
          cogl_attribute_buffer_new (test_ctx, sizeof (integer_uv), integer_uv);
        g_autoptr (CoglAttribute) integer_attr = cogl_attribute_new (
          integer_buffer, name, sizeof (integer_uv[0]), 0, 2,
          COGL_ATTRIBUTE_TYPE_UNSIGNED_SHORT);
        g_autoptr (CoglPrimitive) integer_primitive = NULL;

        cogl_attribute_set_normalized (integer_attr, TRUE);
        attributes[2] = integer_attr;
        integer_primitive = cogl_primitive_new_with_attributes (
          COGL_VERTICES_MODE_TRIANGLE_STRIP, 4, attributes, G_N_ELEMENTS (attributes));
        cogl_primitive_draw (integer_primitive, test_fb, pipeline);
        test_utils_check_pixel (test_fb, cogl_framebuffer_get_width (test_fb) - 8,
                                8, 0x00ff00ff);
      }
    }
}

static void
test_many_custom_attributes (void)
{
  enum { N_CUSTOM = 12 };
  static const float positions[][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
  g_autoptr (CoglPipeline) pipeline = cogl_pipeline_new (test_ctx);
  g_autoptr (CoglAttributeBuffer) position_buffer =
    cogl_attribute_buffer_new (test_ctx, sizeof (positions), positions);
  g_autoptr (GString) declarations = g_string_new (NULL);
  g_autoptr (GString) code = g_string_new ("float total = 0.0;\n");
  g_autoptr (CoglSnippet) snippet = NULL;

  for (int i = 0; i < N_CUSTOM; i++)
    {
      g_string_append_printf (declarations, "attribute float component%d;\n", i);
      g_string_append_printf (code, "total += component%d;\n", i);
    }
  g_string_append (code, "cogl_color_out = vec4(total / 78.0, component11 / 24.0, 0.0, 1.0);\n");
  snippet = cogl_snippet_new (COGL_SNIPPET_HOOK_VERTEX, declarations->str, code->str);
  cogl_pipeline_add_snippet (pipeline, snippet);

  /* Reuse one pipeline with different VI types/layouts. The last custom
   * attribute changes type and stride and participates in color.
   */
  for (int round = 0; round < 3; round++)
    {
      g_autoptr (GPtrArray) attributes = g_ptr_array_new_with_free_func (g_object_unref);
      g_autoptr (CoglPrimitive) primitive = NULL;
      float values[4][N_CUSTOM];
      int16_t short_values[] = { 6, 6, 6, 6 };
      g_autoptr (CoglAttributeBuffer) buffer = NULL;
      g_autoptr (CoglAttributeBuffer) short_buffer = NULL;

      for (int v = 0; v < 4; v++)
        for (int i = 0; i < N_CUSTOM; i++)
          values[v][i] = i + 1;
      buffer = cogl_attribute_buffer_new (test_ctx, sizeof (values), values);
      short_buffer = cogl_attribute_buffer_new (test_ctx, sizeof (short_values), short_values);
      g_ptr_array_add (attributes, cogl_attribute_new (
                         position_buffer, "cogl_position_in", sizeof (positions[0]), 0, 2,
                         COGL_ATTRIBUTE_TYPE_FLOAT));
      for (int i = 0; i < N_CUSTOM; i++)
        {
          g_autofree char *name = g_strdup_printf ("component%d", i);
          CoglAttribute *attribute;
          if (i == N_CUSTOM - 1 && round == 1)
            attribute = cogl_attribute_new (short_buffer, name, sizeof (int16_t), 0, 1,
                                            COGL_ATTRIBUTE_TYPE_SHORT);
          else
            attribute = cogl_attribute_new (buffer, name, sizeof (values[0]),
                                            i * sizeof (float), 1, COGL_ATTRIBUTE_TYPE_FLOAT);
          g_ptr_array_add (attributes, attribute);
        }
      primitive = cogl_primitive_new_with_attributes (
        COGL_VERTICES_MODE_TRIANGLE_STRIP, 4,
        (CoglAttribute **) attributes->pdata, attributes->len);
      cogl_primitive_draw (primitive, test_fb, pipeline);
      if (round == 1)
        test_utils_check_pixel_rgb (test_fb, 8, 8, 235, 64, 0);
      else
        test_utils_check_pixel_rgb (test_fb, 8, 8, 255, 128, 0);
    }
}

static void
test_many_layers (void)
{
  static const uint8_t white[] = { 255, 255, 255, 255 };
  g_autoptr (CoglTexture) texture = make_texture ();
  g_autoptr (CoglTexture) neutral = cogl_texture_2d_new_from_data (
    test_ctx, 1, 1, COGL_PIXEL_FORMAT_RGBA_8888_PRE, 4, white, NULL);
  g_autoptr (CoglPipeline) pipeline = cogl_pipeline_new (test_ctx);
  int n_layers =
    cogl_renderer_get_driver_id (cogl_context_get_renderer (test_ctx)) ==
    COGL_DRIVER_ID_VULKAN ? 17 : 12;
  float texcoords[17 * 4];

  /* Mesa GL exposes 16 vertex inputs; lavapipe exposes 32. Exercise the
   * common multi-layer path and additionally cross Vulkan's former cap.
   */
  g_test_message ("Testing %d simultaneously sampled layers", n_layers);
  for (int i = 0; i < n_layers; i++)
    {
      cogl_pipeline_set_layer_texture (pipeline, i, i == n_layers - 1 ? texture : neutral);
      cogl_pipeline_set_layer_filters (pipeline, i,
                                       COGL_PIPELINE_FILTER_NEAREST,
                                       COGL_PIPELINE_FILTER_NEAREST);
      texcoords[4 * i] = texcoords[4 * i + 2] = .75f;
      texcoords[4 * i + 1] = texcoords[4 * i + 3] = .5f;
    }
  cogl_framebuffer_draw_multitextured_rectangle (test_fb, pipeline,
                                                 -1, -1, 1, 1, texcoords,
                                                 n_layers * 4);
  test_utils_check_pixel (test_fb, 8, 8, 0x00ff00ff);
}

COGL_TEST_SUITE (
  g_test_add_func ("/attributes/sparse-texcoords", test_sparse_texcoords);
  g_test_add_func ("/attributes/many-custom", test_many_custom_attributes);
  g_test_add_func ("/attributes/many-layers", test_many_layers); )
