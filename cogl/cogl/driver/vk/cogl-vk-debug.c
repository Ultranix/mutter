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

#include "cogl/driver/vk/cogl-vk-debug-private.h"

#include "cogl/cogl-attribute-private.h"
#include "cogl/cogl-attribute-buffer.h"
#include "cogl/cogl-buffer-private.h"
#include "cogl/cogl-color.h"
#include "cogl/cogl-framebuffer.h"
#include "cogl/cogl-framebuffer-private.h"
#include "cogl/cogl-pipeline.h"
#include "cogl/cogl-pipeline-state.h"
#include "cogl/cogl-pixel-format.h"
#include "cogl/cogl-types.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static GOnce debug_once = G_ONCE_INIT;
static unsigned int debug_mask;
static FILE *debug_fp;

enum
{
  CH_VBO = 1 << 0,
  CH_BARRIER = 1 << 1,
  CH_READBACK = 1 << 2,
  CH_MVP = 1 << 3,
  CH_TEX = 1 << 4,
  CH_FENCE = 1 << 5,
  CH_BUF = 1 << 6,
  CH_DRAW = 1 << 7,
};

#define CH_ALL (CH_VBO | CH_BARRIER | CH_READBACK | CH_MVP | CH_TEX | \
                CH_FENCE | CH_BUF | CH_DRAW)

static gpointer
init_debug (gpointer data)
{
  const char *env;
  g_auto (GStrv) parts = NULL;
  int i;

  (void) data;
  env = g_getenv ("COGL_VK_DEBUG");
  if (!env || !*env)
    return NULL;

  if (g_strcmp0 (env, "1") == 0 ||
      g_strcmp0 (env, "all") == 0 ||
      g_strcmp0 (env, "true") == 0)
    {
      debug_mask = CH_ALL;
    }
  else
    {
      parts = g_strsplit (env, ",", -1);
      for (i = 0; parts && parts[i]; i++)
        {
          const char *p = g_strstrip (parts[i]);
          if (g_strcmp0 (p, "vbo") == 0)
            debug_mask |= CH_VBO | CH_MVP | CH_TEX;
          else if (g_strcmp0 (p, "barrier") == 0)
            debug_mask |= CH_BARRIER;
          else if (g_strcmp0 (p, "readback") == 0)
            debug_mask |= CH_READBACK;
          else if (g_strcmp0 (p, "mvp") == 0)
            debug_mask |= CH_MVP;
          else if (g_strcmp0 (p, "tex") == 0)
            debug_mask |= CH_TEX;
          else if (g_strcmp0 (p, "fence") == 0)
            debug_mask |= CH_FENCE;
          else if (g_strcmp0 (p, "buf") == 0)
            debug_mask |= CH_BUF;
          else if (g_strcmp0 (p, "draw") == 0)
            debug_mask |= CH_DRAW;
          else if (g_strcmp0 (p, "all") == 0)
            debug_mask |= CH_ALL;
        }
    }

  if (debug_mask)
    {
      debug_fp = fopen ("/tmp/cogl-vk-debug.log", "a");
      if (debug_fp)
        setvbuf (debug_fp, NULL, _IOLBF, 0);
      g_message ("Cogl Vulkan: COGL_VK_DEBUG=%s mask=0x%x → /tmp/cogl-vk-debug.log",
                 env, debug_mask);
    }
  return NULL;
}

static unsigned int
channel_bit (const char *channel)
{
  if (!channel)
    return 0;
  if (g_strcmp0 (channel, "vbo") == 0)
    return CH_VBO;
  if (g_strcmp0 (channel, "barrier") == 0)
    return CH_BARRIER;
  if (g_strcmp0 (channel, "readback") == 0)
    return CH_READBACK;
  if (g_strcmp0 (channel, "mvp") == 0)
    return CH_MVP;
  if (g_strcmp0 (channel, "tex") == 0)
    return CH_TEX;
  if (g_strcmp0 (channel, "fence") == 0)
    return CH_FENCE;
  if (g_strcmp0 (channel, "buf") == 0)
    return CH_BUF;
  if (g_strcmp0 (channel, "draw") == 0)
    return CH_DRAW;
  return CH_ALL;
}

