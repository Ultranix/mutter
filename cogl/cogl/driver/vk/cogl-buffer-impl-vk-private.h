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

#include "cogl/cogl-buffer-impl-private.h"
#include <vulkan/vulkan.h>

G_DECLARE_FINAL_TYPE (CoglBufferImplVk,
                      cogl_buffer_impl_vk,
                      COGL,
                      BUFFER_IMPL_VK,
                      CoglBufferImpl)

#define COGL_TYPE_BUFFER_IMPL_VK (cogl_buffer_impl_vk_get_type ())

VkBuffer cogl_buffer_impl_vk_peek_buffer (CoglBufferImplVk *impl);

uint64_t cogl_buffer_impl_vk_get_content_generation (CoglBufferImplVk *impl);

gboolean cogl_buffer_impl_vk_ensure (CoglBufferImplVk *impl,
                                     CoglBuffer       *buffer,
                                     GError          **error);
