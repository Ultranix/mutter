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

/* The secondary copy code must work even when the primary Cogl driver is VK. */
#include "config.h"
#include "tests/cogl-test-utils.h"
#include "backends/native/meta-renderer-native-gles3.c"

static void
test_secondary_copy (void)
{
  EGLDisplay old_display = eglGetCurrentDisplay ();
  EGLContext old_context = eglGetCurrentContext ();
  EGLSurface old_draw = eglGetCurrentSurface (EGL_DRAW);
  EGLSurface old_read = eglGetCurrentSurface (EGL_READ);
  EGLenum old_api = eglQueryAPI ();
  EGLDisplay display;
  EGLContext context;
  EGLConfig config;
  EGLint count;
  const EGLint config_attrs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                  EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
  const EGLint context_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
  const EGLint image_attrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
  PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
    (void *) eglGetProcAddress ("eglGetPlatformDisplayEXT");
  PFNEGLCREATEIMAGEKHRPROC create_image = (void *) eglGetProcAddress ("eglCreateImageKHR");
  PFNEGLDESTROYIMAGEKHRPROC destroy_image = (void *) eglGetProcAddress ("eglDestroyImageKHR");
  __typeof__ (((CoglDriverGLPrivate *) 0)->glTexImage2D) texture_image;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glReadPixels) read_pixels;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glClearColor) clear_color;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glClear) clear;
  SecondaryGl gl;
  g_autoptr (CoglRendererEGL) renderer = cogl_renderer_egl_new ();
  g_autoptr (GError) error = NULL;
  GLuint textures[2], fb;
  EGLImageKHR image;
  uint8_t source[4 * 4 * 4], result[4 * 4 * 4];
  g_autoptr (MtkRegion) region = mtk_region_create_rectangle (&MTK_RECTANGLE_INIT (1, 1, 2, 2));

  g_assert_nonnull (get_display);
  display = get_display (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
  g_assert_true (eglInitialize (display, NULL, NULL));
  g_assert_true (eglBindAPI (EGL_OPENGL_ES_API));
  g_assert_true (eglChooseConfig (display, config_attrs, &config, 1, &count));
  g_assert_cmpint (count, ==, 1);
  context = eglCreateContext (display, config, EGL_NO_CONTEXT, context_attrs);
  g_assert_true (context != EGL_NO_CONTEXT);
  g_assert_true (eglMakeCurrent (display, EGL_NO_SURFACE, EGL_NO_SURFACE, context));
  cogl_renderer_egl_set_edisplay (renderer, display);
  g_assert_true (meta_renderer_native_gles3_validate_context (renderer, &error));
  g_assert_no_error (error);
  g_assert_true (load_secondary_gl (&gl, renderer, &error));
  texture_image = (void *) eglGetProcAddress ("glTexImage2D");
  read_pixels = (void *) eglGetProcAddress ("glReadPixels");
  clear_color = (void *) eglGetProcAddress ("glClearColor");
  clear = (void *) eglGetProcAddress ("glClear");

  for (int i = 0; i < 16; i++)
    memcpy (&source[i * 4], (uint8_t[]) { 192, 64, 32, 255 }, 4);
  gl.glGenTextures (2, textures);
  gl.glBindTexture (GL_TEXTURE_2D, textures[0]);
  texture_image (GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, source);
  image = create_image (display, context, EGL_GL_TEXTURE_2D_KHR,
                        (EGLClientBuffer) (uintptr_t) textures[0], image_attrs);
  g_assert_true (image != EGL_NO_IMAGE_KHR);
  gl.glBindTexture (GL_TEXTURE_2D, textures[1]);
  texture_image (GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  gl.glGenFramebuffers (1, &fb);
  gl.glBindFramebuffer (GL_FRAMEBUFFER, fb);
  gl.glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[1], 0);
  g_assert_cmpuint (gl.glCheckFramebufferStatus (GL_FRAMEBUFFER), ==, GL_FRAMEBUFFER_COMPLETE);

  ContextData data = { .gl = gl };

  for (int shader = 0; shader < 3; shader++)
    {
      /* Reusing the cached program must restore it after another user. */
      gl.glUseProgram (0);
      gl.glBindFramebuffer (GL_FRAMEBUFFER, fb);
      clear_color (0, 0, 1, 1);
      clear (GL_COLOR_BUFFER_BIT);
      if (shader)
        g_assert_true (paint_egl_image (&data, &gl, image, 4, 4, region, &error));
      else
        blit_egl_image (&gl, image, 4, 4, region);
      gl.glBindFramebuffer (GL_FRAMEBUFFER, fb);
      read_pixels (0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, result);
      g_assert_cmpuint (gl.glGetError (), ==, GL_NO_ERROR);
      for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
          {
            const uint8_t *expected = x >= 1 && x < 3 && y >= 1 && y < 3 ?
                                      source : (const uint8_t[]) { 0, 0, 255, 255 };
            for (int c = 0; c < 4; c++)
              g_assert_cmpint (ABS ((int) result[(y * 4 + x) * 4 + c] - expected[c]), <=, 1);
          }
    }

  if (data.shader_program)
    gl.glDeleteProgram (data.shader_program);
  destroy_image (display, image);
  gl.glDeleteTextures (2, textures);
  gl.glDeleteFramebuffers (1, &fb);
  eglMakeCurrent (display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext (display, context);
  /* The EGLDisplay can be shared with the primary renderer: do not terminate.
   */
  eglBindAPI (old_api);
  if (old_display != EGL_NO_DISPLAY)
    g_assert_true (eglMakeCurrent (old_display, old_draw, old_read, old_context));
}

COGL_TEST_SUITE (g_test_add_func ("/secondary-egl/partial-copy", test_secondary_copy); )
