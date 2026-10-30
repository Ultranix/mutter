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

#include "cogl/driver/vk/cogl-dmabuf-memo-vk-private.h"

#include <sys/stat.h>
#include <string.h>
#include <drm_fourcc.h>

typedef struct _CoglVkDmabufMemoKey
{
  uint64_t width, height, format, drm_fourcc, n_planes, ycbcr_index;
  struct { uint64_t dev, ino, stride, offset, modifier; } planes[4];
} CoglVkDmabufMemoKey;

struct _CoglVkDmabufMemo
{
  GHashTable *by_inode; /* CoglVkDmabufMemoKey* → CoglTexture* (full ref) */
};

static unsigned int
key_hash (gconstpointer key)
{
  const uint8_t *bytes = key;
  unsigned int hash = 2166136261u;

  for (size_t i = 0; i < sizeof (CoglVkDmabufMemoKey); i++)
    hash = (hash ^ bytes[i]) * 16777619u;
  return hash;
}

static gboolean
key_equal (gconstpointer a,
           gconstpointer b)
{
  const CoglVkDmabufMemoKey *ka = a;
  const CoglVkDmabufMemoKey *kb = b;
  return memcmp (ka, kb, sizeof (*ka)) == 0;
}

static gboolean
key_from_import (const CoglVkDmabufImport *import,
                 CoglVkDmabufMemoKey      *out)
{
  struct stat st;

  if (!import || import->n_planes < 1 || import->n_planes > 4 ||
      !import->fds || !import->strides || !import->offsets)
    return FALSE;
  memset (out, 0, sizeof (*out));
  out->width = import->width;
  out->height = import->height;
  out->format = import->format;
  out->drm_fourcc = import->drm_fourcc;
  out->n_planes = import->n_planes;
  out->ycbcr_index = import->ycbcr_index;
  for (int i = 0; i < import->n_planes; i++)
    {
      if (import->fds[i] < 0 || fstat (import->fds[i], &st) != 0)
        return FALSE;
      out->planes[i].dev = st.st_dev;
      out->planes[i].ino = st.st_ino;
      out->planes[i].stride = import->strides[i];
      out->planes[i].offset = import->offsets[i];
      out->planes[i].modifier = import->modifiers ? import->modifiers[i] :
                                DRM_FORMAT_MOD_INVALID;
    }
  return TRUE;
}

CoglVkDmabufMemo *
cogl_vk_dmabuf_memo_new (void)
{
  CoglVkDmabufMemo *memo = g_new0 (CoglVkDmabufMemo, 1);
  memo->by_inode = g_hash_table_new_full (key_hash, key_equal,
                                          g_free, g_object_unref);
  return memo;
}

void
cogl_vk_dmabuf_memo_free (CoglVkDmabufMemo *memo)
{
  if (!memo)
    return;
  g_clear_pointer (&memo->by_inode, g_hash_table_unref);
  g_free (memo);
}

CoglTexture *
cogl_vk_dmabuf_memo_lookup (CoglVkDmabufMemo         *memo,
                            const CoglVkDmabufImport *import)
{
  CoglVkDmabufMemoKey key;
  CoglTexture *tex;

  if (!memo || !key_from_import (import, &key))
    return NULL;

  tex = g_hash_table_lookup (memo->by_inode, &key);
  return tex ? g_object_ref (tex) : NULL;
}

void
cogl_vk_dmabuf_memo_store (CoglVkDmabufMemo         *memo,
                           const CoglVkDmabufImport *import,
                           CoglTexture              *texture)
{
  CoglVkDmabufMemoKey *key;

  if (!memo || !texture)
    return;

  key = g_new (CoglVkDmabufMemoKey, 1);
  if (!key_from_import (import, key))
    {
      g_free (key);
      return;
    }

  g_hash_table_insert (memo->by_inode, key, g_object_ref (texture));
}

void
cogl_vk_dmabuf_memo_remove_texture (CoglVkDmabufMemo *memo,
                                    CoglTexture      *texture)
{
  GHashTableIter iter;
  gpointer k, v;

  if (!memo || !texture)
    return;

  g_hash_table_iter_init (&iter, memo->by_inode);
  while (g_hash_table_iter_next (&iter, &k, &v))
    {
      if (v == texture)
        g_hash_table_iter_remove (&iter);
    }
}
