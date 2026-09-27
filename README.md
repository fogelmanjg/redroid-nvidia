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

NVIDIA didn't have that foundation when this project started:

- **`gpuMode=host` didn't boot on NVIDIA.** `gralloc.redroid.so` (Mesa's GBM, built against
  Android's bionic libc) genuinely ran but failed at `gbm_create_device()` with `EINVAL` — no
  driver-table entry for NVIDIA's proprietary stack, only for `nouveau`. A real cross-libc wall,
  not a config gap. **Still true today** — nothing patches this native path directly — see
  "Current status" below for the different path this project built instead.
- **`gpuMode=guest` (pure software rendering) does work** as a fallback, but that's no
  acceleration at all.
- **Hardware video encode can't reuse redroid-hwenc's approach at all.** `nvidia-vaapi-driver` is
  decode-only — NVIDIA's real hardware encoder (NVENC) is a different, proprietary API, not
  VA-API.

## Current status

Both gaps have a **working solution today**, through a different mechanism than the native path
above: a custom Venus-proxy pipeline (guest Mesa Venus → a host-side `virglrenderer` process →
real Vulkan/NVENC against the real driver), confirmed booting to a real, rendered Android home
screen with real GPU acceleration, and real hardware video encode via NVENC producing correct,
decodable H.264 — both confirmed on **two** real GPUs (GTX 1050 Ti, RTX 4060). Hardware video
*decode* turned out not to need any of this: it's the same VA-API mechanism
[redroid-hwenc](https://github.com/fogelmanjg/redroid-hwenc)'s own daemon already speaks for
AMD/Intel, folded in there instead of duplicated here (Tier 6 below). One open item remains,
isolated to a real driver-level limitation on the RTX 4060 specifically (Tier 7 below) — not a
blocker for the 1050 Ti, and not a flaw in this project's own code.

## Pre-existing footguns (worth checking before you start)

Found during initial investigation, independent of the GPU-acceleration work itself:

- **`loop`/`ext4` kernel modules not loaded** blocks redroid from booting *at all* on a fresh
  NVIDIA host, with a confusing downstream symptom (`vold` failing) unrelated to the real cause.
  `modprobe loop ext4` fixes it.
- **`/dev/dri/renderD128` inside the container is `root:992`**, and `surfaceflinger` isn't in that
  group — host-side `chmod` has no effect since `nvidia-container-toolkit` recreates the node
  fresh inside the container. Fix from *inside* the running container instead.
- **`NVIDIA_DRIVER_CAPABILITIES` defaults to `utility,compute` only** — no `graphics`/`video`/
  `display` — unless set explicitly (`NVIDIA_DRIVER_CAPABILITIES=all`).

See DEVLOG's 2026-09-19 entries for the full diagnosis trail on each.

## Roadmap (by difficulty, not by time)

Each tier assumes the previous one is done, documented session by session in
[DEVLOG.md](DEVLOG.md) as it happens — bugs and dead ends included. This list says what was
expected and what came of it; DEVLOG.md and, where one exists, a `patches/*/README.md` have the
real story. ⭐ marks the highest-leverage checkpoint.

- [x] **Tier 0 — Get a real, precise diagnosis.** Found the actual gap (`NVIDIA_DRIVER_CAPABILITIES` silently missing `graphics`/`video`) and, once fixed, the real blocker underneath: Android's bionic-built Mesa GBM has no driver-table entry for NVIDIA and no way to reach NVIDIA's real (glibc-only) GBM backend — confirming a Venus-proxy approach is the right direction. → DEVLOG 2026-09-19/22
- [x] **Tier 1 — Check what redroid's own Mesa already ships before porting anything.** `host` mode is plain native Mesa GBM, no forwarding logic active — but the vendor partition already ships a *dormant* guest-side Venus driver, just never activated. → DEVLOG 2026-09-22
- [x] **Tier 2 — ⭐ Understand `waydroid-nvidia`'s Venus-proxy architecture.** Confirmed its Wayland requirement is incidental (display sink only, not part of the rendering pipeline) — redroid's own headless hwcomposer never needs it. → DEVLOG 2026-09-22
- [x] **Tier 3 — ⭐ Minimal headless host-side renderer prototype.** A real Vulkan compute shader, dispatch to readback, confirmed correct over the vtest wire protocol against the real RTX 4060 — no Android anywhere in the loop. → DEVLOG 2026-09-22
- [x] **Tier 4 — ⭐ Real integration — booted to the Android home screen with real NVIDIA GPU acceleration through Venus, for the first time.** Wrote a real minigbm backend (`nvidia_venus.c`) and hand-ported `waydroid-nvidia`'s own guest-side Mesa patches. Several independent bugs found along the way (a raw-fd-vs-GEM-handle bug, two missing Venus guest capabilities, a 32-bit ABI truncation). → [patches/mesa/README.md](patches/mesa/README.md), [patches/minigbm/README.md](patches/minigbm/README.md), DEVLOG 2026-09-22/24
- [ ] **Tier 5 — Confirm real 3D acceleration end to end. In progress: renders correctly, one CPU-readback corruption bug not yet fixed.** Root-caused to Skia's `RenderEngine` hardcoding `VK_IMAGE_TILING_OPTIMAL`, so a CPU reader sees the driver's real tiled layout as if it were linear. The direct fixes tried (forcing `LINEAR`, a driver-native linear allocation) both made it worse for independent reasons — the real fix is a separate untiling copy step, not yet implemented. **Shelved in favor of Tier 7**: NVENC reads the tiled surface directly and never hits this path, so it may make this fix unnecessary for the video use case specifically. **Doesn't reproduce on a GTX 1050 Ti** — likely an Ada Lovelace/driver-specific issue, not this project's own code. → DEVLOG 2026-09-24/27
- [x] **Tier 6 — Hardware video decode. Closed by reference (2026-09-27)**: the same VA-API mechanism [redroid-hwenc](https://github.com/fogelmanjg/redroid-hwenc)'s own daemon already speaks for AMD/Intel encode, with NVIDIA folded in as a fourth GPU rather than duplicated here — fully integrated (daemon dispatch + a real `c2.hardware.decoder.h264` component) and verified bit-exact. → [redroid-hwenc's DEVLOG](https://github.com/fogelmanjg/redroid-hwenc/blob/main/DEVLOG.md), 2026-09-27 entries
- [ ] **Tier 7 — ⭐ Hardware video encode (NVENC). Correct, decodable output confirmed end to end on real hardware, from both a synthetic buffer and a real captured Android frame; one open item left, and it's a driver limitation, not this project's bug.** A host-side daemon design like redroid-hwenc's, but through CUDA's external-memory interop (NVENC isn't VA-API) instead of a plain dma-buf import — including a new vtest command (`VCMD_ENCODE_RESOURCE`) so the daemon needs zero changes to Venus's own resource-creation code, and an `SCM_RIGHTS` transport (mirroring redroid-hwenc's own daemon) for buffers Venus never allocated, like a real `Surface` capture. `c2.hardware.encoder.h264 (hw)` now appears in a real `scrcpy --list-encoders` run and produces a correctly positioned, correctly colored, `ffprobe`-valid recording. **What's left**: a low-severity scanline-dropout artifact on the RTX 4060 specifically, root-caused to a real driver-level limitation — a "prime fence" import failure in driver 595.91.07 that also affects Android's own compositor independent of NVENC — confirmed **absent** on a GTX 1050 Ti with an older driver. That older driver isn't viable on the 4060 itself (kernel module incompatibility, then a hardware hang, then confirmed-missing GSP firmware for Ada Lovelace); next untried candidate is driver branch 590. → [patches/virglrenderer/README.md](patches/virglrenderer/README.md), [patches/codec2/README.md](patches/codec2/README.md), [patches/nvenc-daemon/README.md](patches/nvenc-daemon/README.md), DEVLOG 2026-09-25/27

Even if it doesn't go further, each tier on its own is a publishable contribution.

## Hardware compatibility

| GPU | Architecture | 3D accel (Tier 4/5) | NVENC encode (Tier 7) | Notes |
|---|---|---|---|---|
| GTX 1050 Ti | Pascal | Boots, renders | Correct, clean | Reference for "clean" — neither Tier 5's corruption nor Tier 7's driver artifact reproduce here. |
| RTX 4060 | Ada Lovelace | Boots, renders (Tier 5's corruption present) | Correct, one open artifact | Both open items are the *same* underlying driver limitation (driver 595.91.07's "prime fence" import) — see Tier 5/7 above. |

## Why a separate repo

redroid-hwenc went from "nobody has published a fix in 4 years" to a real, working, two-vendor
solution — see [redroid-hwenc's own DEVLOG](https://github.com/fogelmanjg/redroid-hwenc/blob/main/DEVLOG.md)
for exactly how, session by session, bugs and all. This is the same kind of problem, for a GPU
vendor where the starting line is further back. Separate repo because the actual technical work is
unrelated (this is about rendering/gralloc first, not encode), but the same intent: solve it in
public, document the real process — including the dead ends — as it happens.

## Contributing

Same as redroid-hwenc: no CLA, no friction, plain Apache-2.0. If you've got NVIDIA hardware and
want to help — more GPU generations/driver versions for the compatibility table, chasing the RTX
4060's remaining driver-level artifact, or even the native `gpuMode=host` boot crash this project
ended up routing around instead of fixing directly — a PR or an issue comment is worth more than
asking for permission first.

## License

Apache License 2.0 — see [LICENSE](LICENSE).
