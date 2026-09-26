# virglrenderer — Tier 5 (build environment + a confirmed negative result) and Tier 7 (a new vtest command)

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

## Tier 7: `VCMD_ENCODE_RESOURCE` — a new vtest command for host-side NVENC encode

Adds one new custom vtest command, following the exact shape of `VCMD_RESOURCE_ALLOC_GPU`/
`VCMD_RESOURCE_EXPORT_FD` above: encode an existing resource's current content to H.264 via NVENC,
entirely host-side, with no changes anywhere in Venus's own resource- or image-creation code (see
the main `README.md`'s Tier 7 section for why that's possible — the short version: any resource
`virgl_renderer_resource_export_blob()` can hand a dma_buf fd for, this can encode). Confirmed
working end to end against real virglrenderer resource tracking and the real RTX 4060, standalone,
no Android/redroid involved — see [`../../tests/tier7_vcmd_encode_resource_test.c`](../../tests/tier7_vcmd_encode_resource_test.c).

### New files

- [`vtest_gpu_encode.c`](vtest_gpu_encode.c) / [`vtest_gpu_encode.h`](vtest_gpu_encode.h) — the
  encode module itself, structured like `vtest_gpu_alloc.c` (Vulkan loaded lazily via `dlopen` so
  the server keeps working without this command on non-NVIDIA hosts, same for CUDA/NVENC here).
  Add both to `vtest_sources` in `vtest/meson.build`. Needs FFmpeg's `nv-codec-headers` installed
  to `/usr/local/include` (`git clone https://github.com/FFmpeg/nv-codec-headers && cd $_ && sudo
  make install`) for `<ffnvcodec/nvEncodeAPI.h>` — same as [`../../tests/tier7_nvenc_dualexport_spike.c`](../../tests/tier7_nvenc_dualexport_spike.c).
- [`nvenc_scm_listener.c`](nvenc_scm_listener.c) / [`.h`](nvenc_scm_listener.h) /
  [`nvenc_scm_protocol.h`](nvenc_scm_protocol.h) — a second, independent transport for encoding a
  plain (non-Venus) dma-buf, for exactly the case `VCMD_ENCODE_RESOURCE` fundamentally can't reach:
  a real Surface-sourced capture buffer that was never allocated through this project's own Venus
  path (confirmed 2026-09-26, see DEVLOG). Runs as its own detached thread, started once from
  `vtest_main`'s entry point (see the `vtest_server.c` modification below) — calls straight into
  the *same* `vtest_gpu_encode_dmabuf()` above, just reached via `SCM_RIGHTS` fd-passing instead of
  a Venus resource id, mirroring redroid-hwenc's own VA-API daemon exactly (valid here because
  redroid shares one real kernel between guest and host). Add all three to `vtest_sources`.
  [`vtest_server.c`](vtest_server.c) is vendored in full here too (unlike everything else in this
  section) since this is the first change this project has ever needed to make to it — two lines
  only: `#include "nvenc_scm_listener.h"` and one `vtest_nvenc_scm_listener_start(server.socket_name)`
  call right before `vtest_server_run()`.

### Modifications to existing files (small, prose-documented like `VCMD_RESOURCE_ALLOC_GPU` above rather than checked in as a diff)

`vtest_protocol.h`: a new command ID after `VCMD_SEMAPHORE_IMPORT_SYNC_FD`:
```c
#define VCMD_ENCODE_RESOURCE 45
/* request = {res_id, width, height, drm_format, stride, modifier_lo, modifier_hi} - the
 * caller supplies the layout directly (its own gralloc/AHardwareBuffer_describe() metadata)
 * rather than this command deriving it from virgl_renderer_resource_get_info_ext(), which
 * only knows format/dimensions for classic resources, not the opaque host3d blobs this
 * project's buffers actually are (confirmed via a real EINVAL when tried the other way).
 * reply = {status, byte_count} followed by byte_count bytes of Annex-B H.264 if status==0. */
#define VCMD_ENCODE_RESOURCE_SIZE 7
#define VCMD_ENCODE_RESOURCE_RES_ID 0
#define VCMD_ENCODE_RESOURCE_WIDTH 1
#define VCMD_ENCODE_RESOURCE_HEIGHT 2
#define VCMD_ENCODE_RESOURCE_FORMAT 3
#define VCMD_ENCODE_RESOURCE_STRIDE 4
#define VCMD_ENCODE_RESOURCE_MODIFIER_LO 5
#define VCMD_ENCODE_RESOURCE_MODIFIER_HI 6
#define VCMD_ENCODE_RESOURCE_RESP_SIZE 2
#define VCMD_ENCODE_RESOURCE_RESP_STATUS 0
#define VCMD_ENCODE_RESOURCE_RESP_BYTES 1
```

