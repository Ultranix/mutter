# Experimental Vulkan renderer

## Image lifetime and memory reuse

The driver's strong-reference DMA-BUF memo is disabled. Wayland buffers and
output framebuffers own their imports, and final disposal still waits for GPU
completion. The memo helpers remain available to existing unit tests.

Private, single-sample optimal images can reuse backing `VkDeviceMemory` from
an exact-size, exact-memory-type pool capped at **64 MiB and eight entries**.
Old images and views are destroyed after GPU completion; only backing memory
is reused. Imported, mapped, multisample and other allocation paths keep their
existing direct-release behavior. This budget covers the idle pool, not live
textures or internal driver allocations.

A 100 ms source on the owning main context drains pending submissions even
when no new frames arrive. Pooled allocations expire after two seconds idle,
and the source removes itself when no pending or cached work remains. Memory
warnings purge available entries on that context. Allocation failure clears
the pool and retries once. Teardown stops collection and releases the pool
before destroying the Vulkan device.

### Validation

On Intel i915 at 1280×720, six fresh GTK4 GL client windows (1100×650 requested)
were opened and closed, followed by three seconds idle. The old Vulkan render
client retained 73632 KiB resident, including 24 MiB shared. The updated path
retained 49008 KiB resident and zero shared; the separate display client was
3288 KiB in both cases. GLES2 retained 19484 KiB resident and zero shared.
These are isolated compositor measurements, not full GNOME Shell session totals.

All 47 Vulkan conformance tests passed. Pool tests cover entry/byte limits,
reuse, memory-warning purge and idle expiry on a real Vulkan device. The
low-level test also passed with Vulkan synchronization validation enabled.

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
