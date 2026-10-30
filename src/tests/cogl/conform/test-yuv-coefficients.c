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
#include <math.h>
#include <string.h>
#include "meta/meta-multi-texture.h"
#include "compositor/meta-multi-texture-format-private.h"
#include "tests/cogl-test-utils.h"
#include "cogl/cogl-private.h"
#include "cogl/cogl-context-private.h"
#include "cogl/cogl-pipeline-cache-private.h"

static uint8_t
to_byte (double value)
{
  return (uint8_t) CLAMP (lround (value * 255.0), 0, 255);
}

static void
check_conversion (MetaMultiTexture            *texture,
                  MetaMultiTextureCoefficients coeffs,
                  const uint8_t                expected[3])
{
  g_autoptr (CoglPipeline) pipeline = cogl_pipeline_new (test_ctx);
  CoglPipelineCache *cache = cogl_context_get_pipeline_cache (test_ctx);
  CoglPipelineHashTable *fragment_hash =
    cogl_pipeline_cache_get_fragment_hash (cache);
  CoglPipelineHashTable *combined_hash =
    cogl_pipeline_cache_get_combined_hash (cache);

  /* This format matrix intentionally exceeds the normal shader-count
   * warning threshold. Expect those diagnostics without hiding others.
   */
  if (fragment_hash->n_unique_pipelines == 50)
    g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                           "Over 50 separate fragment shaders*");
  if (combined_hash->n_unique_pipelines == 50)
    g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                           "Over 50 separate programs*");

  for (int plane = 0; plane < meta_multi_texture_get_n_planes (texture); plane++)
    {
      cogl_pipeline_set_layer_texture (pipeline, plane,
                                       meta_multi_texture_get_plane (texture, plane));
      cogl_pipeline_set_layer_filters (pipeline, plane,
                                       COGL_PIPELINE_FILTER_NEAREST,
                                       COGL_PIPELINE_FILTER_NEAREST);
    }
  meta_multi_texture_add_pipeline_sampling (
    texture, coeffs, META_MULTI_TEXTURE_ALPHA_MODE_PREMULT_ELECTRICAL, pipeline);
  cogl_framebuffer_draw_rectangle (test_fb, pipeline, -1, -1, 1, 1);
  test_utils_check_pixel_rgb (test_fb, 8, 8,
                              expected[0], expected[1], expected[2]);
  g_test_assert_expected_messages ();
}

static void
test_nv12_coefficients (void)
{
  static const struct {
    double kr, kb;
    MetaMultiTextureCoefficients full, limited;
  } standards[] = {
    { .299, .114, META_MULTI_TEXTURE_COEFFICIENTS_BT601_FULL,
      META_MULTI_TEXTURE_COEFFICIENTS_BT601_LIMITED },
    { .2126, .0722, META_MULTI_TEXTURE_COEFFICIENTS_BT709_FULL,
      META_MULTI_TEXTURE_COEFFICIENTS_BT709_LIMITED },
    { .2627, .0593, META_MULTI_TEXTURE_COEFFICIENTS_BT2020_FULL,
      META_MULTI_TEXTURE_COEFFICIENTS_BT2020_LIMITED },
  };
  static const uint8_t samples[][3] = {
    { 16, 128, 128 }, { 235, 128, 128 }, { 128, 96, 160 },
    { 90, 170, 110 }, { 180, 65, 185 }, { 0, 0, 255 }, { 255, 255, 0 },
  };

  for (unsigned sample = 0; sample < G_N_ELEMENTS (samples); sample++)
    {
      uint8_t y[4], uv[2] = { samples[sample][1], samples[sample][2] };
      CoglTexture **planes = g_new (CoglTexture *, 2);
      g_autoptr (MetaMultiTexture) texture = NULL;
      g_autoptr (GError) error = NULL;

      memset (y, samples[sample][0], sizeof (y));
      planes[0] = cogl_texture_2d_new_from_data (test_ctx, 2, 2,
                                                 COGL_PIXEL_FORMAT_R_8, 2, y, &error);
      g_assert_no_error (error);
      planes[1] = cogl_texture_2d_new_from_data (test_ctx, 1, 1,
                                                 COGL_PIXEL_FORMAT_RG_88, 2, uv, &error);
      g_assert_no_error (error);
      texture = meta_multi_texture_new (META_MULTI_TEXTURE_FORMAT_NV12, planes, 2);

      /* Reuse the same planes across metadata changes, then revisit the first
       * standard. Conversion must belong to the pipeline, not the image.
       */
      for (unsigned round = 0; round < 4; round++)
        for (int limited = 0; limited < 2; limited++)
          {
            unsigned standard = round % G_N_ELEMENTS (standards);
            double kr = standards[standard].kr, kb = standards[standard].kb;
            double kg = 1 - kr - kb;
            double yy = (samples[sample][0] - (limited ? 16.0 : 0.0)) /
                        (limited ? 219.0 : 255.0);
            double cb = (uv[0] - 128.0) / (limited ? 224.0 : 255.0);
            double cr = (uv[1] - 128.0) / (limited ? 224.0 : 255.0);
            uint8_t expected[] = {
              to_byte (yy + 2 * (1 - kr) * cr),
              to_byte (yy - 2 * kb * (1 - kb) / kg * cb -
                       2 * kr * (1 - kr) / kg * cr),
              to_byte (yy + 2 * (1 - kb) * cb),
            };

            g_test_message ("sample %u, standard %u, limited %d", sample, standard, limited);
            check_conversion (texture, limited ? standards[standard].limited :
                              standards[standard].full, expected);
          }
    }
}

