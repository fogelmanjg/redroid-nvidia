# minigbm patches — Tier 4

Base: stock AOSP `platform/external/minigbm`, branch `android-15.0.0_r36`, commit in
[`BASE`](BASE). Clone it with:

```sh
git clone https://android.googlesource.com/platform/external/minigbm
cd minigbm && git checkout $(cat BASE)
```

## What's here

- **`nvidia_venus.c`** — a new minigbm backend, registered for DRM driver name `nvidia-drm`,
  allocating GPU buffers through a host-side `virglrenderer` vtest/venus server over a unix
  socket (see the [host renderer test in `tests/`](../../tests/) and `DEVLOG.md`'s Tier 3/4
  entries for the protocol and the standalone proof it works against real NVIDIA hardware).
  Drop it into `external/minigbm/` and add it to `Android.bp`'s `minigbm_core_files` filegroup
  (see the patch below) and `drv.c`'s backend list.
- **`0001-nvidia-venus-backend-and-cros_gralloc-fixes.patch`** — the rest of the wiring, as a
  plain `git diff` against the base commit above:
  - `drv.c` / `Android.bp`: register the new backend and its source file.
  - `cros_gralloc/cros_gralloc_driver.cc`: **two real, general-purpose bug fixes**, not specific
    to NVIDIA — a double-free in `allocate()`'s error path (explicit `drv_bo_destroy(bo)` at the
    `destroy_hnd:` label runs *again* after the `buffer` unique_ptr already owns `bo` and will
    destroy it itself on scope exit), and an unconditional `initialize_metadata()` call that
    fails whenever the caller doesn't request `enable_metadata_fd` (nothing ever checked that
    before trying to use a reserved region that, correctly, was never created). Both bugs are
    plausibly latent on every other minigbm backend too — they simply never got exercised in
    redroid before, since AMD/Intel redroid always uses Mesa's separate `gralloc.gbm.so` in host
    mode, never `gralloc.cros.so`'s real backend dispatch. Worth upstreaming independently of
    this project.

Apply with `git apply 0001-nvidia-venus-backend-and-cros_gralloc-fixes.patch` from the minigbm
checkout root, after copying `nvidia_venus.c` into place.

See `DEVLOG.md`'s 2026-09-23 entries for the full story of how each bug was found — each one
was hiding behind the previous one, only surfacing once the earlier fix let execution reach it
for the first time.

## 2026-09-24 update: `bo_invalidate`/`bo_flush`

Added `DMA_BUF_IOCTL_SYNC`-based `bo_invalidate`/`bo_flush` callbacks to `nvidia_venus.c` (this
backend had none at all before, unlike every other minigbm backend). Genuinely correct minigbm
behavior for a dma-buf-backed CPU-mappable buffer — but added while chasing a real, still-open
Tier 5 bug (non-deterministic visual corruption in `screencap` output) that this fix did **not**
resolve: confirmed via logging that the active lock/unlock path
(`cros_gralloc/mapper_stablec/Mapper.cpp`, IMapper v5) never actually calls these hooks for this
buffer. See `DEVLOG.md`'s 2026-09-24 entry for the leading hypothesis instead (a likely host-side
race in `virglrenderer`'s brand-new `VCMD_SYNC_EXPORT_SYNC_FILE` handler, from
[`patches/mesa/`](../mesa/)/`waydroid-nvidia`'s sync_fd work).
