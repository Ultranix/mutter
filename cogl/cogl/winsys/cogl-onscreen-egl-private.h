/*
 * Copyright (C) 2007,2008,2009,2010,2011,2013 Intel Corporation.
 * Copyright (C) 2020 Red Hat
 * Copyright (C) 2026 Ultranix
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "cogl/winsys/cogl-onscreen-egl.h"

COGL_EXPORT void
cogl_onscreen_egl_destroy_surface (CoglOnscreen *onscreen,
                                   EGLSurface    surface);

COGL_EXPORT void
cogl_onscreen_egl_bind_surface (CoglOnscreen *onscreen,
                                EGLSurface    surface,
                                EGLContext    egl_context);

COGL_EXPORT int
cogl_onscreen_egl_get_buffer_age_surface (CoglOnscreen *onscreen,
                                          EGLSurface    surface);

COGL_EXPORT void
cogl_onscreen_egl_queue_damage_region_surface (CoglOnscreen    *onscreen,
                                               EGLSurface       surface,
                                               const MtkRegion *region);

COGL_EXPORT gboolean
cogl_onscreen_egl_swap_region_surface (CoglOnscreen    *onscreen,
                                       EGLSurface       surface,
                                       const MtkRegion *region,
                                       CoglFrameInfo   *info,
                                       void            *user_data);

COGL_EXPORT gboolean
cogl_onscreen_egl_swap_buffers_with_damage_surface (CoglOnscreen    *onscreen,
                                                    EGLSurface       surface,
                                                    const MtkRegion *region,
                                                    CoglFrameInfo   *info,
                                                    void            *user_data);
