# Mutter with an experimental Vulkan driver

This repository is a Mutter tree that adds an **optional Cogl Vulkan driver**
for the Wayland compositor. The default OpenGL (GL3 / GLES2) path stays
unchanged. Select Vulkan at runtime with `COGL_DRIVER=vulkan`.

Upstream Mutter is a Wayland display server on top of KMS and libinput. It
implements the compositor side of the Wayland core protocol and related
extensions, window management, compositing, focus tracking, workspaces,
keybindings and monitor configuration. Internally it uses forks of Cogl and
Clutter. This fork keeps that stack and adds a Vulkan rendering backend under
`cogl/cogl/driver/vk/`.

Related upstream work item:
[GNOME/mutter#2606](https://gitlab.gnome.org/GNOME/mutter/-/work_items/2606).

More detail: [doc/vulkan.md](doc/vulkan.md).

## Fork releases

Release **1.1** integrates upstream `main` through `b766717ac9` and the
subsequent Vulkan resource lifetime and rendering changes. Release **1.0**
preserves the previous GitHub `main` at `d7fbd2fc953b`. These are fork release
numbers; Mutter's upstream version and library ABI keep their upstream names.

Changes in 1.1 include deferred destruction of GPU resources, asynchronous
texture uploads and attachment initialization, texture copy-on-write without
an intermediate full-image copy, mapped buffer storage, bounded vertex
snapshot reuse, shader fingerprint reuse, and fewer render pass breaks for
textures already synchronized for sampling. The Vulkan driver also persists
its pipeline cache, requests a high-priority graphics queue when permitted,
and provides optional GPU timing diagnostics through `COGL_VK_GPU_TRACE`.

Related fixes cover YUV shader coefficients, sparse texture matrix uniforms,
drag icons containing subsurfaces, and Xwayland pointer selection for window
move/resize requests. The existing backend-independent rendering interfaces
are retained. See [CHANGELOG.md](CHANGELOG.md) for the release history.

## Quick start

```sh
# Vulkan headers and shaderc required
meson setup build -Dvulkan=enabled
meson compile -C build

COGL_DRIVER=vulkan build/src/mutter --wayland
```

Leave `-Dvulkan` at the default (`disabled`) for a normal GL-only build.
`COGL_DRIVER` accepts `gl3`, `gles2` or `vulkan`.

## GNOME Shell integration

GNOME Shell integration is implemented. GNOME Shell has been built and run
with the Vulkan backend on Ubuntu, including overview and Show Apps
animations, window operations, and Wayland clients. GNOME Shell embeds Mutter
and uses its Cogl and Clutter libraries; select the backend on the
`gnome-shell` process with `COGL_DRIVER=vulkan`.

Build and install this Mutter first, then build GNOME Shell against the
installed libraries and GObject introspection data. Both projects must use
the same Mutter API version. The current tree uses **API 52**, matching
GNOME Shell `main` at the time of this update. The earlier Ubuntu deployment
used GNOME Shell 51.rc with the corresponding API 51 Mutter build.

Install the development dependencies for both projects, including Meson,
Ninja, pkg-config, Vulkan headers, shaderc, GObject introspection, GJS, GTK,
and the GNOME libraries required by their Meson files. The distribution's
Mutter and GNOME Shell build dependencies provide a starting point; the
development branches may require newer versions.

### Build Mutter with Vulkan and introspection

Run these commands from a parent directory containing the source checkouts:

```sh
git clone --branch main https://github.com/Ultranix/mutter.git mutter-vulkan
git clone --branch main https://gitlab.gnome.org/GNOME/gnome-shell.git

export MUTTER_VK_PREFIX=/opt/mutter-vulkan
export MUTTER_VK_API=52

meson setup mutter-vulkan/build-shell mutter-vulkan \
  --prefix="$MUTTER_VK_PREFIX" --libdir=lib \
  --buildtype=debugoptimized \
  -Dvulkan=enabled -Dintrospection=true -Dfonts=true \
  -Dtests=disabled -Ddocs=false
meson compile -C mutter-vulkan/build-shell
sudo meson install -C mutter-vulkan/build-shell --no-rebuild
```

### Build GNOME Shell against the installed Mutter

Keep these variables set during configuration, compilation and execution:

```sh
export PKG_CONFIG_PATH="$MUTTER_VK_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$MUTTER_VK_PREFIX/lib:$MUTTER_VK_PREFIX/lib/mutter-$MUTTER_VK_API:$MUTTER_VK_PREFIX/lib/gnome-shell${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export GI_TYPELIB_PATH="$MUTTER_VK_PREFIX/lib/mutter-$MUTTER_VK_API:$MUTTER_VK_PREFIX/lib/gnome-shell:$MUTTER_VK_PREFIX/lib/girepository-1.0${GI_TYPELIB_PATH:+:$GI_TYPELIB_PATH}"
export XDG_DATA_DIRS="$MUTTER_VK_PREFIX/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"

pkg-config --modversion "libmutter-$MUTTER_VK_API"
pkg-config --variable=prefix "libmutter-$MUTTER_VK_API"

meson setup gnome-shell/build-vulkan gnome-shell \
  --prefix="$MUTTER_VK_PREFIX" --libdir=lib \
  --buildtype=debugoptimized -Dtests=false -Dgtk_doc=false -Dman=false
meson compile -C gnome-shell/build-vulkan
sudo meson install -C gnome-shell/build-vulkan --no-rebuild
sudo glib-compile-schemas "$MUTTER_VK_PREFIX/share/glib-2.0/schemas"
```

The printed pkg-config prefix must be `/opt/mutter-vulkan`. Check that
`libmutter_api_version` in Mutter's `meson.build` and `mutter_api_version` in
GNOME Shell's `meson.build` agree before building. Introspection must remain
enabled because Shell's JavaScript code imports Meta, Clutter and Cogl.

### Run GNOME Shell with Vulkan

From a separate local TTY, with the variables above exported:

```sh
export GSETTINGS_SCHEMA_DIR="$MUTTER_VK_PREFIX/share/glib-2.0/schemas"
dbus-run-session -- env COGL_DRIVER=vulkan \
  "$MUTTER_VK_PREFIX/bin/gnome-shell" --wayland
```

A GDM session launcher should export the same library, introspection, data
and schema paths, set `COGL_DRIVER=vulkan`, and execute the installed
`gnome-shell` binary.

## Vulkan driver features

### Cogl Vulkan backend (`cogl/cogl/driver/vk/`)

- Optional Cogl driver beside GL3 and GLES2; same Cogl / Clutter API surface.
- Device, queue, memory and resource management for textures, buffers, images
  and framebuffers.
- Pipeline and shader path: GLSL snippets translated for Vulkan, SPIR-V
  embedding (wlroots-adapted shaders keep their MIT notice), descriptor sets,
  sampler objects and pipeline cache keyed by full sparse state.
- Rendering: clipping, scissor, clears, offscreen targets, blit, readback with
  write-only bitmap access and row padding, atlas allocation bounded by Vulkan
  image limits.
- Texture formats: BGRA8888, RG, half-float, RGBA1010102, NORM16, swizzle,
  packed depth/stencil, sized RGBA, alpha textures, LOD bias, max level.
- Buffer mapping for read and write, PBOs, unsigned int indices, framebuffer
  bit queries, Mesa pack invert.
- Independent depth and stencil binding semantics (shared storage, separate
  aspects), including 1× and 4× MSAA.
- Timestamp queries when the device reports a period.
- Device loss reported through Cogl reset status (no in-place reconstruction).
- Rendering path separated from EGL so Vulkan does not depend on an EGLSurface
  for drawing.

### Native KMS / GBM output

- Native backend allocates GBM buffers and imports them into Vulkan.
- Output modifiers filtered against real Vulkan import capabilities.
- Prefers higher bit depth (10 bpc), retries linear layouts on allocation
  failure, then falls back in precision (for example to 8 bpc).
- Buffer-age tracking retained; failed frames do not advance front/age and
  request a full redraw.
- No EGLSurface rendering fallback on the Vulkan native path; importable GBM
  buffers are required.
- Direct scanout policy and frame-clock scheduling are unchanged.

### Wayland DMA-BUF and synchronization

- Wayland DMA-BUF import for packed, semi-planar and planar YUV (8/10/12/16-bit)
  through the shared format table, plus R16, RG16, RGB565, RGBA16 and FP16.
- NV12 and other YUV planes kept as separate textures; Mutter color
  representation snippets apply BT.601 / BT.709 / BT.2020 full and narrow range.
- Low-level NV12 / EGLImage APIs accept explicit matrix and range; the
  compatibility constructor defaults to BT.709 narrow range and can switch
  colorimetry dynamically.
- Acquire sync files attached to every imported plane; submissions export
  release sync files, with a completion wait when export is unavailable.
- Format / modifier advertisements filtered by sample, transfer and attachment
  usage. Known Intel CCS painting issues remain gated.
- DMA-BUF identity cache includes all planes and colorimetry parameters (not
  only inode), so Y and UV from the same object stay distinct.

### Multi-GPU and secondary copy

- When the primary GPU runs Vulkan, secondary-GPU accelerated copies can use an
  independent GLES3 context and dispatch table.
- Sync FDs from the primary GPU are waited on by the secondary EGL path.
- Copy or fence failure clears buffer-age history instead of showing a stale
  frame. This is not a full Vulkan secondary renderer.

### Robustness and resource lifetime

- Failed Vulkan submissions roll back recorded image layout / foreign ownership
  and do not advance unused timeline points.
- Descriptor pools retire safely; error paths wait for old work before recycle.
- Xwayland client decoration teardown frees Vulkan images, views and memory.
- Shadow noise texture cache is tied to the Cogl context lifetime.
- HDR mastering peak from the client is preserved (defaults no longer override
  it incorrectly).

### Architecture cleanup for dual backends

- `CoglRenderDevice` and `CoglRenderDeviceMode` own renderer lifetime.
- DMA-BUF format / modifier queries and related features live on CoglRenderer /
  CoglRenderDevice vfuncs instead of hard-coded EGL calls.
- Native backend looks up `MetaRenderDevice` instead of scattered GPU data;
  `create_egl_display` is replaced by `create_renderer`.

## Tests

```sh
meson test -C build --suite cogl/conform/vulkan
meson test -C build --suite cogl/vulkan/unit
COGL_DRIVER=vulkan meson test -C build --suite clutter
```

Coverage includes:

- Shared Cogl conformance on GL3, GLES2 and Vulkan (perspective, scissor,
  sparse attributes, YUV coefficients, descriptor retirement, offscreen,
  depth/stencil binding, NV12 color parameters).
- Mocked unit tests for device, submission, fences, uniforms, render passes and
  DMA-BUF.
- Software Vulkan (lavapipe) submission / readback stress (512 iterations) with
  Khronos validation and sync validation; intentional hazards are detected.
- Selected Mutter core / Wayland / stacking cases under each driver.

Validated in an Alpine aarch64 container with Mesa llvmpipe/lavapipe, and on
Intel Iris Xe (Raptor Lake-U) hardware with a full GL3 / GLES2 / Vulkan
rendering matrix. Physical KMS HDR, multi-GPU modifiers and high-refresh
interaction still need scene-specific hardware checks.

For validation layers:

```sh
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_VALIDATE_SYNC=1
# optional software ICD
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
```

## Branch layout

This GitHub repository is a fork of upstream Mutter. Work lives on `main`
only: the optional Vulkan Cogl driver, native/Wayland integration, tests and
docs are on that branch.

Upstream GNOME Mutter: [gitlab.gnome.org/GNOME/mutter](https://gitlab.gnome.org/GNOME/mutter)

## Documentation

- [Experimental Vulkan renderer](doc/vulkan.md)
- [Building and running](doc/building-and-running.md)
- [Coding style](doc/coding-style.md)
- [Code overview](doc/code-overview.md)
- [Debugging](doc/debugging.md)
- [Monitor configuration](doc/monitor-configuration.md)
- [Multi-GPU](doc/multi-gpu.md)

## API reference

- Meta: <https://mutter.gnome.org/meta/>
- Clutter: <https://mutter.gnome.org/clutter/>
- Cogl: <https://mutter.gnome.org/cogl/>
- Mtk: <https://mutter.gnome.org/mtk/>

## License

Mutter is distributed under the GNU General Public License, version 2 or later.
See [COPYING](COPYING). Vulkan driver and test additions use the same
`GPL-2.0-or-later` terms. Shader sources adapted from wlroots keep the MIT
notice in `cogl/cogl/driver/vk/LICENSE.wlroots`.
