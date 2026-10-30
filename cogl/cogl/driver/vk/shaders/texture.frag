/*
 * Copyright (c) 2017, 2018 Drew DeVault
 * Copyright (c) 2014 Jari Vetoniemi
 * Copyright (c) 2026 Ultranix
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * SPDX-License-Identifier: MIT
 *
 * Derived from wlroots render/vulkan/shaders. See ../LICENSE.wlroots.
 */

/* Build embeds the SPIR-V generated from this shader. */
#version 450

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform UBO {
	layout(offset = 80) float alpha;
	/* 0 = clamp-to-edge, 1 = repeat, 2 = mirrored-repeat. */
	float wrap_s;
	float wrap_t;
} data;

float wrap_coord(float t, float mode)
{
	if (mode < 0.5)
		return clamp(t, 0.0, 0.999999);
	if (mode < 1.5)
		return t - floor(t);
	float n = floor(t);
	float f = t - n;
	if (mod(n, 2.0) >= 1.0)
		return 1.0 - f;
	return f;
}

void main() {
	vec2 u = vec2(wrap_coord(uv.x, data.wrap_s),
	              wrap_coord(uv.y, data.wrap_t));
	out_color = textureLod(tex, u, 0);
	out_color *= data.alpha;
}
