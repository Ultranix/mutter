/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2017 Red Hat
 * Copyright (c) 2018 DisplayLink (UK) Ltd.
 * Copyright (c) 2023 Canonical Ltd.
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
 */

#include "config.h"

#include "backends/native/meta-renderer-native-gles3.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <gio/gio.h>
#include <string.h>

#include "cogl/cogl.h"
#include "cogl/driver/gl/cogl-driver-gl-private.h"
#include "meta/meta-debug.h"
#include "mtk/mtk.h"

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif
#ifndef GL_TEXTURE_WRAP_R_OES
#define GL_TEXTURE_WRAP_R_OES 0x8072
#endif

#define TRIANGLE_COUNT_PER_RECTANGLE 2
#define VERTICES_PER_TRIANGLE 3
#define VALUES_PER_VERTEX 4
#define COMPONENTS_PER_RECTANGLE TRIANGLE_COUNT_PER_RECTANGLE * VERTICES_PER_TRIANGLE * VALUES_PER_VERTEX

/* Dispatch belongs to the secondary EGL context, independently of the
 * primary Cogl driver (which may be Vulkan). */
typedef struct
{
  __typeof__ (((CoglDriverGLPrivate *) 0)->glActiveTexture) glActiveTexture;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glAttachShader) glAttachShader;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glBindBuffer) glBindBuffer;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glBindFramebuffer) glBindFramebuffer;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glBindTexture) glBindTexture;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glBindVertexArray) glBindVertexArray;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glBlitFramebuffer) glBlitFramebuffer;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glBufferData) glBufferData;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glCheckFramebufferStatus) glCheckFramebufferStatus;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glCompileShader) glCompileShader;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glCreateProgram) glCreateProgram;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glCreateShader) glCreateShader;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glDeleteBuffers) glDeleteBuffers;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glDeleteFramebuffers) glDeleteFramebuffers;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glDeleteProgram) glDeleteProgram;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glDeleteShader) glDeleteShader;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glDeleteTextures) glDeleteTextures;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glDeleteVertexArrays) glDeleteVertexArrays;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glDrawArrays) glDrawArrays;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glEGLImageTargetTexture2D) glEGLImageTargetTexture2D;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glEnableVertexAttribArray) glEnableVertexAttribArray;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glFramebufferTexture2D) glFramebufferTexture2D;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glGenBuffers) glGenBuffers;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glGenFramebuffers) glGenFramebuffers;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glGenTextures) glGenTextures;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glGenVertexArrays) glGenVertexArrays;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glGetAttribLocation) glGetAttribLocation;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glGetError) glGetError;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glGetProgramInfoLog) glGetProgramInfoLog;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glGetProgramiv) glGetProgramiv;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glGetShaderInfoLog) glGetShaderInfoLog;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glGetShaderiv) glGetShaderiv;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glGetString) glGetString;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glGetUniformLocation) glGetUniformLocation;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glLinkProgram) glLinkProgram;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glShaderSource) glShaderSource;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glTexParameteri) glTexParameteri;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glUniform1f) glUniform1f;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glUseProgram) glUseProgram;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glVertexAttribPointer) glVertexAttribPointer;
  __typeof__ (((CoglDriverGLPrivate *) 0)->glViewport) glViewport;
} SecondaryGl;

#define SG(driver, call) ((driver)->call)
#define SG_RET(result, driver, call) ((result) = (driver)->call)

