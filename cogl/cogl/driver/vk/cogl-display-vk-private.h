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

#include "cogl/cogl-display.h"

#define COGL_TYPE_DISPLAY_VK (cogl_display_vk_get_type ())

G_DECLARE_FINAL_TYPE (CoglDisplayVk,
                      cogl_display_vk,
                      COGL,
                      DISPLAY_VK,
                      CoglDisplay)
