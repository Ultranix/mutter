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
#include <math.h>
#include <stdio.h>

#include "tests/cogl-test-utils.h"

/* Check that three-component positions retain their depth during perspective
 * projection. Dropping z from the vertex format makes clip.w zero and clips
 * the primitive, which orthographic tests do not detect.
 *
 * A vertex snippet also exercises the generated shader and framebuffer flip
 * vector. Each case clears blue and replaces the color without blending;
 * red or green pixels confirm that the projected geometry was drawn.
 */

#define CLEAR_RGBA 0x0000ffffu
#define RED_RGBA   0xff0000ffu
#define GREEN_RGBA 0x00ff00ffu

static float
stage_z_2d (float z_near)
{
  /* clutter_stage.c calculate_z_translation(), fovy 60°. */
  const float A = 0.57735025882720947265625f;
  const float B = 0.866025388240814208984375f;
  const float C = 0.86162912845611572265625f;
  const float D = 0.00872653536498546600341796875f;

  return z_near * A * B * C / D + z_near;
}

static void
reset_matrices (void)
{
  graphene_matrix_t identity;

  graphene_matrix_init_identity (&identity);
  cogl_framebuffer_set_modelview_matrix (test_fb, &identity);
}

static void
set_stage_perspective (void)
{
  float z_near = 1.f;
  float z = stage_z_2d (z_near);
  float aspect;
  int w, h;

  w = cogl_framebuffer_get_width (test_fb);
  h = cogl_framebuffer_get_height (test_fb);
  aspect = h > 0 ? (float) w / (float) h : 1.f;
  reset_matrices ();
  cogl_framebuffer_perspective (test_fb, 60.f, aspect, z_near, z * 4.f);
}

static void
frustum_quad (float  z,
              float  scale,
              float *hx,
              float *hy)
{
  int w = cogl_framebuffer_get_width (test_fb);
  int h = cogl_framebuffer_get_height (test_fb);
  float aspect = h > 0 ? (float) w / (float) h : 1.f;
  float half_h = fabsf (z) * tanf ((float) G_PI / 6.f);

  *hy = half_h * scale;
  *hx = half_h * aspect * scale;
}

static void
clear_blue (void)
{
  cogl_framebuffer_clear4f (test_fb, COGL_BUFFER_BIT_COLOR, 0.f, 0.f, 1.f, 1.f);
}

static void
replace_blend (CoglPipeline *pipeline)
{
  GError *error = NULL;

  /* Fragments replace the clear, so a black write is not mistaken for
   * "the draw never happened".
   */
  if (!cogl_pipeline_set_blend (pipeline, "RGBA = ADD (SRC_COLOR, 0)", &error))
    {
      g_printerr ("PROBE blend failed: %s\n", error ? error->message : "?");
      g_clear_error (&error);
    }
}

static void
add_snippet (CoglPipeline   *pipeline,
             CoglSnippetHook hook,
             const char     *note)
{
  CoglSnippet *snippet = cogl_snippet_new (hook, NULL, note);

  cogl_pipeline_add_snippet (pipeline, snippet);
  g_object_unref (snippet);
}

static void
expect_center_and_corner (const char *tag,
                          uint32_t    center_rgba)
{
  int w = cogl_framebuffer_get_width (test_fb);
  int h = cogl_framebuffer_get_height (test_fb);
  uint8_t corner[4];
  uint8_t center[4];
  char got_corner[16];
  char got_center[16];
  char want_corner[16];
  char want_center[16];

  cogl_framebuffer_read_pixels (test_fb, 1, 1, 1, 1,
                                COGL_PIXEL_FORMAT_RGBA_8888_PRE, corner);
  cogl_framebuffer_read_pixels (test_fb, w / 2, h / 2, 1, 1,
                                COGL_PIXEL_FORMAT_RGBA_8888_PRE, center);
  g_snprintf (got_corner, sizeof (got_corner), "#%02x%02x%02x%02x",
              corner[0], corner[1], corner[2], corner[3]);
  g_snprintf (got_center, sizeof (got_center), "#%02x%02x%02x%02x",
              center[0], center[1], center[2], center[3]);
  g_snprintf (want_corner, sizeof (want_corner), "#%08x", CLEAR_RGBA);
  g_snprintf (want_center, sizeof (want_center), "#%08x", center_rgba);
  g_printerr ("PROBE %s corner=%s expect=%s center=%s expect=%s\n",
              tag, got_corner, want_corner, got_center, want_center);
  fflush (stderr);
  g_assert_cmpstr (got_corner, ==, want_corner);
  g_assert_cmpstr (got_center, ==, want_center);
}

static CoglPipeline *
solid_pipeline (float r,
                float g,
                float b)
{
  CoglPipeline *pipeline = cogl_pipeline_new (test_ctx);
  CoglColor color;

  cogl_color_init_from_4f (&color, r, g, b, 1.f);
  cogl_pipeline_set_color (pipeline, &color);
  replace_blend (pipeline);
  return pipeline;
}