static gboolean
load_secondary_gl (SecondaryGl     *gl,
                   CoglRendererEGL *renderer,
                   GError         **error)
{
#define LOAD(member, symbol) G_STMT_START { \
    gl->member = (__typeof__ (gl->member)) \
                 cogl_renderer_get_proc_address (COGL_RENDERER (renderer), symbol); \
    if (!gl->member) { \
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, \
                     "Missing secondary GL entry point: %s", symbol); \
        return FALSE; \
      } \
} G_STMT_END
  LOAD (glActiveTexture, "glActiveTexture");
  LOAD (glAttachShader, "glAttachShader");
  LOAD (glBindBuffer, "glBindBuffer");
  LOAD (glBindFramebuffer, "glBindFramebuffer");
  LOAD (glBindTexture, "glBindTexture");
  LOAD (glBindVertexArray, "glBindVertexArray");
  LOAD (glBlitFramebuffer, "glBlitFramebuffer");
  LOAD (glBufferData, "glBufferData");
  LOAD (glCheckFramebufferStatus, "glCheckFramebufferStatus");
  LOAD (glCompileShader, "glCompileShader");
  LOAD (glCreateProgram, "glCreateProgram");
  LOAD (glCreateShader, "glCreateShader");
  LOAD (glDeleteBuffers, "glDeleteBuffers");
  LOAD (glDeleteFramebuffers, "glDeleteFramebuffers");
  LOAD (glDeleteProgram, "glDeleteProgram");
  LOAD (glDeleteShader, "glDeleteShader");
  LOAD (glDeleteTextures, "glDeleteTextures");
  LOAD (glDeleteVertexArrays, "glDeleteVertexArrays");
  LOAD (glDrawArrays, "glDrawArrays");
  LOAD (glEGLImageTargetTexture2D, "glEGLImageTargetTexture2DOES");
  LOAD (glEnableVertexAttribArray, "glEnableVertexAttribArray");
  LOAD (glFramebufferTexture2D, "glFramebufferTexture2D");
  LOAD (glGenBuffers, "glGenBuffers");
  LOAD (glGenFramebuffers, "glGenFramebuffers");
  LOAD (glGenTextures, "glGenTextures");
  LOAD (glGenVertexArrays, "glGenVertexArrays");
  LOAD (glGetAttribLocation, "glGetAttribLocation");
  LOAD (glGetError, "glGetError");
  LOAD (glGetProgramInfoLog, "glGetProgramInfoLog");
  LOAD (glGetProgramiv, "glGetProgramiv");
  LOAD (glGetShaderInfoLog, "glGetShaderInfoLog");
  LOAD (glGetShaderiv, "glGetShaderiv");
  LOAD (glGetString, "glGetString");
  LOAD (glGetUniformLocation, "glGetUniformLocation");
  LOAD (glLinkProgram, "glLinkProgram");
  LOAD (glShaderSource, "glShaderSource");
  LOAD (glTexParameteri, "glTexParameteri");
  LOAD (glUniform1f, "glUniform1f");
  LOAD (glUseProgram, "glUseProgram");
  LOAD (glVertexAttribPointer, "glVertexAttribPointer");
  LOAD (glViewport, "glViewport");
#undef LOAD

  return TRUE;
}

gboolean
meta_renderer_native_gles3_validate_context (CoglRendererEGL *renderer,
                                             GError         **error)
{
  SecondaryGl gl;
  const char *extensions;
  g_auto (GStrv) tokens = NULL;

  if (!load_secondary_gl (&gl, renderer, error))
    return FALSE;
  extensions = (const char *) gl.glGetString (GL_EXTENSIONS);
  tokens = g_strsplit (extensions ? extensions : "", " ", -1);
  if (!g_strv_contains ((const char * const *) tokens, "GL_OES_EGL_image_external"))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "Secondary context lacks GL_OES_EGL_image_external");
      return FALSE;
    }
  return TRUE;
}

typedef struct _ContextData
{
  SecondaryGl gl;
  GArray *buffer_support;
  GLuint shader_program;
} ContextData;

typedef struct
{
  uint32_t drm_format;
  uint64_t drm_modifier;
  gboolean can_blit;
} BufferTypeSupport;

static void
context_data_free (ContextData *context_data)
{
  g_array_free (context_data->buffer_support, TRUE);
  g_free (context_data);
}

static GQuark
get_quark_for_egl_context (EGLContext egl_context)
{
  char key[128];

  g_snprintf (key, sizeof key, "EGLContext %p", egl_context);

  return g_quark_from_string (key);
}

