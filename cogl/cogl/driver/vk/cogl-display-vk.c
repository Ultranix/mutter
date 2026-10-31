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

#include "cogl/driver/vk/cogl-display-vk-private.h"

#include <gio/gio.h>

#include "cogl/driver/vk/cogl-context-vk-private.h"

struct _CoglDisplayVk
{
  CoglDisplay parent_instance;
};

G_DEFINE_TYPE (CoglDisplayVk, cogl_display_vk, COGL_TYPE_DISPLAY)

static gboolean
cogl_display_vk_setup (CoglDisplay  *display,
                       GError      **error)
{
  return TRUE;
}

static void
cogl_display_vk_destroy (CoglDisplay *display)
{
}

static CoglContext *
cogl_display_vk_create_context (CoglDisplay  *display,
                                GError      **error)
{
  return g_initable_new (COGL_TYPE_CONTEXT_VK, NULL, error,
                         "display", display,
                         NULL);
}

static void
cogl_display_vk_class_init (CoglDisplayVkClass *class)
{
  CoglDisplayClass *display_class = COGL_DISPLAY_CLASS (class);

  display_class->setup = cogl_display_vk_setup;
  display_class->destroy = cogl_display_vk_destroy;
  display_class->create_context = cogl_display_vk_create_context;
}

static void
cogl_display_vk_init (CoglDisplayVk *display_vk)
{
}