gboolean
cogl_vk_debug_enabled (const char *channel)
{
  g_once (&debug_once, init_debug, NULL);
  return (debug_mask & channel_bit (channel)) != 0;
}

void
cogl_vk_debug_log (const char *channel,
                   const char *fmt,
                   ...)
{
  g_autofree char *msg = NULL;
  va_list ap;

  g_once (&debug_once, init_debug, NULL);
  if (!(debug_mask & channel_bit (channel)))
    return;

  va_start (ap, fmt);
  msg = g_strdup_vprintf (fmt, ap);
  va_end (ap);

  g_message ("vk-debug[%s]: %s", channel, msg);
  if (debug_fp)
    {
      fprintf (debug_fp, "[%s] %s\n", channel, msg);
      fflush (debug_fp);
    }
}

void
cogl_vk_debug_mid_rp_allocate (int   width,
                               int   height,
                               void *tex)
{
  /* Intentionally empty — gdb: break cogl_vk_debug_mid_rp_allocate */
  (void) width;
  (void) height;
  (void) tex;
}

static const char *
mode_name (CoglVerticesMode mode)
{
  switch (mode)
    {
    case COGL_VERTICES_MODE_POINTS: return "POINTS";
    case COGL_VERTICES_MODE_LINES: return "LINES";
    case COGL_VERTICES_MODE_LINE_LOOP: return "LINE_LOOP";
    case COGL_VERTICES_MODE_LINE_STRIP: return "LINE_STRIP";
    case COGL_VERTICES_MODE_TRIANGLES: return "TRIANGLES";
    case COGL_VERTICES_MODE_TRIANGLE_STRIP: return "TRIANGLE_STRIP";
    case COGL_VERTICES_MODE_TRIANGLE_FAN: return "TRIANGLE_FAN";
    default: return "?";
    }
}

static const char *
topology_name (VkPrimitiveTopology t)
{
  switch (t)
    {
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: return "TRI_LIST";
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return "TRI_STRIP";
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN: return "TRI_FAN";
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return "POINTS";
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: return "LINES";
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: return "LINE_STRIP";
    default: return "?";
    }
}

