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

#include "cogl/driver/vk/cogl-context-vk-private.h"

#include <gio/gio.h>

#include "cogl/cogl-context-private.h"
#include "cogl/cogl-pipeline-private.h"
#include "cogl/cogl-renderer-private.h"
#include "cogl/cogl-texture-2d-private.h"

struct _CoglContextVk
{
  CoglContext parent_instance;
};

static gboolean
cogl_context_vk_initable_init (GInitable     *initable,
                               GCancellable  *cancellable,
                               GError       **error);

static void
cogl_context_vk_initable_iface_init (GInitableIface *iface)
{
  iface->init = cogl_context_vk_initable_init;
}

G_DEFINE_TYPE_WITH_CODE (CoglContextVk, cogl_context_vk, COGL_TYPE_CONTEXT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                cogl_context_vk_initable_iface_init))

static gboolean
cogl_context_vk_initable_init (GInitable     *initable,
                               GCancellable  *cancellable,
                               GError       **error)
{
  CoglContext *context = COGL_CONTEXT (initable);
  CoglRenderer *renderer = cogl_context_get_renderer (context);
  uint8_t white_pixel[] = { 0xff, 0xff, 0xff, 0xff };
  GInitableIface *parent_iface =
    g_type_interface_peek (cogl_context_vk_parent_class, G_TYPE_INITABLE);

  if (!parent_iface->init (initable, cancellable, error))
    return FALSE;

  cogl_context_set_winsys_feature (context,
                                   COGL_WINSYS_FEATURE_SYNC_FD,
                                   TRUE);
  cogl_context_set_winsys_feature (context,
                                   COGL_WINSYS_FEATURE_BUFFER_AGE,
                                   TRUE);
  cogl_context_set_winsys_feature (context,
                                   COGL_WINSYS_FEATURE_SWAP_REGION,
                                   TRUE);

  cogl_renderer_set_feature (renderer, COGL_RENDERER_FEATURE_DMA_BUF_MODIFIERS,
                             TRUE);

  _cogl_pipeline_init_default_layers (context);

  {
    g_autoptr (CoglTexture) default_texture =
      cogl_texture_2d_new_from_data (context,
                                     1, 1,
                                     COGL_PIXEL_FORMAT_RGBA_8888_PRE,
                                     0,
                                     white_pixel,
                                     error);
    if (!default_texture)
      return FALSE;

    cogl_context_set_default_2d_texture (context,
                                         g_steal_pointer (&default_texture));
  }

  return TRUE;
}

static void
cogl_context_vk_init (CoglContextVk *context_vk)
{
}

static void
cogl_context_vk_class_init (CoglContextVkClass *class)
{
}
