# Mesa (guest Venus driver) patches — Tier 4

Base: AOSP `platform/external/mesa3d` mirror, commit in [`BASE`](BASE) (as checked out in this
project's `aosp-redroid-15` tree — no specific branch tag, just AOSP's `master`-tracking mirror
at the point this project cloned it).

## Where this came from

The host-side `virgl_test_server`/`libvirglrenderer` binaries this project uses (from
[`Shiro836/waydroid-nvidia`](https://github.com/Shiro836/waydroid-nvidia)'s `v0.1.2` release)
already carry two protocol extensions their author added to make Venus-over-vtest work at all
for an NVIDIA host in a container: exporting a real kernel `sync_file` fd for a pending venus
sync point (`VCMD_SYNC_EXPORT_SYNC_FILE`), and importing a dma-buf fd as a blob resource
(`VCMD_RESOURCE_IMPORT_BLOB`). Confirmed via `strings` on the deployed binaries — both command
handlers are present.

The *guest* side of both features lives in Mesa's `vn_renderer_vtest.c` (the client transport),
and `waydroid-nvidia` carries matching patches for it too — but against upstream desktop Mesa
(`gitlab.freedesktop.org/mesa/mesa`, base commit `a8ce4d8f`), not this project's AOSP mirror.
The two trees have drifted enough (this AOSP mirror's `vtest_protocol.h` doesn't even have
several intermediate command IDs upstream added since) that `git apply`/`git am` fail outright.
**`0001-venus-vtest-sync-fd-and-dma-buf-import-plus-fixes.patch`** is a hand-port of the same
mechanism onto this tree, keeping the exact wire-protocol numbers upstream chose
(`VCMD_SYNC_EXPORT_SYNC_FILE = 39`, `VCMD_PARAM_HAS_VENUS_SYNC_FD = 3`,
`VCMD_RESOURCE_IMPORT_BLOB = 40`) so it stays wire-compatible with the already-deployed,
already-patched host binaries. The two original upstream patches are kept alongside for
reference/attribution:

- `upstream-reference-0001-waydroid-nvidia-sync-fd.patch` — sync_file export over vtest
  (`has_external_sync`), lets `VK_KHR_external_semaphore_fd`/`VK_KHR_external_fence_fd` actually
  work. Without this, `SurfaceFlinger`'s `RenderEngine` can't initialize its Vulkan backend at
  all: `VulkanInterface::init()` aborts with *"Vulkan device does not support sufficient
  external semaphore sync fd features"* — required unconditionally for the compositor's fence
  sync, host NVIDIA driver support notwithstanding.
- `upstream-reference-0002-waydroid-nvidia-dmabuf-import.patch` — dma-buf import over vtest
  (`has_dma_buf_import`), which `vn_renderer_bo_create_from_dma_buf()` needs unconditionally.
  Without this, the vtest transport's `bo_ops.create_from_dma_buf` is `NULL` and the first real
  call into it (from `vn_get_memory_dma_buf_properties()`, deep in the AHardwareBuffer import
  path) crashes with a null-pointer jump (`rip=0x0`) — this is the actual mechanism behind the
  `EGL_BAD_PARAMETER` wall the whole earlier part of Tier 4 was chasing.

## One more real bug found underneath both

With both of the above applied, a *third*, unrelated latent bug surfaced for the first time:
`vn_android_gralloc_shared_present_usage_init_once()` (`vn_android.c`) asserts that
`front_rendering_usage` is non-zero whenever the gralloc query "succeeds" — but minigbm's
`gralloc0.cc` `GRALLOC_DRM_GET_USAGE` op reports `BUFFER_USAGE_FRONT_RENDERING` (`1ULL << 32`)
through a plain `uint32_t *` (both in minigbm's own local variable and in Mesa's
`cros_get_front_rendering_usage()` call site) — the bit silently truncates to 0 across that
32-bit ABI on **both** ends, so the query "succeeds" but can never actually report a non-zero
value for this specific flag. Patched Mesa's assert to accept that as a legitimate (if
unfortunate) outcome rather than aborting boot over it — a real fix needs the legacy
`gralloc0_perform()` ABI widened on both sides, out of scope here.

## Diagnostic instrumentation

The patch also carries the `REDROID-DIAG` `__android_log_print` instrumentation added across
`vn_android.c`'s `vn_GetAndroidHardwareBufferPropertiesANDROID` call chain while chasing the
original `EGL_BAD_PARAMETER` wall — left in since it's harmless (plain logging) and is what
proved, for the first time, that this path now succeeds end to end against real buffers. Safe to
strip for a production build.

## Result

With all three fixes in place: `SurfaceFlinger`'s `RenderEngine` initializes as real Vulkan
(`GaneshVkRenderEngine::create: successfully initialized`), `vn_GetAndroidHardwareBufferPropertiesANDROID`
succeeds repeatedly against real gralloc buffers, and the guest reaches `sys.boot_completed=1`
with a stable, non-crash-looping `surfaceflinger`. See `DEVLOG.md`'s 2026-09-23/24 entries for
the full chase, and the screenshot referenced there for the current (imperfect — see Tier 5) visual
result.

## Build

Standalone meson+NDK cross-build, see [`../../build/mesa/`](../../build/mesa/). Apply this patch
to a clean AOSP `external/mesa3d` checkout at `BASE`, then:

```sh
ANDROID_ABI=x86_64 ANDROID_API=34 NDK=/opt/android-ndk \
  build/mesa/build.sh /path/to/external/mesa3d /tmp/mesa-build-x86_64
```

producing `src/virtio/vulkan/libvulkan_virtio.so` — drop it in at
`/vendor/lib64/hw/vulkan.virtio.so` in the guest.
