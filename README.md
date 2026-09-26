# redroid-nvidia

Getting [redroid](https://github.com/remote-android/redroid-doc) (Android running in a Docker
container) to actually use an NVIDIA GPU — real 3D acceleration and hardware video decode first,
hardware video encode after that.

## The problem

This is the sibling project to [redroid-hwenc](https://github.com/fogelmanjg/redroid-hwenc), which
adds hardware H.264 encode (via VA-API) to redroid on AMD and Intel — confirmed working end to end
on both vendors. That project could start from a real foundation: `gpuMode=host` (GPU-accelerated
rendering via Mesa) already works on AMD/Intel, so the work was specifically about wiring hardware
*encode* on top of that.

NVIDIA doesn't have that foundation yet. As of today:

- **`gpuMode=host` doesn't boot on NVIDIA.** With driver capabilities correctly set (see below),
  `gralloc.redroid.so` (Mesa's GBM, built against Android's bionic libc) genuinely runs but fails
  at `gbm_create_device()` with `EINVAL` — it has no driver-table entry for NVIDIA's proprietary
  stack, only for `nouveau`. A real cross-libc/cross-namespace wall, not a config gap — see the
  findings below and the roadmap's Tier 0/1.
- **`gpuMode=guest` (pure software rendering) does work** as a fallback — confirmed booting clean
  in ~14s — but that's no acceleration at all, not a fix.
- **Hardware video encode can't reuse redroid-hwenc's approach at all.** `nvidia-vaapi-driver` is
  **decode-only** — `vainfo` lists plenty of decode profiles and zero `VAEntrypointEncSlice`
  entries. NVIDIA's real hardware encoder (NVENC) is exposed through NVIDIA's own proprietary API,
  not VA-API. Whatever solves encode here will need a genuinely different backend from
  redroid-hwenc's VA-API daemon, not a port of it.

So: two real, separate gaps (3D acceleration, then encode), and the first one is the actual
prerequisite for almost everything else — including hardware video *decode* inside apps, not just
encode.

## What's already been ruled out or confirmed

Real findings from initial investigation, carried over so this doesn't start from zero:

- **Two host kernel modules block redroid from booting *at all* on a fresh NVIDIA host**,
  independent of the GPU question: `loop` and `ext4` not loaded (`modprobe loop ext4` fixes it).
  Their absence surfaces as a confusing downstream symptom (`vold`/`blank_screen` failing
  `execv()`) that has nothing obviously to do with the real cause
  (`apexd-bootstrap` failing to mount an APEX because `/dev/loop-control` has no backing module).
  Worth a generic footgun check for anyone deploying redroid on a host that's never needed these
  modules before.
- **A real device-permission bug**, separate from the above: `nvidia-container-toolkit` creates
  `/dev/dri/renderD128` inside the container as `root:992`, and `surfaceflinger` (Android's
  `system` AID) isn't in that group in the container's Android-only user model. Host-side `chmod`
  has no effect — `nvidia-container-toolkit` recreates the node fresh inside the container, not a
  true bind-mount. Fix: `chmod 666` from *inside* the running container.
- **`NVIDIA_DRIVER_CAPABILITIES` defaults to `utility,compute` only** — no `graphics`, `video`, or
  `display` — unless set explicitly. All earlier testing used bare `--gpus all` without ever
  setting it; the "reaches the container" check below was done by reading the host-side generated
  CDI spec file, not a running container, so it couldn't have caught this gap. Fixed
  (`NVIDIA_DRIVER_CAPABILITIES=all` on every `docker run` from now on) and confirmed the graphics
  libraries genuinely present *inside* a running container via `docker exec`, not just inferred.
  With that fixed, the original `chooseEglConfig`/`SIGABRT` crash signature is **gone**.
- **The real blocker, found once the capability gap above was fixed**: redroid's own vendor
  gralloc (`gralloc.redroid.so`, Mesa's GBM built against Android's bionic libc) now genuinely
  runs, but fails at `gbm_create_device()` with `EINVAL` — its driver-name dispatch table only
  knows Mesa's own DRI drivers (i915, radeonsi, amdgpu, …), not NVIDIA. NVIDIA's *real* GBM
  backend (`nvidia-drm_gbm.so`) is present in the container, correctly injected by CDI — but under
  a glibc-only path (`/usr/lib/x86_64-linux-gnu/gbm/`) that Android's bionic-linked vendor HAL has
  no mechanism to load from at all. A genuine cross-libc, cross-namespace wall, not a missing file
  or a config flag — see DEVLOG's 2026-09-22 entry for the full reasoning.
- **Forcing Zink (Mesa's GL-over-Vulkan driver)** via the usual env vars didn't change the crash
  signature — inconclusive as a real test, since Android's `platform_android` EGL backend likely
  doesn't honor those particular override points the same way a normal desktop Mesa app would.
  Worth revisiting with a more precise override point if this route comes back.
- **Prior art exists and fits better than first thought**:
  [waydroid-nvidia](https://github.com/Shiro836/waydroid-nvidia) gets real GPU-accelerated
  Android-in-container on NVIDIA working (verified: Minecraft Bedrock, 2ms present-to-present
  latency). Its approach — proxy Vulkan (Mesa Venus) from the Android guest over a Unix socket to a
  host-side `virglrenderer` process, allocate buffers host-side as native NVIDIA images, hand them
  back as NVIDIA dmabufs — sidesteps the cross-vendor EGL/gralloc negotiation problem entirely,
  exactly the class of bug hit here. **Originally flagged as needing a real Wayland compositor,
  ruling out headless servers — reading their own architecture doc directly showed that's wrong**:
  Wayland/KWin is only the *display* sink, specific to Waydroid's own `hwcomposer.waydroid` HAL,
  not part of the Venus/virglrenderer rendering pipeline itself. redroid has its own, already-
  headless hwcomposer and never needs that piece. See Tier 2 in the roadmap and the DEVLOG for the
  full pipeline breakdown. The strongest lead for this problem, now confirmed compatible with
  headless operation.
- Untested variables worth tracking if revisited: `nvidia-open`/`nvidia-open-dkms` kernel modules
  vs. the classic proprietary blob (not yet confirmed which one the test machines were running),
  driver version (waydroid-nvidia's write-up specifies 595.71+ with `nvidia-drm.modeset=1`), and
  GPU generation (Turing/RTX 20 or newer confirmed working there; the Pascal-era GTX 1050 Ti used
  for testing here is older than that — the RTX 4060 (Ada) in this same fleet would qualify).

## Roadmap (by difficulty, not by time)

Same shape as [redroid-hwenc](https://github.com/fogelmanjg/redroid-hwenc)'s roadmap: each tier
assumes the previous one is done, documented session by session in [DEVLOG.md](DEVLOG.md) as it
happens, bugs and dead ends included. ⭐ marks the highest-leverage checkpoint.

- [x] **Tier 0 — Confirm or kill the "special Docker" theory, and get a real, precise diagnosis.**
      Ruled out `nvidia-docker2` (deprecated, replaced by `nvidia-container-toolkit` on stock
      Docker). Found the real gap instead: `NVIDIA_DRIVER_CAPABILITIES` silently defaulted to
      `utility,compute` in every prior test, so NVIDIA's graphics/EGL/GBM pieces were plausibly
      never actually in the container being tested. Fixed it, confirmed the libraries genuinely
      present inside a running container (not just inferred from the host-side CDI spec file), and
      watched the original `chooseEglConfig`/`SIGABRT` crash disappear — replaced by a precise,
      understood one: Android's bionic-built Mesa GBM has no driver-table entry for NVIDIA and no
      way to reach NVIDIA's real (glibc-only) GBM backend. A genuine cross-libc/cross-namespace
      wall, not a config knob — confirms `waydroid-nvidia`'s Venus-proxy approach is the right
      direction rather than a nudge away. See DEVLOG's 2026-09-22 entry.
- [x] **Tier 1 — ⭐ Check what redroid's own Mesa already ships before porting anything.**
      `/vendor/bin/gpu_config.sh` shows ANGLE is only ever used as a *software* GLES fallback in
      guest mode (an alternative to SwiftShader) — `host` mode itself is plain native Mesa GBM,
      no host-forwarding logic wired up or active. Found and fixed a real, separate bug along the
      way: the render-node auto-detect loop doesn't recognize NVIDIA's driver name (`nvidia-drm`),
      so `gralloc.gbm.device` silently never got set on NVIDIA hosts without an explicit
      `androidboot.redroid_gpu_node=` override. Fixing it and re-testing with the device path
      *provably* correct end to end (confirmed via `getprop`) produced the **identical**
      `gbm_create_device()`/`EINVAL` failure — ruling out "the property wasn't set" as an
      alternate explanation and confirming Tier 0's diagnosis on firmer ground. **Corrected by
      Tier 2, below**: "nothing to reuse" turned out too strong — the vendor partition already
      ships `vulkan.virtio.so` and `virtio_gpu_dri.so`, dormant guest-side Venus driver files,
      just never activated on this hardware path. See DEVLOG's 2026-09-22 entries.
- [x] **Tier 2 — ⭐ Understand `waydroid-nvidia`'s Venus-proxy architecture in depth.** The
      load-bearing question — is the Wayland compositor it requires structural or incidental? —
      is answered directly from their own architecture doc: **incidental**. The full rendering
      pipeline (guest Mesa Venus → vtest unix socket → a standalone host `virglrenderer` process →
      real Vulkan against NVIDIA → dmabuf) never touches Wayland at all; KWin only enters at the
      very last step, through `hwcomposer.waydroid` — Waydroid's *own* hwcomposer HAL, built to
      hand frames to a Wayland window. redroid has its own, already-headless hwcomposer HAL and
      never needs that step. Bonus finding: redroid's vendor partition already ships the guest
      side of exactly this bridge (`vulkan.virtio.so`, `virtio_gpu_dri.so`), just dormant — what's
      genuinely missing is host-side infrastructure: a `virglrenderer` vtest/venus server against
      the real driver, and minigbm's vtest allocation wrapper (a real patch upstream minigbm
      doesn't have). See DEVLOG for the full pipeline diagram and reasoning.
- [x] **Tier 3 — ⭐ Minimal headless host-side renderer prototype.** Confirmed for real, standalone,
      no Wayland/redroid/Android anywhere in the loop. First had to swap `jgustavo48`'s NVIDIA
      driver to the **open** kernel module (a real prerequisite — the closed module has no DMA-BUF
      support, and every buffer here is one; done carefully since it's a live gaming/streaming
      machine, via NVIDIA's own apt repo at the exact installed driver version to avoid a userspace/
      kernel-module mismatch, no regressions found). Then downloaded the project's own `v0.1.2`
      GitHub release (host binaries, checksum-verified — no need to build `virglrenderer` from
      source) and used **Debian's own packaged** Mesa Venus Vulkan driver
      (`mesa-vulkan-drivers`' `libvulkan_virtio.so`) as the test client — no Mesa cross-build
      needed for this stage either. Found and fixed two real bugs (silent failure without
      `VIRGL_LOG_LEVEL=debug`; the sandboxed render-helper process expects a hardcoded install
      path the release tarball doesn't create). With both fixed, a real Vulkan compute shader —
      dispatch, fence wait, readback, verified value-by-value — passed outright over the vtest
      socket against the real RTX 4060:
      `PASS: 65536 elements computed correctly on 'Virtio-GPU Venus (NVIDIA GeForce RTX 4060)'`.
      See DEVLOG for the full story including the kernel-module swap.
- [x] **Tier 4 — ⭐ Real integration into redroid — booted to the Android home screen with real
      NVIDIA GPU acceleration through Venus, for the first time in this project.** redroid ships
      *two* gralloc HALs as prebuilts:
      `gralloc.gbm.so` (Mesa, no NVIDIA support) and `gralloc.cros.so` (minigbm's real backend-
      dispatch system) — `gpu_config.sh` never selected `cros`. Wrote a real minigbm backend
      (`nvidia_venus.c`, driver name `nvidia-drm`) allocating over the vtest wire protocol proven
      standalone in Tier 3, patched `gpu_config.sh` to select `cros` + `ro.hardware.egl=angle` +
      Venus props for NVIDIA. **Found and fixed three real, independently-confirmed bugs, each one
      exposing the next**: (1) this backend's own — `bo->handle` held a raw fd instead of a real
      local GEM handle, so minigbm's generic `drv_bo_get_plane_fd()` failed silently on every
      allocation (this, not anything about "gpu writeable", was the true original cause of every
      earlier "output buffer not gpu writeable" abort — the buffer was never actually allocated);
      (2) a genuine double-free in AOSP's own `cros_gralloc_driver::allocate()`, never triggered
      before on this codebase (fixing bug 1 let execution reach it for the first time); (3)
      `initialize_metadata()` called unconditionally in that same AOSP function, also never
      exercised before — strong evidence `gralloc.cros.so` had plausibly never been exercised
      through a real boot on this tree at all (AMD/Intel redroid always used Mesa's separate
      `gralloc.gbm.so` in host mode). **With all three fixed, buffer allocation is now genuinely,
      fully correct** — every real format/size combination succeeds cleanly, confirmed via
      logging; SurfaceFlinger's RenderEngine stands up a real Vulkan device against the GPU through
      Venus (`ANGLE (NVIDIA, Vulkan 1.1.274 (NVIDIA Virtio-GPU Venus (NVIDIA GeForce RTX 4060)))`).
      **The wall moved one level deeper**: `Could not create EGL image, err = (0x300c)`
      (`EGL_BAD_PARAMETER`) — traced by hand across three codebases (ANGLE, Mesa/Venus, AOSP's
      `u_gralloc`/gralloc0 bridge). Confirmed `EGL_NATIVE_BUFFER_ANDROID` in ANGLE really is the
      AHardwareBuffer import path (`HardwareBufferImageSiblingVkAndroid`), that `0x300c` is
      ANGLE's own generic fallback for *any* underlying Vulkan failure there, and — via a small
      standalone probe reusing Tier 3's connection method, no Android boot needed
      ([`tests/list_exts.c`](tests/list_exts.c)) — that both prerequisites Venus checks before
      advertising `VK_ANDROID_external_memory_android_hardware_buffer`
      (`EXT_image_drm_format_modifier`, `EXT_queue_family_foreign`) genuinely reach the guest.
      **Then found the "real Vulkan device" claim above needed a caveat**: reproducing it from a
      clean container hit three *earlier*, unrelated problems first — two infra bugs (two test
      containers sharing one GPU/vtest server destabilizes the whole Android container, not just
      Vulkan; the `libdrm.so -> libdrm.so.2` symlink fix from `gralloc.cros.so` needs reapplying to
      *every* fresh container, and skipping it fails silently as far back as
      `vkEnumerateInstanceVersion` itself) and one real, older bug — `SurfaceFlinger` picks the GL
      Skia backend by default here (an aconfig flag gates Vulkan, unset in this build), fixed
      permanently by adding `setprop debug.renderengine.backend skiavkthreaded` to
      `gpu_config.sh`. With a clean, single-container boot and all three fixed, hit a new, precise,
      **not-Mesa** wall: `VulkanInterface::init()` aborts on `VK_KHR_external_semaphore_fd`'s
      `SYNC_FD` handle type — required unconditionally by SurfaceFlinger's compositor fence sync.
      Confirmed by direct comparison against the same running `virgl_test_server`: the **host**
      NVIDIA driver genuinely supports it (real `vulkaninfo` device extension), but Venus never
      forwards it to the guest (absent from all 108 extensions `list_exts` sees, though the base
      `VK_KHR_external_semaphore` is present). **Went looking for how
      [`waydroid-nvidia`](https://github.com/Shiro836/waydroid-nvidia) itself solves exactly
      this — same problem, same architecture — and found real, working patches for it.** The
      host binaries this project already uses (`v0.1.2`) turned out to already carry the
      server-side half (confirmed via `strings`); only the *guest*-side Mesa patch was missing.
      Two upstream trees had drifted too far apart for `git apply` to work, so hand-ported the
      mechanism onto this project's own Mesa checkout instead, keeping upstream's exact
      wire-protocol numbers so it stays compatible with the already-deployed host binaries — see
      [`patches/mesa/`](patches/mesa/). That alone exposed a **second** missing transport
      capability (`bo_ops.create_from_dma_buf` also hardcoded `NULL`) that
      `vn_get_memory_dma_buf_properties()` — deep in the AHardwareBuffer import path — needs
      unconditionally; ported waydroid-nvidia's matching fix for that too. Underneath both, hit a
      **third**, genuinely new bug: a 32-bit truncation of `BUFFER_USAGE_FRONT_RENDERING`
      (`1ULL << 32`) across minigbm's legacy `gralloc0_perform()` ABI, on both ends, silently
      losing the bit and firing an over-strict Mesa assert — softened the assert rather than
      widen a legacy ABI for a flag not needed here. **With all three fixed, `RenderEngine`
      initialized as real Vulkan for the first time ever in this project**
      (`GaneshVkRenderEngine::create: successfully initialized`), and the exact
      `EGL_BAD_PARAMETER`/AHardwareBuffer wall from earlier in this same tier **resolved itself**
      — it was never a bug in the import logic, which was correct the whole time; it was blocked
      from ever running by the two missing transport capabilities above. `sys.boot_completed`
      reached `1`, `surfaceflinger` stayed up with no restart loop, and a `screencap` from inside
      the guest shows Android's real setup wizard, composited through real Vulkan RenderEngine
      through Venus through a real RTX 4060 —
      [`docs/tier4-first-boot-nvidia-venus.png`](docs/tier4-first-boot-nvidia-venus.png). The
      image has a visible corruption artifact (legible, not a crash) — see Tier 5 below for what
      it actually turned out to be. See DEVLOG for the full Tier 4 trail, bug by bug.
- [ ] **Tier 5 — Confirm real 3D acceleration end to end. In progress: system boots and renders,
      real corruption bug narrowed to byte-level precision, root cause not yet fixed.** Same bar
      redroid-hwenc held itself to for encode: an actual verified rendered frame, not just
      "doesn't crash." Started from the visible corruption in Tier 4's screenshot, assumed a
      stride/row-pitch bug in `nvidia_venus.c` (the one piece of this chain written from scratch).
      **Ruled that out with one cheap test**: three `screencap`s of the exact same static screen
      came back with three different corruption patterns and checksums — a real stride bug is
      deterministic on unchanging content; this isn't. Checked the stride math anyway
      (`vtest_gpu_alloc_cpu`'s `ALIGN(width*bpp, 256)`, confirmed against real logged values) —
      it's correct. Added `bo_invalidate`/`bo_flush` (`DMA_BUF_IOCTL_SYNC`) to `nvidia_venus.c` —
      genuinely-correct minigbm behavior this backend was missing entirely — but confirmed via
      logging it's **never called** by the active lock path on this Android 15 build (the newer
      AIDL `IMapper` v5, which calls `cros_gralloc_driver::lock()`/`unlock()` directly), so it
      didn't touch the actual bug. Traced that real lock path instead: it does wait on a real
      Android acquire-fence before returning a CPU pointer — but `cros_gralloc_sync_wait()`
      treats any negative fence value, including `-1`, as "already signaled, don't wait" — the
      exact convention today's own `vn_queue.c` patch introduced for the new sync_fd path.
      Tested whether an immediate submit-then-export race against the host's own bookkeeping
      could cause a premature `-1`: added a diagnostic 2ms delay before the export call — **no
      change**, weakening that specific theory. **Went straight to the raw bytes instead**:
      `screencap`'s raw dump mode shows real, correct background pixels
      (`(66, 133, 244, 255)`, Android's own blue) alternating with **exactly `(0, 0, 0, 0)`** —
      genuinely never-written, zero-filled memory, not stale or noisy data — in run-lengths that
      are **always an exact multiple of 16 pixels (64 bytes)**. That precise, fixed-grain
      quantization rules out both prior theories and points at something much more specific: a
      **tiled/block-linear GPU memory layout being read back as if it were plain row-major
      linear** — most likely in whatever Vulkan blit actually populates this
      `vtest_gpu_alloc_cpu`-backed CPU buffer from `RenderEngine`'s real rendered frame, not
      declaring/honoring the linear tiling everyone downstream assumes.
      **Found the exact line within the hour**: `RenderEngine` uses Skia directly
      (`GaneshVkRenderEngine`), not ANGLE (a wrong turn first — ANGLE is for GLES *apps*, unrelated
      to `SurfaceFlinger` itself). Skia's own `AHardwareBufferVk.cpp::make_vk_backend_texture`
      hardcodes `VK_IMAGE_TILING_OPTIMAL` unconditionally, with its own `TODO` already admitting
      it: *"Add better linear support throughout Ganesh."* Confirmed via logging that Mesa's
      correct explicit-modifier path (`vn_android_get_image_builder`) never even fires for this
      buffer — matches the byte-level evidence exactly: `OPTIMAL` means the real driver writes
      using its own proprietary tiled addressing into memory every CPU reader treats as plain
      linear. **Patched it (and ANGLE's identical pattern) to use `LINEAR` for genuinely
      CPU-accessed buffers, built a full ANGLE and `surfaceflinger` (Skia is statically linked),
      tested on real hardware — and it made things measurably *worse***: raw pixel dumps went
      from "real content in a structured 64-byte grid" to "almost entirely zero, no legible
      content at all." The real NVIDIA driver genuinely doesn't handle `LINEAR` AHB import
      correctly here, not just for the `INPUT_ATTACHMENT` case the upstream comments call out.
      **Reverted both changes cleanly** (confirmed via a fresh boot reproducing the original,
      better, partially-legible corruption exactly) — an important negative result, not a dead
      end: it rules out a tiling-mode switch entirely and narrows the real fix to something more
      structural — an explicit untiling *copy* step (driver-tiled render target →
      genuinely-linear separate buffer) before any CPU reader touches the memory, not a property
      of the single AHB-imported image. See DEVLOG's 2026-09-24 entries for the full trail, test
      by test, including exactly which file/line to pick this back up from.
      **Set up a `virglrenderer` build environment (real upstream source at the exact commit
      `waydroid-nvidia`'s own patches target, all four applied cleanly) — the missing piece
      before now — and used it to test the "obvious" host-side fix**: `vtest_gpu_alloc.c`
      already contains a complete, working, real-Vulkan linear+host-visible+renderable
      allocation (`vtest_gpu_alloc_image(linear=true)`), just never wired up to
      `vtest_gpu_alloc_cpu()` — the actual function used for every CPU-mappable buffer — which
      instead uses a plain memfd+`/dev/udmabuf` path, under the original author's own comment:
      *"NVIDIA-linear path suspected of breaking hwcomposer's own SW buffers."* Wired it up and
      tested on real hardware: **confirmed the author's suspicion directly** — `vkQueueSubmit
      resulted in CS error` on every connection, tearing down the Vulkan context each time,
      eventually limping to a flat, completely blank white screen (no corruption, but also zero
      content — worse than the baseline). Reverted cleanly (checksums confirmed against the
      pre-experiment originals). This rules out *any* fix that makes the CPU-mappable buffer a
      real, directly GPU-renderable image — both a bare tiling-mode switch and a fully-correct
      driver-native linear allocation fail, independently, for different reasons. **Real next
      step**: keep the render target `OPTIMAL` (confirmed more stable) and add a genuinely
      separate second buffer with an explicit `vkCmdCopyImage`/`vkCmdBlitImage` untiling step
      between them — standard practice elsewhere, just not implemented anywhere in this chain
      yet. **Checked whether this is narrowly a `screencap` problem before committing to that
      fix**: connected `adb` directly to the container's Docker bridge IP and recorded a few
      seconds with `scrcpy --no-window --record=...` — **same corruption, clearly present in
      real video output.** Makes sense without Tier 6/7 yet: no hardware encoder means Android's
      `MediaCodec` falls back to a software H.264 encoder, which needs the same CPU-readable RGBA
      source `screencap` does, hitting the identical broken path. Practical takeaway: today,
      *every* way to get pixels out of this guest for viewing or recording is affected, not just
      screenshots. **Decision: shelved here, in favor of Tier 7.** Rather than build Tier 5's own
      two-buffer untiling fix first and put a real encoder on top of it, do the reverse: build
      Tier 7 (NVENC) first, since a real hardware encoder reads the driver's native tiled surface
      directly and never touches it from the CPU — for the video/streaming use case specifically,
      it may make Tier 5's fix unnecessary by construction rather than needing it as a
      prerequisite. `screencap`/any genuine CPU pixel read still needs Tier 5's fix regardless,
      but that's no longer the critical path. See DEVLOG's 2026-09-25 entries for the full trail.
- [ ] **Tier 6 — Hardware video decode.** Should become reachable once Tier 5 is solid —
      `nvidia-vaapi-driver` already provides VA-API decode; the Codec2 side of that story hasn't
      been investigated at all yet in this context.
- [ ] **Tier 7 — ⭐ Hardware video encode (NVENC). In progress: the make-or-break spike is
      confirmed on real hardware.** Structurally similar to what redroid-hwenc solved for VA-API
      (a host-side daemon the Codec2 component talks to), but the exact transport differs: NVENC
      is driven through CUDA's external-memory interop, not a plain VA-API dma-buf import, and the
      real question was whether that interop tolerates the *same* dma-buf minigbm's
      `nvidia_venus.c` backend already hands the guest for `HW_VIDEO_ENCODER`-usage buffers today
      (untouched by Tier 5 — that backend only sets the `MAPPABLE` alloc flag for `SW_READ`/
      `SW_WRITE`/`LINEAR` usage, never for `HW_VIDEO_ENCODER` alone, so such a buffer already comes
      back from `vtest_gpu_alloc_gpu()` as a real, OPTIMAL-tiled, GPU-only allocation — exactly
      the shape Tier 7 needs, with no minigbm changes required). Standalone spike
      ([`tests/tier7_nvenc_dualexport_spike.c`](tests/tier7_nvenc_dualexport_spike.c), no
      Android/redroid/virglrenderer involved) built the same OPTIMAL/DRM-modifier-tiled image
      `vtest_gpu_alloc_gpu()` allocates, cleared it to a known color via `vkCmdClearColorImage`
      (GPU-only, no CPU write), and tried to get that exact allocation into NVENC. **First real
      finding**: CUDA's `cuImportExternalMemory` has no dma_buf-specific handle type at all — only
      `OPAQUE_FD`/`WIN32`/`D3D12` — and a fd the Vulkan driver exported specifically as
      `VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT` is rejected outright when imported as
      `OPAQUE_FD` (`CUDA_ERROR_UNKNOWN`), so the two handle-type tags are not interchangeable on
      this driver despite referring to the same kind of Linux fd underneath. **Fix, confirmed
      working**: request *both* handle-type bits (`DMA_BUF_BIT_EXT | OPAQUE_FD_BIT_KHR`) in the
      same `VkExternalMemoryImageCreateInfo`/`VkExportMemoryAllocateInfo` at allocation time, then
      call `vkGetMemoryFdKHR` twice against the one underlying `VkDeviceMemory` — once per handle
      type. The driver hands back two independent fds referencing the same allocation; the
      `DMA_BUF` one is exactly what today's guest-side pipeline already consumes unchanged, and
      the `OPAQUE_FD` one imports into CUDA cleanly
      (`cuImportExternalMemory`/`cuExternalMemoryGetMappedMipmappedArray`/
      `cuMipmappedArrayGetLevel` → a real `CUarray`, tagged with the
      `CUDA_ARRAY3D_VIDEO_ENCODE_DECODE` flag `dynlink_cuda.h` defines for exactly this use).
      **Two further real bugs found and fixed**, both driver-version mismatches rather than
      logic errors: (1) the installed `ffnvcodec`/NVENC SDK header (13.1, from FFmpeg's
      `nv-codec-headers`) is newer than what this driver's `libnvidia-encode` (595.91.07) actually
      implements (13.0, confirmed via `NvEncodeAPIGetMaxSupportedVersion`) — every `NV_ENC_*_VER`
      struct-version macro bakes in the header's own major.minor at compile time, so using them
      as-is fails every call with `NV_ENC_ERR_INVALID_VERSION`; fixed by rebuilding each struct's
      version tag from the driver's own reported major.minor instead of the header's; (2) NVENC
      manages pushing/popping its CUDA context internally and rejects one still left current on
      the calling thread (`NV_ENC_ERR_INVALID_ENCODERDEVICE` from `NvEncInitializeEncoder`, despite
      `NvEncOpenEncodeSessionEx` on the very same context having already succeeded) — fixed with
      an explicit `cuCtxPopCurrent` after CUDA-side setup finishes and before any NVENC call,
      matching NVIDIA's own `NvEncoderCuda` sample. **With all three fixed, the full chain ran
      end to end on the real RTX 4060**: real Vulkan OPTIMAL/tiled image → dual dma_buf/opaque_fd
      export → CUDA external-memory import → `CUarray` → `NvEncRegisterResource`
      (`NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY`) → `NvEncEncodePicture` → 314 bytes of real,
      decodable H.264 — decoded back with `ffmpeg`, the pixel color survived the whole trip
      ((220,180,40) in → (214,184,44) out, matching within normal YUV420 round-trip rounding).
      **Zero CPU reads of pixel data anywhere in this path.** One operational gotcha found along
      the way: leaving the NVENC session/CUDA context open at process exit hangs the driver's own
      teardown — fixed by calling `NvEncDestroyEncoder` explicitly before exiting.

      **Second spike — can this work on a resource Venus itself owns, with zero changes to
      Venus's own resource-creation code?** virglrenderer already exposes
      `virgl_renderer_resource_export_blob(res_id, &fd_type, &fd)` — a dma_buf fd for *any*
      tracked resource by ID, including whatever real resource Venus creates for SurfaceFlinger's
      render target. The open question was whether the *encode side*, receiving only that fd (not
      the original `VkImage`/`VkDevice` that made it, since Venus's own resource-creation code is
      out of scope here), could still get it into NVENC.
      **First attempt, confirmed broken**
      ([`tests/tier7_nvenc_reexport_spike-FAILED.c`](tests/tier7_nvenc_reexport_spike-FAILED.c),
      kept as a documented negative result): import the foreign fd and re-export it as
      `OPAQUE_FD_BIT_KHR` from the same `vkAllocateMemory` call
      (`VkImportMemoryFdInfoKHR` + `VkExportMemoryAllocateInfo` chained together). This returns
      `VK_SUCCESS`, `cuImportExternalMemory` succeeds, NVENC produces a plausible-looking bitstream
      — but the decoded pixel content comes back black, not the real cleared color.
      `vkGetPhysicalDeviceImageFormatProperties2(handleType=DMA_BUF_BIT_EXT)` on this exact
      image/modifier reports `compatibleHandleTypes=0x200` (`DMA_BUF` only) — `OPAQUE_FD` (`0x1`)
      is not in that set, meaning this combination is a genuine Vulkan spec violation (an
      export's handle type must be compatible with the import it's chained to) that the driver
      silently accepts instead of rejecting, producing wrong data with no error anywhere in the
      chain. **Fixed shape, confirmed correct**
      ([`tests/tier7_nvenc_copy_export_spike.c`](tests/tier7_nvenc_copy_export_spike.c)): plain-
      import the foreign fd (`DMA_BUF_BIT_EXT` only, no export chained — self-compatible per the
      same query, and confirmed correct) into the encode side's own `VkDevice`, then a real
      `vkCmdCopyImage` (both images `OPTIMAL`/tiled throughout — never touches Tier 5's broken
      `LINEAR`/CPU-mappable path) into a second, freshly *self*-allocated image built with spike
      1's already-proven dual-export shape, then export and encode that copy. Confirmed correct on
      real hardware: (30,200,90) in → (44,226,94) out, the same order of YUV round-trip delta as
      spike 1. **This means the real Tier 7 daemon needs zero changes to Venus/virglrenderer's own
      resource/image-creation code** — it can `export_blob()` any existing resource by ID,
      independent of whatever handle type Venus itself used to create it, at the cost of one real
      GPU copy per encoded frame.

      **Third spike — the new vtest command itself, confirmed working end to end over the real
      wire protocol.** Added `VCMD_ENCODE_RESOURCE` directly to virglrenderer's vtest server (no
      Venus/resource-creation changes — see
      [`patches/virglrenderer/README.md`](patches/virglrenderer/README.md) for the exact protocol
      additions and the new [`vtest_gpu_encode.c`](patches/virglrenderer/vtest_gpu_encode.c)
      module, which wraps the second spike's confirmed shape — plain-import → GPU copy → NVENC —
      behind a persistent, resolution-aware encoder session reused across calls): request =
      `{res_id, width, height, format, stride, modifier}` (the caller supplies the layout directly
      rather than relying on `virgl_renderer_resource_get_info_ext()`, which doesn't know about
      this project's opaque host3d blob resources — confirmed via a real `EINVAL` when tried the
      naive way first), reply = H.264 bytes. A standalone client
      ([`tests/tier7_vcmd_encode_resource_test.c`](tests/tier7_vcmd_encode_resource_test.c), no
      Android/redroid involved — same "no Android boot needed" spirit as `tests/list_exts.c`)
      drives the real vtest wire protocol directly: allocate a real GPU-only resource, register it
      for a real `res_id`, then call the new command. **Two more real bugs found and fixed**: (1)
      a pre-existing, racy `SIGSEGV` in `vtest_resource_import_blob()`'s own synchronous barrier
      when a blob import is the *very first* thing asked of a freshly-created Venus context —
      never triggered by a real client (which always has genuine ring traffic first), only by this
      minimal test harness; documented but out of scope to fix here. (2) `cuImportExternalMemory`
      failing with `CUDA_ERROR_INVALID_CONTEXT` on the *second* call to the encode module: the
      first spike's CUDA context is popped once at startup for NVENC's sake, but a persistent,
      multi-call encoder needs to flip it back to current for each reconfigure's CUDA-side work,
      then pop again before the NVENC calls in that same reconfigure — a real ordering bug specific
      to making the one-shot spike's flow persistent across many frames. With both fixed: real
      `res_id` → `virgl_renderer_resource_export_blob()` → plain-import → GPU copy → NVENC → 83
      bytes of real, `ffprobe`-valid 256×256 H.264, confirmed on the RTX 4060.

      **The guest side — a real `c2.hardware.encoder.h264` Codec2 component, confirmed building
      and linking against this project's actual AOSP tree.** Written as
      [`patches/codec2/`](patches/codec2/) (built and confirmed at
      `~/aosp-redroid-15/external/nvenc_codec2/` — this project's own AOSP checkout, the same one
      Tier 4's minigbm/gralloc work used), adapted directly from redroid-hwenc's own
      `VaapiEncComponent` (present in this same tree at `external/vaapi_codec2/`). Two real
      simplifications over that reference, both confirmed rather than assumed: (1) no daemon or
      dma-buf forwarding needed at all — the buffer already lives on the host, so the component
      only resolves its *Venus resource id* via the guest kernel's own
      `DRM_IOCTL_VIRTGPU_RESOURCE_INFO` (a standard virtio-gpu ioctl) and asks the host to encode
      that resource directly via `VCMD_ENCODE_RESOURCE`, over a **brand-new vtest connection** of
      its own; (2) no empirical native_handle_t reverse-engineering needed — this project's actual
      gralloc HAL (`cros_gralloc`) produces a real, well-defined `cros_gralloc_handle_t` for every
      buffer, read directly with the same validation `cros_gralloc_convert_handle()` itself does,
      unlike `VaapiEncComponent`'s own empirically-dumped-integer-offsets situation. That the
      component can use a *separate* connection from minigbm's own at all rests on a real,
      checked finding: `virgl_renderer_resource_export_blob()` resolves resources through
      `virgl_resource_lookup()` — no `ctx_id` involved — confirming virglrenderer's resource table
      is global across the whole server process, not scoped per connection. **Confirmed building
      clean** (both the component library and the full service binary, 64 and 32-bit) inside the
      `redroid-build-persist` container — one real Soong gotcha found and fixed identically to how
      `VaapiEncComponent`'s own service Android.bp already had to (a `cc_library_static`'s
      `shared_libs` don't propagate to the binary that links it — `libdrm` needed relisting
      explicitly, confirmed via a real `undefined symbol: drmIoctl` on the first attempt). **Not
      yet tested**: actual deployment into a running redroid container and a real on-device encode
      — see [`patches/codec2/README.md`](patches/codec2/README.md)'s closing section for the exact
      three open checks (service registration/`dumpsys media.c2`, whether a real Surface-sourced
      frame really is a `cros_gralloc_handle_t` in practice, and the full `MediaCodec` → this
      component → NVENC round trip) — the natural next session's checkpoint, the same way
      redroid-hwenc's own Tier 4 (build/register) and Tier 5 (real integration) were split across
      sessions rather than done in one sitting. See DEVLOG's 2026-09-25 entries for the full
      session, including the exact commands and error codes at each step.

      **Fourth session, same day — real deployment on `jgustavo48`, first-ever full boot with
      NVIDIA GPU acceleration, and Tier 5's corruption bug confirmed live (and confirmed mild).**
      Not a code change to this project's own patches — a live-infrastructure debugging session
      that closes the loop between everything above and an actual, usable device. Three
      independent classes of bug, all on the deployment/host side, none in Tier 7's own code:
      (1) a chain of missing host kernel prerequisites specific to `jgustavo48` post-reboot
      (binder device nodes existing as plain directories instead of real nodes, `loop`/`ext4`
      modules unloaded, a long list of netfilter modules `netd` needs that goes well beyond the
      `iptable_filter` pair this project's own earlier findings mentioned); (2) **two separate,
      previously-undiscovered deployment gaps**: no existing `deploy_t4_*.sh` script on
      `jgustavo48` ever copied the real Vulkan ICD (`vulkan.virtio.so`) or ANGLE into a fresh
      container at all, and that ICD's own `libdrm.so` dependency (the unversioned name, not the
      `.so.2` the image ships) was missing for *both* the 64-bit copy (blocking `RenderEngine`/
      `surfaceflinger` outright with "Could not find any physical devices") *and*, discovered only
      after fixing the first, the 32-bit copy (crashing the 32-bit `media.codec`/OMX service with
      `Abort message: 'gralloc-mapper is missing'` — the actual reason a first successful-looking
      `scrcpy` connection showed a persistently empty window: the video encoder's producer-side
      process was dying before ever handing back a frame, not a Tier 5 corruption issue at all);
      (3) `virgl_test_server` needs `--multi-clients` even for a single container, since
      `surfaceflinger` opens more than one Venus connection and the second one hangs forever
      without it — confirmed via `debuggerd -b` on the stuck PID, not guessed. Full checklist
      written up as a standalone reference for this host, since it's a distinct, longer list than
      what was known before. With all of it fixed: a genuinely fresh container reached
      `sys.boot_completed=1` for the first time in this project's history, `adb`/`scrcpy` connected
      over the container's own Docker-bridge IP with zero port publishing needed, and a **real
      rendered UI was visible** — Android's home screen, sharp and legible, `OpenGL version: 4.6.0
      NVIDIA 595.91.07` in scrcpy's own banner.

      **Tier 5's corruption bug, seen live for the first time (through the actual intended
      use case — screen viewing/streaming, not just `screencap`), turned out to be real but
      minor**: intermittent, roughly one bad frame out of many, self-correcting on the very next
      frame, matching the non-deterministic race this project's own Tier 5 investigation had
      already diagnosed (a premature "already signaled" sync-fence answer racing the GPU's actual
      completion) rather than a deterministic, systemic corruption. Confirmed via `dumpsys media.c2`
      that this was the **stock software encoder**, not NVENC — the new Codec2 service still isn't
      deployed into any running container, so this result is the honest baseline Tier 7 is meant to
      improve on, not a contaminated measurement. Practically: the device is fully usable today
      exactly as it stands, corruption included — a real, useful milestone independent of whatever
      Tier 7's remaining deployment step ends up showing.

      Two smaller fixes landed the same session, both operational rather than architectural: the
      Android Setup Wizard was permanently disabled at the image level
      (`ro.setupwizard.mode=DISABLED` in `/system/build.prop`, since the guest filesystem here is
      just container layers with no dm-verity — directly editable, no `remount` dance needed),
      committed as a new image tag (`redroid-jg-15:gapps-nosetup` on `jgustavo48`) that also bakes
      in every deployment fix above so future containers need zero manual post-boot patching; and a
      real gap surfaced while validating it — two redroid containers cannot share one set of
      `/dev/binder`/`/dev/hwbinder`/`/dev/vndbinder` nodes (the second Android instance to grab the
      real binder driver crashes immediately, silently, before logcat even starts), so running more
      than one container concurrently on `jgustavo48` needs a second `binder_linux` device set —
      not yet set up, not yet needed, but now a known, understood prerequisite rather than a
      surprise for next time.

      **Fifth session, next day — the service actually deployed, registered, and picked by a real
      app for the first time, and the real remaining bug is now a data problem, not a plumbing
      one.** Checked `redroid-hwenc`'s own history first, since it already solved this exact class
      of deployment problem for VA-API — corrected a wrong assumption in this project's own
      `patches/codec2/README.md` along the way (no need to disable the stock service at all; the
      stock store is named `/software`, a new `/default` store coexists fine). Resolved the full
      transitive shared-library closure in one pass (`readelf -d` + a recursive walk against the
      AOSP build's own `vendor/lib64/`, 45 libraries, deployed as one tarball) instead of the
      slow one-crash-per-fix loop every earlier session used — a real process improvement worth
      keeping. Found and fixed two real deployment gaps beyond that: `androidboot.use_redroid_c2=1`
      needed adding to the container's own `/init` cmdline args (redroid's `docker run` `CMD` *is*
      `/init`'s argv), and the deployed `/vendor/etc/media_codecs.xml` predated the
      `<MediaCodec name="c2.hardware.encoder.h264">` line already sitting correctly in the source
      tree — redeployed the file wholesale after diffing to confirm that was the only difference.
      **Result: `c2.hardware.encoder.h264 (hw) [vendor]` appeared in a real `scrcpy
      --list-encoders` run** — the same milestone line redroid-hwenc's own DEVLOG recorded for
      VA-API, now true for NVENC. Driving it for real hit exactly the next wall redroid-hwenc's own
      history predicted: the component's `cros_gralloc_handle_t` size/magic validation rejects the
      real Surface-sourced input buffer outright (`numFds=1 numInts=46`, vs. 36 for a genuine
      `cros_gralloc_handle_t`) — confirming, on this project's own NVIDIA stack, the same finding
      `VaapiEncComponent`'s own header already documented on AMD/Intel: a real captured frame does
      not arrive as the gralloc HAL's own native struct. Added temporary diagnostic logging,
      rebuilt (~17s, incremental), and captured real, consistent raw bytes to decode from — width
      (720), height (1280), and a DRM fourcc (`ABGR8888`) all recognizable at fixed offsets, plus a
      stride-shaped value matching Tier 5's own already-known `3072`-byte stride for this exact
      buffer — but the wrapper type itself isn't yet identified (its apparent magic constant,
      `0xabcddcba`, matches nothing in this project's own AOSP tree, so it's very likely produced by
      a prebuilt component with no local source). See DEVLOG's 2026-09-26 entry for the full
      byte-level detail and the four deployment bugs' exact fixes.

      **What's left, cleanly scoped for the next session**: turn this session's raw byte capture
      into a second, fallback parsing path in `NvencEncComponent::process()` — exactly mirroring how
      `VaapiEncComponent` itself grew a non-cros_gralloc fallback for its own equivalent discovery —
      then get an actual encoded frame out the other end, closing Tier 7 completely and, if the
      theory holds, eliminating the earlier session's observed software-encoder corruption entirely
      by construction (NVENC never reads the tiled surface through the CPU at all). See
      [`reference_jgustavo48_redroid_host_prerequisites`] (this project's own working memory,
      external to the repo) for the host-prerequisite checklist this session re-applied after a
      clean reboot.

Even if it doesn't go further, each tier on its own is a publishable contribution.

## Why a separate repo

redroid-hwenc went from "nobody has published a fix in 4 years" to a real, working, two-vendor
solution — see [redroid-hwenc's own DEVLOG](https://github.com/fogelmanjg/redroid-hwenc/blob/main/DEVLOG.md)
(a different repo than this one) for exactly how, session by session, bugs and all. This is the same kind of problem, for a GPU vendor
where the starting line is further back. Separate repo because the actual technical work is
unrelated (this is about rendering/gralloc first, not encode), but the same intent: solve it in
public, document the real process — including the dead ends — as it happens.

## Contributing

Same as redroid-hwenc: no CLA, no friction, plain Apache-2.0. If you've got NVIDIA hardware and
want to help chase the `gpuMode=host` boot crash, or you've solved a piece of this already, a PR
or an issue comment is worth more than asking for permission first.

## License

Apache License 2.0 — see [LICENSE](LICENSE).
