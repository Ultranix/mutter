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

/* Match the row-major matrix layout in CoglVkVertPush. */
layout(push_constant, row_major) uniform UBO {
	mat4 proj;
	vec2 uv_offset;
	vec2 uv_size;
} data;

layout(location = 0) out vec2 uv;

/* Unit-quad vertex positions. */
const vec2[] values = {
	{0, 0},
	{1, 0},
	{1, 1},
	{0, 1},
};

void main() {
	vec2 pos = values[gl_VertexIndex % 4];
	uv = data.uv_offset + pos * data.uv_size;
	vec4 clip = data.proj * vec4(pos, 0.0, 1.0);
	clip.z = clip.z * 0.5 + clip.w * 0.5;
	gl_Position = clip;
}
