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

/* Texture import cache for Cogl DMA-BUF images.
 * Import identity includes every plane and its interpretation. In particular,
 * the Y and UV planes can share one inode while requiring different views.
 */

#pragma once

#include "cogl/cogl-texture.h"

#include <glib.h>

G_BEGIN_DECLS

typedef struct _CoglVkDmabufMemo CoglVkDmabufMemo;

typedef struct
{
  int width, height;
  CoglPixelFormat format;
  uint32_t drm_fourcc;
  int n_planes;
  const int *fds;
  const uint32_t *strides;
  const uint32_t *offsets;
  const uint64_t *modifiers;
  unsigned int ycbcr_index;
} CoglVkDmabufImport;

CoglVkDmabufMemo * cogl_vk_dmabuf_memo_new (void);

void cogl_vk_dmabuf_memo_free (CoglVkDmabufMemo *memo);

CoglTexture * cogl_vk_dmabuf_memo_lookup (CoglVkDmabufMemo         *memo,
                                          const CoglVkDmabufImport *import);

void cogl_vk_dmabuf_memo_store (CoglVkDmabufMemo         *memo,
                                const CoglVkDmabufImport *import,
                                CoglTexture              *texture);

void cogl_vk_dmabuf_memo_remove_texture (CoglVkDmabufMemo *memo,
                                         CoglTexture      *texture);

G_END_DECLS