static void
draw_p3_quad (CoglPipeline *pipeline,
              float         hx,
              float         hy,
              float         z)
{
  CoglVertexP3 verts[4] = {
    { -hx, -hy, z },
    {  hx, -hy, z },
    {  hx,  hy, z },
    { -hx,  hy, z },
  };
  CoglPrimitive *prim;

  prim = cogl_primitive_new_p3 (test_ctx, COGL_VERTICES_MODE_TRIANGLE_FAN,
                                4, verts);
  cogl_primitive_draw (prim, test_fb, pipeline);
  g_object_unref (prim);
}

static void
draw_p2_quad (CoglPipeline *pipeline,
              float         hx,
              float         hy)
{
  CoglVertexP2 verts[4] = {
    { -hx, -hy },
    {  hx, -hy },
    {  hx,  hy },
    { -hx,  hy },
  };
  CoglPrimitive *prim;

  prim = cogl_primitive_new_p2 (test_ctx, COGL_VERTICES_MODE_TRIANGLE_FAN,
                                4, verts);
  cogl_primitive_draw (prim, test_fb, pipeline);
  g_object_unref (prim);
}

static void
draw_p3t2_quad (CoglPipeline *pipeline,
                float         hx,
                float         hy,
                float         z)
{
  CoglVertexP3T2 verts[4] = {
    { -hx, -hy, z, 0.f, 0.f },
    {  hx, -hy, z, 1.f, 0.f },
    {  hx,  hy, z, 1.f, 1.f },
    { -hx,  hy, z, 0.f, 1.f },
  };
  CoglPrimitive *prim;

  prim = cogl_primitive_new_p3t2 (test_ctx, COGL_VERTICES_MODE_TRIANGLE_FAN,
                                  4, verts);
  cogl_primitive_draw (prim, test_fb, pipeline);
  g_object_unref (prim);
}

/* Hot/journal rectangle in pixels. Proves the framebuffer and readback. */
static void
test_ortho_rect (void)
{
  CoglPipeline *pipeline;
  int w = cogl_framebuffer_get_width (test_fb);
  int h = cogl_framebuffer_get_height (test_fb);

  reset_matrices ();
  cogl_framebuffer_orthographic (test_fb, 0, 0, w, h, -1, 100);
  clear_blue ();
  pipeline = solid_pipeline (1.f, 0.f, 0.f);
  cogl_framebuffer_draw_rectangle (test_fb, pipeline,
                                   w * 0.25f, h * 0.25f,
                                   w * 0.75f, h * 0.75f);
  g_object_unref (pipeline);
  expect_center_and_corner ("ortho-rect", RED_RGBA);
}

/* 3-float positions, z = 0, ortho. Passes even if z is dropped. */
static void
test_ortho_p3_z0 (void)
{
  CoglPipeline *pipeline;
  int w = cogl_framebuffer_get_width (test_fb);
  int h = cogl_framebuffer_get_height (test_fb);

  reset_matrices ();
  cogl_framebuffer_orthographic (test_fb, 0, 0, w, h, -1, 100);
  clear_blue ();
  pipeline = solid_pipeline (1.f, 0.f, 0.f);
  {
    CoglVertexP3 verts[4] = {
      { w * 0.2f, h * 0.2f, 0.f },
      { w * 0.8f, h * 0.2f, 0.f },
      { w * 0.8f, h * 0.8f, 0.f },
      { w * 0.2f, h * 0.8f, 0.f },
    };
    CoglPrimitive *prim;

    prim = cogl_primitive_new_p3 (test_ctx, COGL_VERTICES_MODE_TRIANGLE_FAN,
                                  4, verts);
    cogl_primitive_draw (prim, test_fb, pipeline);
    g_object_unref (prim);
  }
  g_object_unref (pipeline);
  expect_center_and_corner ("ortho-p3-z0", RED_RGBA);
}

/* z comes from the modelview, not the attribute. Perspective itself works
 * even when a 2-float position leaves attribute z at 0.
 */
static void
test_persp_p2_modelview_z (void)
{
  CoglPipeline *pipeline;
  graphene_matrix_t mv;
  graphene_point3d_t off;
  float z = stage_z_2d (1.f);
  float hx, hy;

  set_stage_perspective ();
  graphene_point3d_init (&off, 0.f, 0.f, -z);
  graphene_matrix_init_translate (&mv, &off);
  cogl_framebuffer_set_modelview_matrix (test_fb, &mv);
  frustum_quad (z, 0.5f, &hx, &hy);
  clear_blue ();
  pipeline = solid_pipeline (1.f, 0.f, 0.f);
  draw_p2_quad (pipeline, hx, hy);
  g_object_unref (pipeline);
  expect_center_and_corner ("persp-p2-mv", RED_RGBA);
}

