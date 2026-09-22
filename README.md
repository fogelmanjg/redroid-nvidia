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

- **`gpuMode=host` doesn't boot on NVIDIA.** SurfaceFlinger crashes (`SIGABRT` in
  `SkiaGLRenderEngine::chooseEglConfig`) — a real EGL config negotiation failure between whatever
  gralloc redroid falls back to and NVIDIA's proprietary EGL/GL stack. redroid's own vendor
  gralloc (`gralloc.redroid.so`, built around Mesa/GBM assumptions) isn't even the one in play —
  Mesa's own client library falls back to a generic implementation first, and that's what fails to
  agree on a config with NVIDIA.
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
- **Prior art exists and actually works, but doesn't fit this project's constraints**:
  [waydroid-nvidia](https://github.com/Shiro836/waydroid-nvidia) gets real GPU-accelerated
  Android-in-container on NVIDIA working (verified: Minecraft Bedrock, 2ms present-to-present
  latency). Its approach — proxy Vulkan (Mesa Venus) from the Android guest over a Unix socket to a
  host-side renderer, allocate buffers host-side as native NVIDIA images, hand them to the consumer
  as NVIDIA dmabufs — sidesteps the cross-vendor EGL/gralloc negotiation problem entirely, which is
  exactly the class of bug hit here. **The catch**: it requires a real Wayland compositor already
  running on the host (verified against KWin/Plasma) — the Android container renders *through*
  that existing desktop session. That's the opposite of headless operation, which matters for
  running this on a bare server with nothing but Docker. Porting the approach as-is would mean
  giving up headless operation specifically on NVIDIA hosts. The most concrete lead that exists for
  this problem today, even so.
- Untested variables worth tracking if revisited: `nvidia-open`/`nvidia-open-dkms` kernel modules
  vs. the classic proprietary blob (not yet confirmed which one the test machines were running),
  driver version (waydroid-nvidia's write-up specifies 595.71+ with `nvidia-drm.modeset=1`), and
  GPU generation (Turing/RTX 20 or newer confirmed working there; the Pascal-era GTX 1050 Ti used
  for testing here is older than that — the RTX 4060 (Ada) in this same fleet would qualify).

## Roadmap

Nothing below is started as active work yet — this is the shape of the problem, not a schedule.

- [ ] **Get `gpuMode=host` to boot on NVIDIA at all.** The actual blocker, now precisely scoped:
      Android's guest-side Mesa GBM (bionic) cannot load NVIDIA's real GBM backend (glibc) at all
      — confirmed a genuine cross-libc/cross-namespace wall, not a config gap (see DEVLOG
      2026-09-22). `waydroid-nvidia`'s Venus-proxy approach — route rendering through a host-side
      process that *can* load NVIDIA's real stack, instead of expecting the Android guest to do it
      directly — is the strongest lead, now confirmed by direct evidence rather than analogy.
      Adapting it to work headless (no real Wayland compositor) is the open question.
- [ ] **Confirm real 3D acceleration works end to end** once boot succeeds — not just "doesn't
      crash," an actual rendered frame, the same bar redroid-hwenc held itself to for encode.
- [ ] **Hardware video decode.** Should become reachable once `gpuMode=host` genuinely works —
      `nvidia-vaapi-driver` already provides VA-API decode; the Codec2 side of that story hasn't
      been investigated at all yet in this context.
- [ ] **Hardware video encode (NVENC).** The actual encode goal, structurally similar to what
      redroid-hwenc solved for VA-API — likely a host-side daemon speaking NVENC instead of
      VA-API, reusing whatever of that project's architecture (Codec2 component shape, protocol
      design) still applies once the encode-specific parts are swapped. Not started.

## Why a separate repo

redroid-hwenc went from "nobody has published a fix in 4 years" to a real, working, two-vendor
solution — see [its DEVLOG](https://github.com/fogelmanjg/redroid-hwenc/blob/main/DEVLOG.md) for
exactly how, session by session, bugs and all. This is the same kind of problem, for a GPU vendor
where the starting line is further back. Separate repo because the actual technical work is
unrelated (this is about rendering/gralloc first, not encode), but the same intent: solve it in
public, document the real process — including the dead ends — as it happens.

## Contributing

Same as redroid-hwenc: no CLA, no friction, plain Apache-2.0. If you've got NVIDIA hardware and
want to help chase the `gpuMode=host` boot crash, or you've solved a piece of this already, a PR
or an issue comment is worth more than asking for permission first.

## License

Apache License 2.0 — see [LICENSE](LICENSE).
