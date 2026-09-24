# ANGLE patch — Tier 5 (negative result, documented in place)

Base: AOSP `platform/external/angle` mirror, commit in [`BASE`](BASE).

## What this is

Same story as [`patches/skia/`](../skia/): `VK_IMAGE_TILING_LINEAR` for CPU-accessed
`AHardwareBuffer` imports was tried in ANGLE's equivalent, identically-hardcoded
`AhbDescUsageToVkImageTiling` (in `HardwareBufferImageSiblingVkAndroid.cpp`) and reverted after
the same fix, in Skia's own separate copy of this pattern, was proven to make things worse on
real NVIDIA/Venus hardware. This ANGLE path turned out not to even be the one
`SurfaceFlinger`'s `RenderEngine` uses (`RenderEngine` calls into Skia's `GaneshVkRenderEngine`
directly, not through ANGLE - ANGLE is the GLES-over-Vulkan layer used by *apps*), so the change
here was never actually exercised by the Tier 4/5 investigation - it's reverted along with the
Skia one on the reasoning that the same NVIDIA/Venus driver limitation would very likely apply
here too for any app that does hit this path.

What's committed here is the same kind of **explanatory comment left in place**, marking the file
and function for anyone who later needs to fix this for a real GLES app hitting the same
AHardwareBuffer+CPU-access combination through ANGLE specifically.

## Apply

```sh
git apply 0001-document-optimal-tiling-negative-result.patch
```
from an `external/angle` checkout at `BASE`.
