# Changelog

Version numbers below identify releases of this fork, independently of
upstream Mutter versions and library ABI numbers.

## 1.1 — 2026-11-10

- Merge upstream main through `b766717ac9`, including runtime DRM device
  removal and the text-input panel fix.
- Retain the Cogl renderer, render device and framebuffer abstractions.
- Retire images, views, framebuffers, buffers and memory after the submissions
  that use them complete, including upload command buffers and staging storage.
- Queue texture uploads and internal attachment clears without immediate CPU
  fence waits, while preserving synchronization for external presentation.
- Copy unchanged texture contents directly into replacement storage during
  partial copy-on-write updates, avoiding an intermediate full-image snapshot.
- Keep host-visible buffer storage and vertex snapshot chunks mapped; protect
  recorded buffer readers before subsequent host writes.
- Restrict vertex snapshots to draw ranges and cache immutable snapshots with
  bounded accounting and weak source references.
- Reuse shader fingerprints across changes that do not alter generated code,
  while tracking shader-relevant layer indices and wrapping.
- Reuse render passes when sampled textures are already synchronized; restore
  sampling state when a submission fails.
- Persist the Vulkan pipeline cache and request high-priority graphics queues
  with a fallback when the driver rejects the priority request.
- Add optional calibrated GPU timing diagnostics.
- Correct YUV coefficients and sparse GL texture matrix uniform indexing.
- Handle drag icon subsurface visibility/order and freeze all icon surfaces
  when finishing a drag.
- Correct nearest-pointer selection for Xwayland move/resize requests.

## 1.0

Snapshot of the previous GitHub main at `d7fbd2fc953b`, including the optional
Cogl Vulkan driver, native KMS integration, Wayland DMA-BUF synchronization,
and rendering abstractions.