`vtest.h`: `int vtest_encode_resource(uint32_t length_dw);`

`vtest_server.c`: one more line in the `vtest_commands[]` table, right after
`SEMAPHORE_IMPORT_SYNC_FD` — `HANDLER(ENCODE_RESOURCE, encode_resource, true)`. `init_context:
true` matters here: it's what makes the framework lazily create a real context before dispatch
(see the gotcha below).

`vtest_renderer.c`: `#include "vtest_gpu_encode.h"` near the existing `vtest_gpu_alloc.h` include,
and a new `vtest_encode_resource()` function right after `vtest_resource_export_fd()` — reads the
request, calls `virgl_renderer_resource_export_blob(res_id, &fd_type, &fd)` (the exact same call
`VCMD_RESOURCE_EXPORT_FD` already makes, just without handing the fd back to the client), passes
the fd plus the caller-supplied layout into `vtest_gpu_encode_dmabuf()`, and writes back
`{status, byte_count}` followed by the bytes.

### Two real bugs found getting the first end-to-end wire-protocol run working

1. **A pre-existing, racy `SIGSEGV`** in `vtest_resource_import_blob()`'s own synchronous barrier
   (`virgl_renderer_context_export_fence(ctx->ctx_id, 0, 0, &barrier_fd)`, added by this project's
   own `0004`-era patches — see the comment already there about forcing a round trip so a
   just-imported resource's `res_id` is safe to use immediately). Crashes intermittently
   (confirmed via `gdb`, ~1-in-3 to ~1-in-5 runs) when that barrier is the *very first* thing ever
   asked of a freshly-created Venus proxy context — i.e. only when a client imports a blob before
   ever submitting any real Venus ring traffic. **Not triggered by any real client**: redroid's
   guest Mesa Venus driver always creates a real `VkInstance`/`VkDevice` (genuine ring 0 activity)
   long before it ever imports a dma_buf as a blob resource. Only surfaced here because
   [`tier7_vcmd_encode_resource_test.c`](../../tests/tier7_vcmd_encode_resource_test.c) is a
   minimal hand-rolled vtest client that skips straight to `VCMD_RESOURCE_IMPORT_BLOB` with no
   real Vulkan/Venus traffic first — a test-harness artifact, not a Tier 7 bug, and out of scope to
   fix here (noted for whoever next touches `vtest_resource_import_blob`).
2. **`cuImportExternalMemory` failing with `CUDA_ERROR_INVALID_CONTEXT` (201)`, real and Tier
   7's own**: `vtest_gpu_encode.c`'s one-time init pops the CUDA context right after creating it
   (`encode_cuda_nvenc_init_locked()`, needed so NVENC gets a floating context — see the main
   `README.md`'s first spike). But the *per-resolution* reconfigure step
   (`encode_reconfigure_locked()`) makes further CUDA calls (`cuImportExternalMemory`,
   `cuExternalMemoryGetMappedMipmappedArray`) *after* that pop, on a context no longer current on
   this thread. Fixed with `cuCtxSetCurrent(enc.cuctx)` right before those calls, then popping
   again (`cuCtxPopCurrent_v2`) before the NVENC calls later in the same function — the context
   needs to flip between "current" (for CUDA) and "floating" (for NVENC) within a single
   reconfigure, not just once at startup like the original spike's single-shot flow.

With both fixed, `tier7_vcmd_encode_resource_test.c` runs the *entire* real chain over the real
vtest wire protocol: connect → `VCMD_CREATE_RENDERER` → `VCMD_RESOURCE_ALLOC_GPU` (real dma_buf on
the RTX 4060) → `VCMD_CONTEXT_INIT` (capset `VIRTGPU_DRM_CAPSET_VENUS` = 4 — plain/no-capset
contexts hit bug 1 above even more reliably) → `VCMD_RESOURCE_IMPORT_BLOB` (real `res_id`) →
`VCMD_ENCODE_RESOURCE` → 83 bytes of real, `ffprobe`-valid 256×256 H.264.