static gboolean
can_blit_buffer (ContextData  *context_data,
                 CoglRenderer *renderer,
                 uint32_t      drm_format,
                 uint64_t      drm_modifier)
{
  g_autoptr (GArray) modifiers = NULL;
  g_autoptr (GArray) external_only = NULL;
  g_autoptr (GError) error = NULL;
  int i;
  gboolean can_blit;
  BufferTypeSupport support;

  can_blit = drm_modifier == DRM_FORMAT_MOD_LINEAR;

  for (i = 0; i < context_data->buffer_support->len; i++)
    {
      BufferTypeSupport *other_support =
        &g_array_index (context_data->buffer_support, BufferTypeSupport, i);

      if (other_support->drm_format == drm_format &&
          other_support->drm_modifier == drm_modifier)
        return other_support->can_blit;
    }

  modifiers = g_array_new (FALSE, FALSE, sizeof (uint64_t));
  external_only = g_array_new (FALSE, FALSE, sizeof (gboolean));

  if (!cogl_renderer_query_dma_buf_modifiers (renderer, drm_format,
                                              modifiers, external_only,
                                              &error))
    {
      meta_topic (META_DEBUG_RENDER,
                  "Failed to query supported DMA buffer modifiers (%s), "
                  "assuming blitting linearly will still work.",
                  error->message);
      goto out;
    }

  can_blit = FALSE;
  for (i = 0; i < modifiers->len; i++)
    {
      if (drm_modifier == g_array_index (modifiers, uint64_t, i))
        {
          can_blit = !g_array_index (external_only, gboolean, i);
          goto out;
        }
    }

out:
  support = (BufferTypeSupport) {
    .drm_format = drm_format,
    .drm_modifier = drm_modifier,
    .can_blit = can_blit,
  };
  g_array_append_val (context_data->buffer_support, support);
  return can_blit;
}

static GLuint
load_shader (SecondaryGl *driver,
             const char  *src,
             GLenum       type,
             GError     **error)
{
  GLuint shader;

  SG_RET (shader, driver, glCreateShader (type));

  if (shader)
    {
      GLint compiled;

      SG (driver, glShaderSource (shader, 1, &src, NULL));
      SG (driver, glCompileShader (shader));
      SG (driver, glGetShaderiv (shader, GL_COMPILE_STATUS, &compiled));
      if (!compiled)
        {
          GLchar log[1024];

          SG (driver, glGetShaderInfoLog (shader, sizeof (log) - 1, NULL, log));
          log[sizeof (log) - 1] = '\0';
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Secondary shader compilation failed: %s", log);
          SG (driver, glDeleteShader (shader));
          shader = 0;
        }
    }
  else
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Failed to create secondary shader");

  return shader;
}

static gboolean
ensure_shader_program (ContextData *context_data,
                       SecondaryGl *driver,
                       GError     **error)
{
  static const char vertex_shader_source[] =
    "#version 100\n"
    "attribute vec2 position;\n"
    "attribute vec2 texcoord;\n"
    "varying vec2 v_texcoord;\n"
    "uniform float framebuffer_width;\n"
    "uniform float framebuffer_height;\n"
    "\n"
    "void main()\n"
    "{\n"
    "  gl_Position = vec4(position.x / framebuffer_width * 2.0 - 1.0, position.y / framebuffer_height * 2.0 - 1.0, 0.0, 1.0);\n"
    "  v_texcoord = vec2(texcoord.x / framebuffer_width, texcoord.y / framebuffer_height);\n"
    "}\n";

  static const char fragment_shader_source[] =
    "#version 100\n"
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float;\n"
    "uniform samplerExternalOES s_texture;\n"
    "varying vec2 v_texcoord;\n"
    "\n"
    " void main()\n"
    "{\n"
    "  gl_FragColor = texture2D(s_texture, v_texcoord);\n"
    "}\n";

  GLint linked;
  GLuint vertex_shader = 0, fragment_shader = 0;
  GLuint shader_program;

  if (context_data->shader_program)
    return TRUE;

  SG_RET (shader_program, driver, glCreateProgram ());
  if (!shader_program)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Failed to create secondary shader program");
      return FALSE;
    }

  vertex_shader = load_shader (driver, vertex_shader_source, GL_VERTEX_SHADER, error);
  if (!vertex_shader)
    goto failed;
  fragment_shader = load_shader (driver, fragment_shader_source, GL_FRAGMENT_SHADER, error);
  if (!fragment_shader)
    goto failed;

  SG (driver, glAttachShader (shader_program, vertex_shader));
  SG (driver, glAttachShader (shader_program, fragment_shader));
  SG (driver, glLinkProgram (shader_program));
  SG (driver, glGetProgramiv (shader_program, GL_LINK_STATUS, &linked));
  if (!linked)
    {
      GLchar log[1024];

      SG (driver, glGetProgramInfoLog (shader_program, sizeof (log) - 1, NULL, log));
      log[sizeof (log) - 1] = '\0';
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Secondary shader link failed: %s", log);
      goto failed;
    }

  SG (driver, glDeleteShader (vertex_shader));
  SG (driver, glDeleteShader (fragment_shader));
  context_data->shader_program = shader_program;
  return TRUE;

