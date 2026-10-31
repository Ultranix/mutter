/*
 * Cogl
 *
 * A Low Level GPU Graphics and Utilities API
 *
 * Copyright (C) 2024 Red Hat.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <drm_fourcc.h>

#include "cogl/cogl-renderer-egl.h"
#include "cogl/cogl-texture-2d-private.h"

#include "cogl/driver/gl/cogl-driver-gl-private.h"
#include "cogl/driver/gl/cogl-texture-driver-gl-private.h"
#include "cogl/driver/gl/cogl-texture-2d-gl-private.h"



G_DEFINE_TYPE (CoglTextureDriverGL, cogl_texture_driver_gl, COGL_TYPE_TEXTURE_DRIVER)

static gboolean
cogl_texture_driver_gl_texture_2d_can_create (CoglTextureDriver *tex_driver,
                                              CoglContext       *ctx,
                                              int                width,
                                              int                height,
                                              CoglPixelFormat    internal_format)
{
  CoglDriver *driver = cogl_context_get_driver (ctx);
  CoglDriverGL *driver_gl = COGL_DRIVER_GL (driver);
  CoglDriverGLClass *driver_klass = COGL_DRIVER_GL_GET_CLASS (driver_gl);
  GLenum gl_intformat;
  GLenum gl_format;
  GLenum gl_type;

  /* We only support single plane formats for now */
  if (cogl_pixel_format_get_n_planes (internal_format) != 1)
    return FALSE;

  driver_klass->pixel_format_to_gl (driver_gl,
                                    internal_format,
                                    &gl_intformat,
                                    &gl_format,
                                    &gl_type);

  /* Check that the driver can create a texture with that size */
  if (!driver_klass->texture_size_supported (driver_gl,
                                             GL_TEXTURE_2D,
                                             gl_intformat,
                                             gl_format,
                                             gl_type,
                                             width,
                                             height))
    return FALSE;

  return TRUE;
}

static GType
cogl_texture_driver_gl_texture_2d_get_type (CoglTextureDriver *driver)
{
  return COGL_TYPE_TEXTURE_2D_GL;
}

static CoglTexture *
cogl_texture_driver_gl_import_dma_buf (CoglTextureDriver *driver,
                                       CoglContext       *context,
                                       int                width,
                                       int                height,
                                       CoglPixelFormat    format,
                                       uint32_t           drm_format,
                                       int                n_planes,
                                       const int         *fds,
                                       const uint32_t    *strides,
                                       const uint32_t    *offsets,
                                       const uint64_t    *modifiers,
                                       CoglYuvMatrix      matrix,
                                       CoglYuvRange       range,
                                       GError           **error)
{
  CoglRendererEGL *renderer =
    COGL_RENDERER_EGL (cogl_context_get_renderer (context));
  EGLImageKHR image;
  CoglTexture *texture;
  int i;

  if (n_planes < 1 || n_planes > 3 ||
      matrix != COGL_YUV_MATRIX_BT709 || range != COGL_YUV_RANGE_NARROW)
    {
      g_set_error_literal (error, COGL_SYSTEM_ERROR, COGL_SYSTEM_ERROR_UNSUPPORTED,
                           "Unsupported DMA buffer plane count or colorimetry");
      return NULL;
    }

  if (modifiers)
    {
      for (i = 0; i < n_planes; i++)
        if (modifiers[i] != DRM_FORMAT_MOD_LINEAR &&
            modifiers[i] != DRM_FORMAT_MOD_INVALID)
          break;
      if (i == n_planes)
        modifiers = NULL;
    }

  image = cogl_renderer_egl_create_dmabuf_image (renderer, width, height,
                                                 drm_format, n_planes,
                                                 fds, strides, offsets,
                                                 modifiers, error);
  if (image == EGL_NO_IMAGE_KHR)
    return NULL;

  texture = cogl_texture_2d_new_from_egl_image (context, width, height, format,
                                                image,
                                                COGL_EGL_IMAGE_FLAG_NO_GET_DATA,
                                                error);
  cogl_renderer_egl_destroy_image (renderer, image, NULL);
  return texture;
}

static CoglPixelFormat
cogl_texture_driver_gl_find_best_upload_format (CoglTextureDriver *tex_driver,
                                                CoglPixelFormat    format)
{
  CoglDriver *driver = cogl_texture_driver_get_driver (tex_driver);
  CoglDriverGL *driver_gl = COGL_DRIVER_GL (driver);
  CoglDriverGLClass *driver_class = COGL_DRIVER_GL_GET_CLASS (driver_gl);

  return driver_class->pixel_format_to_gl (driver_gl, format,
                                           NULL, NULL, NULL);
}

static void
cogl_texture_driver_gl_class_init (CoglTextureDriverGLClass *klass)
{
  CoglTextureDriverClass *driver_klass = COGL_TEXTURE_DRIVER_CLASS (klass);

  driver_klass->texture_2d_new_from_dma_buf =
    cogl_texture_driver_gl_import_dma_buf;

  driver_klass->find_best_upload_format =
    cogl_texture_driver_gl_find_best_upload_format;
  driver_klass->texture_2d_get_type = cogl_texture_driver_gl_texture_2d_get_type;
  driver_klass->texture_2d_can_create = cogl_texture_driver_gl_texture_2d_can_create;
}

static void
cogl_texture_driver_gl_init (CoglTextureDriverGL *driver)
{
}
