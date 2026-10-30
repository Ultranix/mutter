# Experimental Vulkan renderer

The Vulkan Cogl driver is optional and leaves the default GL renderer enabled.
Configure it with Vulkan headers and shaderc installed:

```sh
meson setup build -Dvulkan=enabled
meson compile -C build
```

Select it when starting a Mutter Wayland compositor:

```sh
COGL_DRIVER=vulkan build/src/mutter --wayland
```

The native backend allocates GBM buffers and imports them into Vulkan. It
filters output modifiers against Vulkan import capabilities and retries linear
layouts before reducing output precision. EGL remains in use for device and
buffer interoperability. Secondary GPU accelerated copies use their own GLES3
context and dispatch table.

Wayland DMA-BUF YUV buffers retain separate plane textures. Mutter's existing
sampling snippets apply the surface's color representation. The low-level NV12
API also accepts explicit matrix and range parameters; its compatibility
constructor defaults to BT.709 narrow range. Acquire sync files are attached to
all imported planes. Rendering submissions export release sync files, with a
completion wait when export is unavailable.

## Tests

Enable Cogl, Clutter and Mutter tests as described in the test documentation.
The existing Cogl conformance cases run with GL3, GLES2 and Vulkan. Additional
Vulkan cases cover descriptor pool retirement, sparse vertex attributes,
separate depth/stencil binding, offscreen operations and NV12 color parameters.
Mocked unit tests exercise production submission, fence, device, uniform and
DMA-BUF code. A software Vulkan test also checks validation and readback across
512 submissions.

```sh
meson test -C build --suite cogl/conform/vulkan
meson test -C build --suite cogl/vulkan/unit
COGL_DRIVER=vulkan meson test -C build --suite clutter
```

For Vulkan validation, install the Khronos validation layer and set
`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` and
`VK_LAYER_VALIDATE_SYNC=1`. The software submission test explicitly enables
validation and checks its diagnostics. Set the Mesa lavapipe ICD through
`VK_ICD_FILENAMES` to run without a hardware Vulkan device.

## Review and hardware validation

This backend is experimental. Software tests do not establish hardware KMS
presentation, HDR output, multi-GPU modifier compatibility or timing behavior.
Those paths require testing on supported physical devices. Device loss is
reported through Cogl reset status; the device is not reconstructed in place.
The native output path requires importable GBM buffers and has no EGLSurface
rendering fallback for Vulkan. This work does not change direct scanout policy,
frame-clock scheduling or the default GL renderer.

Shader sources adapted from wlroots retain the MIT license notice in
`cogl/cogl/driver/vk/LICENSE.wlroots`.
The corresponding embedded SPIR-V headers retain the same license and
original copyright notices. Vulkan driver and test additions use
`GPL-2.0-or-later`, consistent with Mutter.
