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

/* Command recording and submission for the Cogl Vulkan driver.
 * Each batch tracks image transitions, client acquire fences and resources
 * that must remain alive until the submission completes.
 */

#pragma once

#include "cogl/driver/vk/cogl-device-vk-private.h"
#include "cogl/driver/vk/cogl-fence-vk-private.h"
#include "cogl/driver/vk/cogl-image-vk-private.h"

G_BEGIN_DECLS

typedef struct _CoglVkPass CoglVkPass;

struct _CoglVkPass
{
  CoglVkDevice *device; /* unowned */
  VkCommandBuffer command_buffer;
  CoglVkFenceTicket *fence;
  gboolean submitted;
  GArray *image_states; /* Layout/ownership before recording, for abort. */
  gboolean recording;
  gboolean in_render_pass;
  CoglVkImage *render_color; /* unowned */
  CoglVkImage *render_depth_stencil; /* unowned; optional DS attachment */
  gpointer render_setup; /* CoglVkRenderFormatSetup *, unowned */
  /* Client sync waits — transferred to fence ticket on submit. */
  GArray *wait_semaphores; /* VkSemaphore */
  /* Imported images needing an ownership release before submission. */
  GPtrArray *foreign_images; /* CoglVkImage *, unowned */
  /* Scissor rectangle supplied by the Cogl clip stack. */
  VkRect2D scissor;
  gboolean scissor_valid;
  /* From flush_framebuffer_state VIEWPORT; else full FB extent. */
  VkViewport viewport;
  gboolean viewport_valid;
  /* Stencil clip active → EQUAL ref=1; else compareMask=0 (always pass). */
  gboolean stencil_clip_active;
  /* GPU timestamp query indices for this cmd buffer (-1 = not written). */
  int timestamp_begin;
  int timestamp_end;
};

CoglVkPass * cogl_vk_pass_begin (CoglVkDevice *device,
                                 GError      **error);

gboolean cogl_vk_pass_submit (CoglVkPass *pass,
                              GError    **error);

/* Import a temporary SYNC_FD semaphore payload. Takes the fd on success. */
gboolean cogl_vk_pass_add_wait_sync_fd (CoglVkPass *pass,
                                        int         sync_fd,
                                        GError    **error);

/* Track imported images whose ownership must be released at submission. */
gboolean cogl_vk_pass_has_foreign_image (CoglVkPass  *pass,
                                         CoglVkImage *image);
void cogl_vk_pass_note_foreign_image (CoglVkPass  *pass,
                                      CoglVkImage *image);

void cogl_vk_pass_note_image_state (CoglVkPass  *pass,
                                    CoglVkImage *image);

void cogl_vk_pass_free (CoglVkPass *pass);

G_END_DECLS
