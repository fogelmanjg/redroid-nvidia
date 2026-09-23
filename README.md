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
- [ ] **Tier 4 — ⭐ Real integration into redroid — both halves of the Tier 0 wall now cracked.**
      redroid ships *two* gralloc HALs as prebuilts: `gralloc.gbm.so` (Mesa, no NVIDIA support) and
      `gralloc.cros.so` (minigbm's real backend-dispatch system) — `gpu_config.sh` never selected
      `cros`. Wrote a real minigbm backend (`nvidia_venus.c`, driver name `nvidia-drm`) allocating
      over the same vtest wire protocol proven standalone in Tier 3, patched `gpu_config.sh` to
      select `cros` + `ro.hardware.egl=angle` + Venus props (`ro.hardware.vulkan=virtio`,
      `mesa.vn.debug=vtest`, `mesa.vtest.socket.name=...`) for NVIDIA, fixed two real missing-
      library issues (`libdmabufheap.so` via a same-partition vendor copy, `libdrm.so` via a
      symlink to this image's versioned `libdrm.so.2`). **Result**: `gralloc.cros.so` genuinely
      loads and allocates (`Using gralloc0 CrOS API`, no more crash loop) — the allocation half of
      the wall from Tier 0. Then, deployed against a real `virgl_test_server` with the vtest
      socket bind-mounted into the container: **SurfaceFlinger's RenderEngine genuinely stands up
      a Vulkan device against the real GPU through Venus and reports it by name** —
      `ANGLE (NVIDIA, Vulkan 1.1.274 (NVIDIA Virtio-GPU Venus (NVIDIA GeForce RTX 4060)))` — the
      "no suitable EGLConfig" abort chased since Tier 0 is completely gone. Still crashes, in
      Android's shader-cache-priming step (`output buffer not gpu writeable`) — **investigated
      further and ruled out the easy explanations**: the allocation genuinely succeeds
      (confirmed via added logging: real render-capable request, real success, real fd), the
      request itself is correct (traced to AOSP's own `Cache.cpp`, which explicitly asks for
      `GRALLOC_USAGE_HW_RENDER`), and disabling shader-cache priming step by step (real, official
      `debug.sf.prime_shader_cache.*` properties) doesn't fix it — the same buffer fails identically
      no matter which of the dozen+ draw calls touches it first, meaning the buffer itself never
      becomes genuinely GPU-writable, not a priming-step-specific bug. Next, well-scoped step:
      trace the `cros_gralloc_buffer.cc` → AHardwareBuffer → ANGLE Vulkan-import path specifically,
      since both endpoints (host-side Vulkan image creation, this backend's own allocation) are now
      independently confirmed correct. See DEVLOG for the full trail.
- [ ] **Tier 5 — Confirm real 3D acceleration end to end.** Same bar redroid-hwenc held itself to
      for encode: an actual verified rendered frame, not just "doesn't crash."
- [ ] **Tier 6 — Hardware video decode.** Should become reachable once Tier 5 is solid —
      `nvidia-vaapi-driver` already provides VA-API decode; the Codec2 side of that story hasn't
      been investigated at all yet in this context.
- [ ] **Tier 7 — Hardware video encode (NVENC).** The actual encode goal, structurally similar to
      what redroid-hwenc solved for VA-API — likely a host-side daemon speaking NVENC instead of
      VA-API, reusing whatever of that project's architecture (Codec2 component shape, protocol
      design) still applies once the encode-specific parts are swapped.

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
