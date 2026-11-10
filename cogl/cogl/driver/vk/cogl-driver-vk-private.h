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

#pragma once

#include "cogl/cogl-boxed-value.h"
#include "cogl/cogl-driver-private.h"
#include "cogl/cogl-sampler-cache-private.h"
#include "cogl/driver/vk/cogl-device-vk-private.h"
#include "cogl/driver/vk/cogl-pass-vk-private.h"
#include "cogl/driver/vk/cogl-pipeline-vk-private.h"
#include "cogl/driver/vk/cogl-dmabuf-memo-vk-private.h"

typedef struct _CoglAttribute CoglAttribute;

struct _CoglDriverVk
{
  CoglDriver parent_instance;
  CoglVkDevice *vk_device;
  CoglVkPipelineCache *pipeline_cache;
  CoglVkDmabufMemo *dmabuf_memo;
  CoglVkPass *current_pass;
  int latest_sync_fd;
  int pending_drm_fd;
  /* Active clip scissor, applied when rendering starts or the next draw runs.
   */
  VkRect2D pending_scissor;
  gboolean pending_scissor_valid;
  uint32_t pending_scissor_fb_height;
  gboolean pending_scissor_y_flipped;
  /* Viewport from flush_framebuffer_state; applied on begin_render/draw. */
  VkViewport pending_viewport;
  gboolean pending_viewport_valid;
  /* Front face / depth-write from FB flush (used when building PSOs). */
  VkFrontFace pending_front_face;
  gboolean pending_depth_write;
  /* Non-AABB / multi-rect clip → stencil test EQUAL on subsequent draws. */
  gboolean pending_stencil_clip;
  /* entry* → VkSampler (as gpointer); see sampler_init/free. */
  GHashTable *samplers;
  /* Guard re-entry from image_free during submit/free. */
  gboolean flushing_current_pass;
  gboolean submission_failed;
  /* location → CoglBoxedValue*; merged by pack_uniforms at draw. */
  GHashTable *pending_uniforms;
  /* Last flushed attributes (journal → draw); unowned pointers. */
  CoglAttribute **pending_attributes;
  int pending_n_attributes;
  /* Pipeline after layer overrides (fallback/disable/layer0). NULL when
   * flush left the caller pipeline unchanged. Owned iff
   * pending_pipeline_owned. Consumed (and unref'd) by draw_*.
   */
  CoglPipeline *pending_pipeline;
  gboolean pending_pipeline_owned;
  /* Preserve the last offscreen allocation flags, as the Cogl GL driver does.
   */
  gboolean have_last_offscreen_allocate_flags;
  unsigned int last_offscreen_allocate_flags;
};

G_DECLARE_FINAL_TYPE (CoglDriverVk,
                      cogl_driver_vk,
                      COGL,
                      DRIVER_VK,
                      CoglDriver)

#define COGL_TYPE_DRIVER_VK (cogl_driver_vk_get_type ())

COGL_EXPORT CoglVkDevice * cogl_driver_vk_peek_device (CoglDriverVk * driver);

COGL_EXPORT CoglVkPipelineCache * cogl_driver_vk_peek_pipeline_cache (CoglDriverVk *driver);

/* Per-entry sampler, or pipeline-cache default when entry unknown. */
VkSampler cogl_driver_vk_peek_sampler (CoglDriverVk                *driver,
                                       const CoglSamplerCacheEntry *entry);

void cogl_driver_vk_store_sync_fd_from_pass (CoglDriverVk *driver);

int cogl_driver_vk_get_latest_sync_fd (CoglDriverVk *driver);

/* Submit+free current_pass if any. Required before destroying VkImage /
 * VkImageView / buffers still referenced by a recording CB (WaitIdle alone
 * does not help — the CB was never submitted).
 */
void cogl_driver_vk_flush_current_pass (CoglDriverVk *driver);

typedef enum
{
  COGL_VK_RETIRE_UNAVAILABLE,
  COGL_VK_RETIRE_DEFERRED,
  COGL_VK_RETIRE_COMPLETE,
} CoglVkRetireResult;

/* A recording pass or the newest pending submit owns deferred resources.
 * COMPLETE proves that no recorded or submitted use remains; UNAVAILABLE
 * requires the caller to submit recorded work and wait before destruction. */
CoglVkRetireResult cogl_driver_vk_retire_list (CoglDriverVk      *driver,
                                               CoglVkRetireList **retire);

/* Overlay set_uniform pending values onto pack_uniforms name lookup. */
const CoglBoxedValue *
cogl_driver_vk_peek_pending_uniform (CoglDriverVk *driver,
                                     GLint         location);

gboolean cogl_driver_vk_initialize (CoglDriverVk  *driver_vk,
                                    CoglRenderer  *renderer,
                                    GError       **error);