/* The onscreen bug: z lives in the 3-float attribute and the modelview
 * is identity. Dropping z makes clip.w = 0.
 */
static void
test_persp_p3 (void)
{
  CoglPipeline *pipeline;
  float z = stage_z_2d (1.f);
  float hx, hy;

  set_stage_perspective ();
  frustum_quad (z, 0.5f, &hx, &hy);
  clear_blue ();
  pipeline = solid_pipeline (1.f, 0.f, 0.f);
  draw_p3_quad (pipeline, hx, hy, -z);
  g_object_unref (pipeline);
  expect_center_and_corner ("persp-p3", RED_RGBA);
}

/* Cold shader, but no flip-vector multiply (fragment snippet only). */
static void
test_persp_p3_frag_snippet (void)
{
  CoglPipeline *pipeline;
  float z = stage_z_2d (1.f);
  float hx, hy;

  set_stage_perspective ();
  frustum_quad (z, 0.5f, &hx, &hy);
  clear_blue ();
  pipeline = solid_pipeline (1.f, 0.f, 0.f);
  add_snippet (pipeline, COGL_SNIPPET_HOOK_FRAGMENT,
               "/* cold fragment, no flip */");
  draw_p3_quad (pipeline, hx, hy, -z);
  g_object_unref (pipeline);
  expect_center_and_corner ("persp-p3-frag", RED_RGBA);
}

/* Cold shader plus UBO _cogl_flip_vector. A zero flip vector collapses
 * the quad even when z is correct.
 */
static void
test_persp_p3_vert_snippet (void)
{
  CoglPipeline *pipeline;
  float z = stage_z_2d (1.f);
  float hx, hy;

  set_stage_perspective ();
  frustum_quad (z, 0.5f, &hx, &hy);
  clear_blue ();
  pipeline = solid_pipeline (1.f, 0.f, 0.f);
  add_snippet (pipeline, COGL_SNIPPET_HOOK_VERTEX,
               "/* cold vertex, multiply flip vector */");
  draw_p3_quad (pipeline, hx, hy, -z);
  g_object_unref (pipeline);
  expect_center_and_corner ("persp-p3-vert", RED_RGBA);
}

/* 1×1 opaque texture, no snippet. A black sample shows up as black,
 * a missed raster as blue.
 */
static void
test_persp_p3t2 (void)
{
  CoglPipeline *pipeline;
  CoglTexture *tex;
  float z = stage_z_2d (1.f);
  float hx, hy;

  set_stage_perspective ();
  frustum_quad (z, 0.5f, &hx, &hy);
  clear_blue ();
  tex = test_utils_create_color_texture (test_ctx, GREEN_RGBA);
  pipeline = solid_pipeline (1.f, 1.f, 1.f);
  cogl_pipeline_set_layer_texture (pipeline, 0, tex);
  draw_p3t2_quad (pipeline, hx, hy, -z);
  g_object_unref (tex);
  g_object_unref (pipeline);
  expect_center_and_corner ("persp-p3t2", GREEN_RGBA);
}

/* Closest to the live stage: textured 3-float quad and a vertex snippet. */
static void
test_persp_p3t2_vert_snippet (void)
{
  CoglPipeline *pipeline;
  CoglTexture *tex;
  float z = stage_z_2d (1.f);
  float hx, hy;

  set_stage_perspective ();
  frustum_quad (z, 0.5f, &hx, &hy);
  clear_blue ();
  tex = test_utils_create_color_texture (test_ctx, GREEN_RGBA);
  pipeline = solid_pipeline (1.f, 1.f, 1.f);
  cogl_pipeline_set_layer_texture (pipeline, 0, tex);
  add_snippet (pipeline, COGL_SNIPPET_HOOK_VERTEX,
               "/* stage-like cold textured draw */");
  draw_p3t2_quad (pipeline, hx, hy, -z);
  g_object_unref (tex);
  g_object_unref (pipeline);
  expect_center_and_corner ("persp-p3t2-vert", GREEN_RGBA);
}

COGL_TEST_SUITE (
  g_test_add_func ("/perspective/01-ortho-rect", test_ortho_rect);
  g_test_add_func ("/perspective/02-ortho-p3-z0", test_ortho_p3_z0);
  g_test_add_func ("/perspective/03-persp-p2-modelview-z", test_persp_p2_modelview_z);
  g_test_add_func ("/perspective/04-persp-p3", test_persp_p3);
  g_test_add_func ("/perspective/05-persp-p3-frag-snippet", test_persp_p3_frag_snippet);
  g_test_add_func ("/perspective/06-persp-p3-vert-snippet", test_persp_p3_vert_snippet);
  g_test_add_func ("/perspective/07-persp-p3t2", test_persp_p3t2);
  g_test_add_func ("/perspective/08-persp-p3t2-vert-snippet", test_persp_p3t2_vert_snippet); )
