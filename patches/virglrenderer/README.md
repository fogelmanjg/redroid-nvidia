# virglrenderer — Tier 5 (build environment + a confirmed negative result)

Base: real upstream `gitlab.freedesktop.org/virgl/virglrenderer`, commit in [`BASE`](BASE) — the
*exact* commit `waydroid-nvidia`'s own virglrenderer patches target (unlike Mesa's drifted AOSP
mirror, this one applies clean with plain `git am`).

## Build environment

```sh
git clone https://gitlab.freedesktop.org/virgl/virglrenderer.git
cd virglrenderer && git checkout $(cat BASE)

# waydroid-nvidia's own patches (fetch from Shiro836/waydroid-nvidia's patches/virglrenderer/):
git am 0001-vtest-support-exporting-sync_file-fds-for-venus-sync.patch \
       0002-vtest-support-importing-dmabufs-as-blob-resources-fo.patch \
       0003-vtest-raise-listen-backlog-to-128.patch
git apply 0004-wip-gpu-alloc-and-global-priority.patch
# plus the two new files that patch's meson.build change references
# (from Shiro836/waydroid-nvidia's src/virglrenderer-vtest/):
#   vtest/vtest_gpu_alloc.c
#   vtest/vtest_gpu_alloc.h

meson setup build -Dvenus=true -Dunstable-apis=true   # needs libepoxy-dev
ninja -C build
```

Produces a real, from-source `virgl_test_server` / `virgl_render_server` / `libvirglrenderer.so.1`
— confirmed as a working drop-in replacement for `waydroid-nvidia`'s prebuilt `v0.1.2` release
(same `list_exts`/`vnprobe` connectivity, same redroid boot behavior when unmodified). This was
the missing build environment before this entry — Mesa's already existed
([`../mesa/`](../mesa/)), virglrenderer's didn't.

## What's here: a confirmed negative result, not a working patch

While chasing Tier 5's screencap corruption (see the main `DEVLOG.md`'s 2026-09-24/25 entries),
reading this file surfaced something worth trying: `vtest_gpu_alloc.c` already has a complete,
working real-Vulkan implementation of a linear, host-visible, **renderable** image
(`vtest_gpu_alloc_image()` called with `linear=true`) — explicit `DRM_FORMAT_MOD_LINEAR`
modifier, `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT`, `HOST_VISIBLE|HOST_CACHED` memory, a real
`vkGetMemoryFdKHR` export, and `rowPitch` read back from the driver's own
`vkGetImageSubresourceLayout()` instead of hand-computed. But `vtest_gpu_alloc_cpu()` — the
function actually used for every `MAPPABLE` (CPU-visible) buffer, screenshot destinations
included — doesn't call it; it uses a separate, much simpler memfd + `/dev/udmabuf` path instead,
under the original author's own comment: *"experiment control: udmabuf-first (NVIDIA-linear path
suspected of breaking hwcomposer's own SW buffers)"*.

**`vtest_gpu_alloc.c-tried-and-reverted`** is this project's own attempt at rewiring
`vtest_gpu_alloc_cpu()` to call the existing `vtest_gpu_alloc_image(..., true, ...)` path instead
— a small, surgical change reusing code that was already there and already built cleanly. Tested
directly on real NVIDIA hardware. **Result: confirmed the original author's own suspicion,
directly and reproducibly.** The host's `virgl_render_server` log filled with `vkQueueSubmit
resulted in CS error` / `ring_submit_cmd: vn_dispatch_command failed` on every single
`surfaceflinger` connection attempt, tearing down the Vulkan context each time and eventually
producing a flat, completely blank white screen — no corruption pattern, but also zero legible
content, strictly worse than the `OPTIMAL`-tiling baseline this project already has. Reverted
cleanly (checksums confirmed against pre-experiment original binaries).

**Do not apply this file as-is** — it's kept as a documented, tested dead end, not a patch to
carry forward. It rules out *any* fix that makes the CPU-mappable buffer a real, directly
GPU-renderable Vulkan image on this stack, whether via a guest-side tiling-mode switch (see
[`patches/skia/`](../skia/), also reverted) or via this fully-correct, driver-native host-side
linear allocation. See `DEVLOG.md`'s 2026-09-25 entry for what's actually left to try next
(a genuinely separate second buffer with an explicit `vkCmdCopyImage`/`vkCmdBlitImage` untiling
step, rather than one buffer serving both the GPU-render and CPU-read roles).