void
cogl_vk_debug_image_texels (const char  *tag,
                            CoglVkImage *image)
{
  VkImageSubresource sub = {
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
  };
  VkSubresourceLayout layout;
  void *mapped = NULL;
  uint32_t bpp;
  uint32_t x, y;
  uint32_t max_x, max_y;
  VkResult res;
  VkFormatProperties props;

  if (!cogl_vk_debug_enabled ("tex") || !image || !image->device)
    return;

  vkGetPhysicalDeviceFormatProperties (image->device->physical_device,
                                       image->format, &props);
  cogl_vk_debug_log (
    "tex",
    "%s img=%p %ux%u vk_fmt=%d cogl_fmt=%d host_vis=%d layout=%d tiling=linear "
    "feat_linear=0x%llx feat_optimal=0x%llx sampled_lin=%d sampled_opt=%d",
    tag ? tag : "img",
    (void *) image, image->width, image->height,
    (int) image->format, (int) image->cogl_format,
    image->host_visible, (int) image->layout,
    (unsigned long long) props.linearTilingFeatures,
    (unsigned long long) props.optimalTilingFeatures,
    (props.linearTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0,
    (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0);

  if (!image->host_visible || image->n_memories < 1 ||
      image->memories[0] == VK_NULL_HANDLE)
    return;

  bpp = 4;
  if (image->format == VK_FORMAT_R8_UNORM ||
      image->format == VK_FORMAT_R8_SRGB)
    bpp = 1;
  else if (image->format == VK_FORMAT_R8G8_UNORM)
    bpp = 2;
  else if (image->format == VK_FORMAT_R8G8B8_UNORM ||
           image->format == VK_FORMAT_B8G8R8_UNORM)
    bpp = 3;
  else
    bpp = 4;

  vkGetImageSubresourceLayout (image->device->device, image->image, &sub,
                               &layout);
  res = vkMapMemory (image->device->device, image->memories[0], 0,
                     VK_WHOLE_SIZE, 0, &mapped);
  if (res != VK_SUCCESS || !mapped)
    {
      cogl_vk_debug_log ("tex", "  map failed (%d)", (int) res);
      return;
    }

  cogl_vk_debug_log ("tex",
                     "  subresource offset=%llu rowPitch=%llu bpp=%u",
                     (unsigned long long) layout.offset,
                     (unsigned long long) layout.rowPitch, bpp);

  max_y = MIN (image->height, 2u);
  max_x = MIN (image->width, 4u);
  for (y = 0; y < max_y; y++)
    {
      for (x = 0; x < max_x; x++)
        {
          const uint8_t *p = (const uint8_t *) mapped + layout.offset +
                             y * layout.rowPitch + x * bpp;
          if (bpp >= 4)
            cogl_vk_debug_log ("tex",
                               "  texel(%u,%u)=#%02x%02x%02x%02x",
                               x, y, p[0], p[1], p[2], p[3]);
          else if (bpp == 3)
            cogl_vk_debug_log ("tex",
                               "  texel(%u,%u)=#%02x%02x%02x (rgb888)",
                               x, y, p[0], p[1], p[2]);
          else if (bpp == 2)
            cogl_vk_debug_log ("tex",
                               "  texel(%u,%u)=#%02x%02x",
                               x, y, p[0], p[1]);
          else
            cogl_vk_debug_log ("tex",
                               "  texel(%u,%u)=#%02x",
                               x, y, p[0]);
        }
    }

  vkUnmapMemory (image->device->device, image->memories[0]);
}

static void
log_attr_samples_and_ndc (CoglAttribute        **attributes,
                          int                    n_attributes,
                          int                    first,
                          int                    count,
                          const CoglVkVertPush  *vert,
                          int                    fb_w,
                          int                    fb_h)
{
  int i;

  for (i = 0; i < n_attributes; i++)
    {
      CoglAttribute *a = attributes[i];
      const char *name;

      if (!a || !a->name_state)
        continue;
      name = a->name_state->name ? a->name_state->name : "?";
      /* Never map attribute buffers here: draw_common may already be
       * recording a render-pass CB. Mapping (esp. DEVICE_LOCAL oneshot
       * copies) during an open CB caused device-lost on the following
       * vkCmdBindVertexBuffers (custom-attributes + COGL_VK_DEBUG=vbo).
       */
      cogl_vk_debug_log ("vbo",
                         "  attr[%d] %s comps=%d type=%d norm=%d stride=%zu "
                         "offset=%zu (skip map during RP)",
                         i, name, a->n_components, (int) a->type,
                         a->normalized, (size_t) a->stride, (size_t) a->offset);
    }
}

void
cogl_vk_debug_vbo_draw (CoglVkPass              *pass,
                        CoglFramebuffer         *fb,
                        CoglPipeline            *pipeline,
                        CoglVerticesMode         mode,
                        int                      first,
                        int                      count,
                        gboolean                 opaque,
                        float                    alpha,
                        gboolean                 textured,
                        gboolean                 use_cold,
                        gboolean                 dummy_white,
                        CoglVkImage             *src,
                        CoglVkImage             *dst,
                        const CoglVkVertPush    *vert,
                        CoglAttribute          **attributes,
                        int                      n_attributes,
                        unsigned int             has_color,
                        unsigned int             has_texcoord,
                        unsigned int             color_type,
                        unsigned int             color_components,
                        gboolean                 normalized_color,
                        VkPipeline               pipe,
                        VkDescriptorSet          ds,
                        VkCullModeFlags          cull_mode,
                        VkFrontFace              front_face,
                        VkPrimitiveTopology      topology)
{
  CoglColor pipe_color;
  float pr, pg, pb, pa;
  int fb_w = 0, fb_h = 0;
  gboolean bake_flip = FALSE;
  gboolean y_flip = FALSE;

  if (!cogl_vk_debug_enabled ("vbo"))
    return;

  if (fb)
    {
      fb_w = cogl_framebuffer_get_width (fb);
      fb_h = cogl_framebuffer_get_height (fb);
      y_flip = cogl_framebuffer_is_y_flipped (fb);
      bake_flip = cogl_vk_should_bake_y_flip_projection (fb, pipeline);
    }

  if (pipeline)
    cogl_pipeline_get_color (pipeline, &pipe_color);
  else
    memset (&pipe_color, 0, sizeof (pipe_color));

  pr = cogl_color_get_red (&pipe_color);
  pg = cogl_color_get_green (&pipe_color);
  pb = cogl_color_get_blue (&pipe_color);
  pa = cogl_color_get_alpha (&pipe_color);

  cogl_vk_debug_log (
    "vbo",
    "DRAW mode=%s topo=%s first=%d count=%d opaque=%d alpha=%.4f tex=%d cold=%d "
    "dummy_white=%d has_color=%d has_texcoord=%d color_type=%u comps=%u norm=%d "
    "cull=0x%x front=%s pipe_color=(%.3f %.3f %.3f %.3f) pipe=%p ds=%p",
    mode_name (mode), topology_name (topology), first, count, opaque, alpha,
    textured, use_cold, dummy_white, has_color, has_texcoord, color_type,
    color_components, normalized_color, (unsigned) cull_mode,
    front_face == VK_FRONT_FACE_CLOCKWISE ? "CW" : "CCW",
    pr, pg, pb, pa, (void *) pipe, (void *) ds);

  if (pipeline)
    {
      CoglDepthState depth_state;

      cogl_pipeline_get_depth_state (pipeline, &depth_state);
      cogl_vk_debug_log (
        "vbo",
        "  depth test=%d write=%d func=%d range=[%.3f,%.3f]",
        depth_state.test_enabled, depth_state.write_enabled,
        (int) depth_state.test_function,
        depth_state.range_near, depth_state.range_far);
    }

  cogl_vk_debug_log (
    "vbo",
    "  fb=%dx%d y_flip=%d bake_flip=%d dst=%p %ux%u layout=%d "
    "src=%p %ux%u layout=%d host_vis=%d fmt=%d cogl_fmt=%d "
    "stencil_clip=%d in_rp=%d",
    fb_w, fb_h, y_flip, bake_flip,
    (void *) dst, dst ? dst->width : 0, dst ? dst->height : 0,
    dst ? (int) dst->layout : -1,
    (void *) src, src ? src->width : 0, src ? src->height : 0,
    src ? (int) src->layout : -1,
    src ? src->host_visible : 0,
    src ? (int) src->format : -1,
    src ? (int) src->cogl_format : -1,
    pass ? pass->stencil_clip_active : 0,
    pass ? pass->in_render_pass : 0);

  /* Hypotheses for pixel color (hot VBO_FRAG_TEX = tex * v_color * alpha). */
  cogl_vk_debug_log (
    "vbo",
    "  expect_if_pipe_as_vcolor=#%02x%02x%02x%02x "
    "has_color=%u → color_mul from %s",
    (unsigned) (pr * 255.f + 0.5f),
    (unsigned) (pg * 255.f + 0.5f),
    (unsigned) (pb * 255.f + 0.5f),
    (unsigned) (pa * alpha * 255.f + 0.5f),
    has_color,
    has_color ? "vertex (mul=1,1,1,a)" : "pipeline color");

  if (pass && pass->viewport_valid)
    cogl_vk_debug_log ("vbo",
                       "  viewport=(%.1f,%.1f %.1fx%.1f) depth=[%.2f,%.2f]",
                       pass->viewport.x, pass->viewport.y,
                       pass->viewport.width, pass->viewport.height,
                       pass->viewport.minDepth, pass->viewport.maxDepth);
  if (pass && pass->scissor_valid)
    cogl_vk_debug_log ("vbo",
                       "  scissor=(%d,%d %ux%u)",
                       pass->scissor.offset.x, pass->scissor.offset.y,
                       pass->scissor.extent.width, pass->scissor.extent.height);

  if (vert && cogl_vk_debug_enabled ("mvp"))
    {
      cogl_vk_debug_log (
        "mvp",
        "  push.mat4 row0=[%.6g %.6g %.6g %.6g] row1=[%.6g %.6g %.6g %.6g] "
        "row3=[%.6g %.6g %.6g %.6g]",
        vert->mat4[0][0], vert->mat4[0][1], vert->mat4[0][2], vert->mat4[0][3],
        vert->mat4[1][0], vert->mat4[1][1], vert->mat4[1][2], vert->mat4[1][3],
        vert->mat4[3][0], vert->mat4[3][1], vert->mat4[3][2], vert->mat4[3][3]);
    }

  if (src)
    cogl_vk_debug_image_texels ("src", src);

  log_attr_samples_and_ndc (attributes, n_attributes, first, count, vert,
                            fb_w, fb_h);
}

void
cogl_vk_debug_barrier (const char   *tag,
                       CoglVkImage  *image,
                       VkImageLayout old_layout,
                       VkImageLayout new_layout,
                       gboolean      used_host_src)
{
  if (!cogl_vk_debug_enabled ("barrier") || !image)
    return;
  cogl_vk_debug_log ("barrier",
                     "%s img=%p %ux%u host_vis=%d dmabuf=%d %d→%d host_src=%d",
                     tag ? tag : "barrier",
                     (void *) image, image->width, image->height,
                     image->host_visible, image->is_dmabuf,
                     (int) old_layout, (int) new_layout, used_host_src);
}

void
cogl_vk_debug_readback (CoglFramebuffer *fb,
                        int              x,
                        int              y,
                        int              w,
                        int              h,
                        const uint8_t   *rgba,
                        int              rowstride)
{
  int i;
  int n;

  if (!cogl_vk_debug_enabled ("readback") || !rgba)
    return;

  cogl_vk_debug_log ("readback",
                     "fb=%p y_flip=%d rect=(%d,%d %dx%d) stride=%d",
                     (void *) fb,
                     fb ? cogl_framebuffer_is_y_flipped (fb) : 0,
                     x, y, w, h, rowstride);

  n = MIN (w * h, 8);
  for (i = 0; i < n; i++)
    {
      const uint8_t *p = rgba + (size_t) i * 4;
      int px = x + (i % MAX (w, 1));
      int py = y + (i / MAX (w, 1));
      cogl_vk_debug_log ("readback",
                         "  (%d,%d) = #%02x%02x%02x%02x",
                         px, py, p[0], p[1], p[2], p[3]);
    }

  /* When sampling 1×1 (conform pixel checks), also note expected neighbors
   * via a second peek only if caller already provided a larger rect.
   */
  if (w == 1 && h == 1)
    {
      cogl_vk_debug_log ("readback",
                         "  note: 1x1 sample — check prior DRAW bind/color_mul "
                         "if unexpected black");
    }
}

CoglVkProfileScope
cogl_vk_profile_begin (CoglVkProfileStats *stats)
{
  static gsize initialized;
  static gboolean enabled;
  if (g_once_init_enter (&initialized))
    {
      enabled = g_strcmp0 (g_getenv ("COGL_VK_PROFILE"), "1") == 0;
      g_once_init_leave (&initialized, 1);
    }
  if (!enabled)
    return (CoglVkProfileScope) { 0 };
  return (CoglVkProfileScope) { stats, g_get_monotonic_time () };
}

void
cogl_vk_profile_end (CoglVkProfileScope *scope)
{
  CoglVkProfileStats *stats = scope->stats;
  int64_t now, elapsed;
  if (!stats)
    return;
  now = g_get_monotonic_time ();
  elapsed = now - scope->start;
  if (!stats->window_start)
    stats->window_start = scope->start;
  stats->total_us += elapsed;
  stats->max_us = MAX (stats->max_us, elapsed);
  stats->count++;
  if (now - stats->window_start >= G_USEC_PER_SEC)
    {
      g_printerr ("COGLVKPROFILE %s count=%" G_GUINT64_FORMAT
                  " us=%" G_GINT64_FORMAT " max_us=%" G_GINT64_FORMAT "\n",
                  stats->name, stats->count, stats->total_us, stats->max_us);
      stats->window_start = now;
      stats->count = 0;
      stats->total_us = stats->max_us = 0;
    }
}