failed:
  if (vertex_shader)
    SG (driver, glDeleteShader (vertex_shader));
  if (fragment_shader)
    SG (driver, glDeleteShader (fragment_shader));
  SG (driver, glDeleteProgram (shader_program));
  return FALSE;
}

static void
clear_gl_errors (SecondaryGl *driver)
{
  while (driver->glGetError () != GL_NO_ERROR)
    ;
}

static void
blit_egl_image (SecondaryGl      *driver,
                EGLImageKHR       egl_image,
                int               width,
                int               height,
                const MtkRegion  *region)
{
  GLuint texture;
  GLuint framebuffer;
  int i;
  int n_rectangles = mtk_region_num_rectangles (region);

  SG (driver, glViewport (0, 0, width, height));

  SG (driver, glGenFramebuffers (1, &framebuffer));
  SG (driver, glBindFramebuffer (GL_READ_FRAMEBUFFER, framebuffer));

  SG (driver, glActiveTexture (GL_TEXTURE0));
  SG (driver, glGenTextures (1, &texture));
  SG (driver, glBindTexture (GL_TEXTURE_2D, texture));
  SG (driver, glEGLImageTargetTexture2D (GL_TEXTURE_2D, egl_image));
  SG (driver, glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                               GL_NEAREST));
  SG (driver, glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                               GL_NEAREST));
  SG (driver, glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                               GL_CLAMP_TO_EDGE));
  SG (driver, glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                               GL_CLAMP_TO_EDGE));
  SG (driver, glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_R_OES,
                               GL_CLAMP_TO_EDGE));

  SG (driver, glFramebufferTexture2D (GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                      GL_TEXTURE_2D, texture, 0));

  for (i = 0; i < n_rectangles; ++i)
    {
      MtkRectangle rectangle;
      GLint x1, y1, x2, y2;

      rectangle = mtk_region_get_rectangle (region, i);

      x1 = rectangle.x;
      y1 = rectangle.y;
      x2 = x1 + rectangle.width;
      y2 = y1 + rectangle.height;

      SG (driver, glBlitFramebuffer (x1, y1, x2, y2,
                                     x1, y1, x2, y2,
                                     GL_COLOR_BUFFER_BIT,
                                     GL_NEAREST));
    }


  SG (driver, glDeleteTextures (1, &texture));
  SG (driver, glDeleteFramebuffers (1, &framebuffer));
}

