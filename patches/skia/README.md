# Skia patch — Tier 5 (negative result, documented in place)

Base: AOSP `platform/external/skia` mirror, commit in [`BASE`](BASE).

## What this is

Not a functional fix — `VK_IMAGE_TILING_LINEAR` for CPU-accessed `AHardwareBuffer` imports was
tried here and empirically made Tier 5's screencap corruption *worse* on real NVIDIA/Venus
hardware (see the [main README](../../README.md)'s Tier 5 entry and `DEVLOG.md`'s 2026-09-24
entries for the full story: raw pixel dumps went from "real content in a structured 64-byte grid"
to "almost entirely zero, no legible content at all"). The functional change was reverted.

What's committed here is the **explanatory comment left in place** at
`src/gpu/ganesh/vk/AHardwareBufferVk.cpp`'s `make_vk_backend_texture`, next to the
`VK_IMAGE_TILING_OPTIMAL` hardcoding — pointing at exactly why this is the real root cause of the
Tier 5 corruption, and exactly why the obvious tiling-mode fix doesn't work on this stack. Applying
this patch changes no behavior; it's a marker for whoever picks up Tier 5 next, in the exact file
and function where the fix eventually needs to happen (an explicit untiling copy step, not a
tiling-mode switch — see DEVLOG).

## Apply

```sh
git apply 0001-document-optimal-tiling-negative-result.patch
```
from an `external/skia` checkout at `BASE`.