static void
test_yuv_layout (gconstpointer format_data)
{
  int format = GPOINTER_TO_INT (format_data);

  const MetaMultiTextureFormatInfo *info = meta_multi_texture_format_get_info (format);
  const char *name = meta_multi_texture_format_to_string (format);
  CoglTexture **planes = g_new0 (CoglTexture *, info->n_planes);
  g_autoptr (MetaMultiTexture) texture = NULL;
  g_autoptr (GError) error = NULL;
  double values[] = { 128.0 / 255, 96.0 / 255, 160.0 / 255 };
  uint8_t bytes[3][128] = { 0 };
  uint16_t words[3][64] = { 0 };
  gboolean wide = info->subformats[0] == COGL_PIXEL_FORMAT_R_16;
  gboolean packed = format <= META_MULTI_TEXTURE_FORMAT_VYUY;
  gboolean reverse = g_str_has_prefix (name, "YVU") ||
                     format == META_MULTI_TEXTURE_FORMAT_NV21 ||
                     format == META_MULTI_TEXTURE_FORMAT_NV61 ||
                     format == META_MULTI_TEXTURE_FORMAT_NV42;
  int bits = 16;
  int shift = 0;
  uint16_t sample[] = { 32768, 24576, 40960 };

  if (wide)
    {
      if (g_str_has_suffix (name, "10"))
        bits = 10;
      else if (g_str_has_suffix (name, "12"))
        bits = 12;
      shift = name[0] == 'P' ? 16 - bits : 0;
      for (int c = 0; c < 3; c++)
        {
          sample[c] = ((uint32_t) (c == 0 ? 128 : c == 1 ? 96 : 160) << (bits - 8)) << shift;
          values[c] = (double) sample[c] / (name[0] == 'P' ? 65535 : ((1u << bits) - 1));
        }
    }
  for (int i = 0; i < 32; i++)
    {
      if (packed)
        {
          const uint8_t patterns[4][4] = {
            {128, 96, 128, 160}, {128, 160, 128, 96},
            {96, 128, 160, 128}, {160, 128, 96, 128},
          };
          memcpy (&bytes[0][i * 4], patterns[format - META_MULTI_TEXTURE_FORMAT_YUYV], 4);
        }
      else
        {
          bytes[0][i] = 128;
          words[0][i] = sample[0];
          if (info->n_planes == 2)
            {
              bytes[1][i * 2] = reverse ? 160 : 96;
              bytes[1][i * 2 + 1] = reverse ? 96 : 160;
              words[1][i * 2] = sample[1];
              words[1][i * 2 + 1] = sample[2];
            }
          else
            {
              bytes[1][i] = reverse ? 160 : 96;
              bytes[2][i] = reverse ? 96 : 160;
              words[1][i] = sample[1];
              words[2][i] = sample[2];
            }
        }
    }
  for (int plane = 0; plane < info->n_planes; plane++)
    {
      int width = 4 / info->hsub[plane], height = 4 / info->vsub[plane];
      int stride = packed ? 8 : width * (wide ? 2 : 1) *
                   (plane == 1 && info->n_planes == 2 ? 2 : 1);
      const uint8_t *data = wide ? (uint8_t *) words[info->plane_indices[plane]] :
                            bytes[info->plane_indices[plane]];
      planes[plane] = cogl_texture_2d_new_from_data (test_ctx, width, height,
                                                     info->subformats[plane],
                                                     stride, data, &error);
      g_assert_no_error (error);
    }
  texture = meta_multi_texture_new (format, planes, info->n_planes);
  for (int limited = 0; limited < 2; limited++)
    {
      double y = (values[0] * 255 - (limited ? 16 : 0)) / (limited ? 219 : 255);
      double u = (values[1] * 255 - 128) / (limited ? 224 : 255);
      double v = (values[2] * 255 - 128) / (limited ? 224 : 255);
      uint8_t expected[] = {
        to_byte (y + 1.5748 * v),
        to_byte (y - 2 * .0722 * .9278 / .7152 * u - 2 * .2126 * .7874 / .7152 * v),
        to_byte (y + 1.8556 * u),
      };
      g_test_message ("%s limited=%d", name, limited);
      check_conversion (texture, limited ? META_MULTI_TEXTURE_COEFFICIENTS_BT709_LIMITED :
                        META_MULTI_TEXTURE_COEFFICIENTS_BT709_FULL, expected);
    }
}

COGL_TEST_SUITE (
  g_test_add_func ("/yuv/coefficients/nv12", test_nv12_coefficients);
  for (int format = META_MULTI_TEXTURE_FORMAT_YUYV; format < N_META_MULTI_TEXTURE_FORMATS; format++)
{
  g_autofree char *path = g_strdup_printf ("/yuv/%s/%s", format < META_MULTI_TEXTURE_FORMAT_NV12 ? "packed" : format < META_MULTI_TEXTURE_FORMAT_YUV420 ? "semiplanar" : "planar", meta_multi_texture_format_to_string (format));
  g_test_add_data_func (path, GINT_TO_POINTER (format), test_yuv_layout);
})