static gboolean
paint_egl_image (ContextData      *context_data,
                 SecondaryGl      *driver,
                 EGLImageKHR       egl_image,
                 int               width,
                 int               height,
                 const MtkRegion  *region,
                 GError          **error)
{
  int i;
  GLuint texture;
  GLint *vertices = NULL;
  GLuint vertex_buffer_object;
  GLuint vertex_array_object;
  GLint position_attrib, texcoord_attrib;
  GLint framebuffer_width_uniform, framebuffer_height_uniform;
  GLuint size_per_rectangle = COMPONENTS_PER_RECTANGLE * sizeof(GLint);
  GLuint vertices_size;
  int n_rectangles = mtk_region_num_rectangles (region);

  vertices_size = n_rectangles * size_per_rectangle;

  vertices = g_alloca (vertices_size);

  for (i = 0; i < n_rectangles; ++i)
    {
      GLint x1, y1, x2, y2;
      GLint u1, v1, u2, v2;
      MtkRectangle rectangle;
      GLint *rectangle_vertices;

      rectangle = mtk_region_get_rectangle (region, i);
      rectangle_vertices = &vertices[i * COMPONENTS_PER_RECTANGLE];

      x1 = rectangle.x;
      y1 = rectangle.y;
      x2 = rectangle.x + rectangle.width;
      y2 = rectangle.y + rectangle.height;

      u1 = rectangle.x;
      v1 = rectangle.y;
      u2 = rectangle.x + rectangle.width;
      v2 = rectangle.y + rectangle.height;

      rectangle_vertices[0] = x1;
      rectangle_vertices[1] = y1;
      rectangle_vertices[2] = u1;
      rectangle_vertices[3] = v1;

      rectangle_vertices[4] = x2;
      rectangle_vertices[5] = y1;
      rectangle_vertices[6] = u2;
      rectangle_vertices[7] = v1;

      rectangle_vertices[8] = x1;
      rectangle_vertices[9] = y2;
      rectangle_vertices[10] = u1;
      rectangle_vertices[11] = v2;

      rectangle_vertices[12] = x1;
      rectangle_vertices[13] = y2;
      rectangle_vertices[14] = u1;
      rectangle_vertices[15] = v2;

      rectangle_vertices[16] = x2;
      rectangle_vertices[17] = y1;
      rectangle_vertices[18] = u2;
      rectangle_vertices[19] = v1;

      rectangle_vertices[20] = x2;
      rectangle_vertices[21] = y2;
      rectangle_vertices[22] = u2;
      rectangle_vertices[23] = v2;
    }

  if (!ensure_shader_program (context_data, driver, error))
    return FALSE;
  SG (driver, glUseProgram (context_data->shader_program));

  SG (driver, glViewport (0, 0, width, height));

  SG (driver, glGenVertexArrays (1, &vertex_array_object));
  SG (driver, glBindVertexArray (vertex_array_object));

  SG (driver, glGenBuffers (1, &vertex_buffer_object));
  SG (driver, glBindBuffer (GL_ARRAY_BUFFER,
                            vertex_buffer_object));
  SG (driver, glBufferData (GL_ARRAY_BUFFER,
                            vertices_size,
                            vertices,
                            GL_DYNAMIC_DRAW));

  SG_RET (position_attrib, driver,
          glGetAttribLocation (context_data->shader_program, "position"));
  SG (driver, glEnableVertexAttribArray (position_attrib));
  SG (driver, glVertexAttribPointer (position_attrib, 2, GL_INT, GL_FALSE,
                                     4 * sizeof (GLint), NULL));

  SG_RET (texcoord_attrib, driver,
          glGetAttribLocation (context_data->shader_program, "texcoord"));
  SG (driver, glEnableVertexAttribArray (texcoord_attrib));
  SG (driver, glVertexAttribPointer (texcoord_attrib, 2, GL_INT, GL_FALSE,
                                     4 * sizeof (GLint),
                                     (void*)(sizeof(GLint) * 2)));

  SG_RET (framebuffer_width_uniform, driver,
          glGetUniformLocation (context_data->shader_program,
                                "framebuffer_width"));
  SG (driver, glUniform1f (framebuffer_width_uniform, width));

  SG_RET (framebuffer_height_uniform, driver,
          glGetUniformLocation (context_data->shader_program,
                                "framebuffer_height"));
  SG (driver, glUniform1f (framebuffer_height_uniform, height));

  SG (driver, glActiveTexture (GL_TEXTURE0));
  SG (driver, glGenTextures (1, &texture));
  SG (driver, glBindTexture (GL_TEXTURE_EXTERNAL_OES, texture));
  SG (driver, glEGLImageTargetTexture2D (GL_TEXTURE_EXTERNAL_OES, egl_image));
  SG (driver, glTexParameteri (GL_TEXTURE_EXTERNAL_OES,
                               GL_TEXTURE_MAG_FILTER,
                               GL_NEAREST));
  SG (driver, glTexParameteri (GL_TEXTURE_EXTERNAL_OES,
                               GL_TEXTURE_MIN_FILTER,
                               GL_NEAREST));
  SG (driver, glTexParameteri (GL_TEXTURE_EXTERNAL_OES,
                               GL_TEXTURE_WRAP_S,
                               GL_CLAMP_TO_EDGE));
  SG (driver, glTexParameteri (GL_TEXTURE_EXTERNAL_OES,
                               GL_TEXTURE_WRAP_T,
                               GL_CLAMP_TO_EDGE));

  SG (driver, glDrawArrays (GL_TRIANGLES, 0,
                            TRIANGLE_COUNT_PER_RECTANGLE *
                            VERTICES_PER_TRIANGLE *
                            n_rectangles));

  SG (driver, glDeleteTextures (1, &texture));
  SG (driver, glDeleteBuffers (1, &vertex_buffer_object));
  SG (driver, glDeleteVertexArrays (1, &vertex_array_object));
  return TRUE;
}

gboolean
meta_renderer_native_gles3_blit_shared_bo (CoglDriver       *cache_owner,
                                           CoglRenderer     *renderer,
                                           EGLContext        egl_context,
                                           EGLImageKHR       dst_egl_image,
                                           EGLImageKHR       src_egl_image,
                                           struct gbm_bo    *shared_bo,
                                           const MtkRegion  *region,
                                           GError          **error)
{
  unsigned int width;
  unsigned int height;
  GQuark context_data_quark;
  ContextData *context_data;
  SecondaryGl *driver;
  GLenum gl_error;
  GLuint dst_texture, dst_framebuffer;
  gboolean can_blit;
  gboolean copied = TRUE;

  /* Policy peer CopyTexture…: empty region → no BeginUpdate / FBO setup. */
  if (!region || mtk_region_is_empty (region))
    return TRUE;

  context_data_quark = get_quark_for_egl_context (egl_context);
  context_data = g_object_get_qdata (G_OBJECT (cache_owner), context_data_quark);
  if (!context_data)
    {
      context_data = g_new0 (ContextData, 1);
      if (!load_secondary_gl (&context_data->gl, COGL_RENDERER_EGL (renderer), error))
        {
          g_free (context_data);
          return FALSE;
        }
      context_data->buffer_support = g_array_new (FALSE, FALSE,
                                                  sizeof (BufferTypeSupport));

      g_object_set_qdata_full (G_OBJECT (cache_owner),
                               context_data_quark,
                               context_data,
                               (GDestroyNotify) context_data_free);
    }

  driver = &context_data->gl;
  clear_gl_errors (driver);
  can_blit = can_blit_buffer (context_data,
                              renderer,
                              gbm_bo_get_format (shared_bo),
                              gbm_bo_get_modifier (shared_bo));

  width = gbm_bo_get_width (shared_bo);
  height = gbm_bo_get_height (shared_bo);

  SG (driver, glGenFramebuffers (1, &dst_framebuffer));
  SG (driver, glBindFramebuffer (GL_DRAW_FRAMEBUFFER, dst_framebuffer));
  SG (driver, glActiveTexture (GL_TEXTURE0));
  SG (driver, glGenTextures (1, &dst_texture));
  SG (driver, glBindTexture (GL_TEXTURE_2D, dst_texture));
  SG (driver, glEGLImageTargetTexture2D (GL_TEXTURE_2D, dst_egl_image));
  SG (driver, glFramebufferTexture2D (GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                      GL_TEXTURE_2D, dst_texture, 0));

  if (driver->glCheckFramebufferStatus (GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
      driver->glDeleteTextures (1, &dst_texture);
      driver->glDeleteFramebuffers (1, &dst_framebuffer);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Secondary output framebuffer is incomplete");
      return FALSE;
    }

  if (can_blit)
    blit_egl_image (driver, src_egl_image, width, height, region);
  else
    copied = paint_egl_image (context_data, driver, src_egl_image,
                              width, height, region, error);

  SG (driver, glDeleteTextures (1, &dst_texture));
  SG (driver, glDeleteFramebuffers (1, &dst_framebuffer));
  gl_error = driver->glGetError ();
  if (!copied)
    return FALSE;
  if (gl_error != GL_NO_ERROR)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Secondary GL copy failed (0x%x)", gl_error);
      return FALSE;
    }
  return TRUE;
}

void
meta_renderer_native_gles3_forget_context (CoglDriver *driver,
                                           EGLContext  egl_context)
{
  GQuark context_data_quark = get_quark_for_egl_context (egl_context);

  g_object_set_qdata (G_OBJECT (driver), context_data_quark, NULL);
}
