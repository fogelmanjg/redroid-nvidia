# Devlog

Session-by-session log — what was tried, what worked, what didn't, and why. See the
[README](README.md) for the current shape of the problem and the roadmap.

The first entries below happened inside
[redroid-hwenc](https://github.com/fogelmanjg/redroid-hwenc)'s own devlog, while that project was
checking whether its AMD/Intel VA-API encode work would extend to NVIDIA too. It doesn't — NVIDIA
needs its own rendering foundation first — so this history moved here, where the actual ongoing
work lives. Copied verbatim rather than summarized, so nothing about the reasoning gets lost in
translation.

## 2026-09-19 — NVIDIA: real architecture finding, plus an unrelated blocker

Set up a fresh machine from scratch for this one: Docker CE, `nvidia-container-toolkit`
(confirmed working — `docker run --gpus all ... nvidia-smi` succeeds inside a container), and
`nvidia-vaapi-driver` (Debian packages it: `nvidia-vaapi-driver 0.0.13-1`).

**Real finding, worth designing around now rather than discovering later:** unlike Mesa
(AMD/Intel), which bundles VA-API encode support automatically, `nvidia-vaapi-driver` is
**decode-only**. `vainfo` with it loaded lists plenty of decode profiles
(H.264/HEVC/VP8/VP9/AV1, all `VAEntrypointVLD`) and **zero** `VAEntrypointEncSlice` entries. The
RTX 4060 (Ada Lovelace) obviously has strong hardware encode — but it's exposed through NVENC,
NVIDIA's own proprietary API, not through VA-API. This means a future hardware Codec2 component
can't be a single VA-API backend for every vendor — it needs a vendor split: VA-API for AMD/Intel
(see redroid-hwenc), a separate NVENC-based path for NVIDIA (here).

**Unrelated blocker, not chased to a conclusion that night:** redroid itself won't boot on this
particular fresh install. `vold` and `blank_screen` — and only those two, both perfectly normal
64-bit x86-64 PIE binaries with the same `/system/bin/linker64` interpreter as everything else —
fail with `cannot execv(...): No such file or directory` moments after their process is forked,
while `hwservicemanager`/`servicemanager` start fine. Reproduces identically with `--gpus all`
removed and with `androidboot.redroid_gpu_mode=guest` (pure software rendering, no GPU
dependency at all) — so it's not GPU/NVIDIA-toolkit related. Ruled out: image corruption (layer
ID matches the source host exactly), binder device permissions (fixed a real `chmod 666` miss
along the way, different bug, didn't fix this one), storage driver (identical overlay2-on-btrfs
setup works fine on another machine), 32-bit/IA32 emulation (both binaries are 64-bit; kernel
has `CONFIG_IA32_EMULATION=y` anyway), and container-level AppArmor confinement (profile is
correctly `unconfined` for this `--privileged` container). Leading suspicion: a mount-namespace
propagation quirk specific to this fresh Debian trixie + Docker 29.8.1 + containerd 2.3.5
install, since both failing services do their own mount-namespace work at start. Not resolved —
parked separate from the (already answered) VA-API question above.

## 2026-09-19 (same day) — Hunted the boot blocker down: two missing host kernel modules, then the real NVIDIA issue

Picked this back up on a second, independent fresh host (a GTX 1050 Ti machine, also a from-
scratch Docker + nvidia-container-toolkit install) specifically to tell apart "quirk of one
machine" from "something structural." Same crash, identical `cannot execv` signature. Confirmed
this had nothing to do with Docker/containerd version either — downgraded to the exact versions
running on the machine where redroid boots fine (Docker 29.7.2, containerd 2.3.3); crash
persisted unchanged.

The real trail was earlier in `dmesg`, above where the `execv` errors show up (easy to miss if
you only grep for the visible symptom):

```
apexd-bootstrap: Failed to activate .../com.android.tzdata.apex: Could not create loop device
  for .../com.android.tzdata.apex: Failed to open loop-control: No such device
init: Service apexd-bootstrap has 'reboot_on_failure' option and failed, shutting down system.
```

`apexd-bootstrap` — the thing that mounts Android's APEX modules, itself a hard boot dependency
— was failing because **the `loop` kernel module wasn't loaded on the host.** `/dev/loop-control`
existed as a stale device node, but nothing backed it (`lsmod | grep loop` was empty), so opening
it returned ENODEV. That failure trips `apexd-bootstrap`'s `reboot_on_failure`, which makes
Android's init do its own internal soft-reboot — and it's a strictly worse state *after* that
soft-reboot that produces the `vold`/`blank_screen` `execv` errors seen in the earlier entry.
Those were downstream noise, not the cause.

`modprobe loop` got loop devices created, but the APEX *mount* still failed
(`Mounting failed for .../com.android.tzdata.apex: No such device`) — this time because
**`ext4` wasn't registered as a filesystem in the kernel either** (`cat /proc/filesystems` had
no `ext4` line at all, vs. a working host which listed it with `ext4`/`mbcache`/`jbd2` loaded).
`modprobe ext4` fixed that too.

With both modules loaded, redroid boots almost all the way: `vold`, `apexd`, `adbd`,
`gpuservice`, the graphics composer service — all come up clean. **The actual remaining failure
is now squarely an NVIDIA/Mesa incompatibility, not a host config gap:**

```
MESA: Using gralloc header from libdrm/android/gralloc_handle.h. [...] Initializing a fallback
  gralloc as a helper: Using fallback gralloc implementation
libc: Fatal signal 6 (SIGABRT) [...] in tid ... (surfaceflinger)
  #03 SkiaGLRenderEngine::chooseEglConfig
  #04 SkiaGLRenderEngine::create
```

redroid's own vendor gralloc (`gralloc.redroid.so`, built against Mesa/GBM assumptions) isn't
being used — Mesa's client library falls back to a generic gralloc implementation instead, and
SurfaceFlinger's Skia-based render engine then aborts trying to pick an EGL config against
whatever that fallback actually hands it through NVIDIA's proprietary EGL/GL stack. This crashes
`surfaceflinger` in a loop (`exited 4 times before boot completed`), which is why
`sys.boot_completed` never gets set.

**Where this leaves NVIDIA support:** it's not a missing package or a config flag this time — it's
a real vendor HAL incompatibility between redroid's Mesa-oriented gralloc/hwcomposer and NVIDIA's
driver stack. `gpuMode=guest` (pure software rendering, no GPU driver in the loop) should sidestep
this entirely and is worth confirming as a fallback; getting `gpuMode=host` genuinely working on
NVIDIA would mean either an NVIDIA-aware gralloc/hwcomposer HAL (nobody seems to have published
one for redroid) or something narrower fixing just this EGL config negotiation — not yet
investigated further.

**Worth carrying back to `redroid-manager`'s Doctor, independent of the NVIDIA question:** missing
`loop`/`ext4` kernel modules is a real, generic footgun for anyone deploying redroid on a host
that's never needed them before — cheap, high-value checks to add.

## 2026-09-19 (same day) — NVIDIA options, tried in order: guest mode works, two real sub-bugs found, core issue confirmed

Went through the cheap options before assuming a full vendor HAL rewrite is necessary.

**`gpuMode=guest` (pure software rendering) confirmed working on NVIDIA** — boots clean in ~14s,
completely sidesteps the gralloc/EGL crash since no GPU driver is involved at all. Not a real fix
(no hardware acceleration), but a legitimate practical fallback, and useful confirmation that
everything *else* (loop/ext4/binder/Codec2) is solid on this hardware — the remaining problem is
narrowly scoped to `gpuMode=host`'s graphics HAL.

**Checked whether NVIDIA's GBM/Vulkan pieces are actually reaching the container** (they need to,
for any Mesa-side fix to have a chance): confirmed complete. `nvidia-container-toolkit`'s CDI
spec (`/var/run/cdi/nvidia.yaml`) injects `libnvidia-egl-gbm.so`, `nvidia-drm_gbm.so`, the EGL
external platform configs, and the Vulkan ICD (`nvidia_icd.json`) into the container. Nothing
missing here — ruling this out as the cause narrows the problem to an actual negotiation failure,
not an incomplete environment.

**Tried forcing Zink** (Mesa's OpenGL-over-Vulkan driver, hoping to route around NVIDIA's
native EGL/GBM path entirely) via `MESA_LOADER_DRIVER_OVERRIDE=zink` / `GALLIUM_DRIVER=zink` env
vars on the container. Inconclusive as a fix — the crash signature didn't change, and log
evidence suggests these env vars don't actually get honored by the code path in play here
(Android's `platform_android` EGL backend, not a normal desktop Mesa app), so this wasn't a real
test of Zink, just a reminder that the override point needs to be found more precisely if this
route gets revisited.

**Found and fixed a real, separate bug along the way:** the crash logs (once actually read
closely, past the vold/blank_screen detour) showed `EGL-MAIN: failed to open
/dev/dri/renderD128: Permission denied` before the "fallback gralloc" messages. The device node
`--gpus all` creates inside the container is `crw-rw---- root:992` — and `surfaceflinger` runs as
Android's `system` AID, which isn't a member of that group inside the container's (Android-only)
user model. Chmod'ing the device from the *host* has no effect (nvidia-container-toolkit
recreates the node fresh inside the container, not a true bind-mount of the host's permission
bits) — fixed it with `docker exec ... chmod 666 /dev/dri/*` from inside the running container
instead.

**With that permission bug fixed, the actual root cause is confirmed, cleanly, with no more
noise in the way:** `RenderEngine: no suitable EGLConfig found, giving up`, in
`SkiaGLRenderEngine::chooseEglConfig`. Device access is no longer the problem — this is a genuine
EGL config negotiation failure between whatever Mesa's Android-platform fallback gralloc offers
and what NVIDIA's EGL implementation is willing to hand back. redroid's own vendor gralloc
(`gralloc.redroid.so`) apparently isn't even the one being used here (Mesa's log explicitly says
it's using a "fallback gralloc" instead) — so there are really two layered questions now: (1) why
does `gralloc.redroid.so` decline to handle NVIDIA in the first place, and (2) why does Mesa's
own fallback then fail to agree on an EGL config with NVIDIA specifically. Neither answered yet.

**Where this leaves it:** the cheap options are exhausted and didn't produce a fix — this really
does look like it needs actual HAL-level work, not a config/env-var nudge. `gpuMode=guest` stays
the practical fallback for NVIDIA hosts in the meantime.

## 2026-09-19 (same day) — Prior art exists, but it doesn't fit redroid's headless model

Before assuming a HAL rewrite from scratch, searched for whether anyone solved the equivalent
problem in a similar project. They have:
[**waydroid-nvidia**](https://github.com/Shiro836/waydroid-nvidia) gets full GPU-accelerated
Android-in-container on NVIDIA working — real, verified (Minecraft Bedrock, 2ms present-to-present
latency benchmarks). The approach: proxy Vulkan (Mesa Venus) from the Android guest over a unix
socket to a host-side renderer, allocate buffers host-side as NVIDIA block-linear images, and
hand them to the consumer as native NVIDIA dmabufs — no cross-vendor EGL/gralloc negotiation at
all, which is exactly the class of problem hit here.

**The catch that matters for this project:** this architecture requires a real Wayland compositor
(KWin/Plasma, verified) already running on the host — the Android container renders *through*
that existing desktop session, it isn't headless. That's a real, new constraint that doesn't exist
for AMD/Intel today, and matters for the headless-server use case this whole effort cares about.
Porting this approach as-is would mean giving up headless operation specifically on NVIDIA hosts.

Other requirements worth noting if this gets revisited: `nvidia-open`/`nvidia-open-dkms` kernel
modules specifically (not the classic proprietary blob) — not yet confirmed which one was running
on the test machines. Driver 595.71+ with `nvidia-drm.modeset=1`. Turing (RTX 20/GTX 16) or
newer GPUs — the GTX 1050 Ti used for the tests above is Pascal, older than what's verified to
work; the RTX 4060 (Ada) in this same fleet would qualify. Separately, NVIDIA's developer forums
note they don't build `nvidia-utils` against bionic libc — a known ABI friction point in this
space generally, distinct from this specific bug but the same swampy territory.

Not a drop-in fix, but the most concrete lead that exists for this specific problem today.

## 2026-09-22 — Repo split off from redroid-hwenc

redroid-hwenc reached a real, confirmed-on-two-vendors Tier 5 (AMD + Intel, VA-API hardware
encode working end to end for a real app). NVIDIA needs its own foundation first (see README) and
the technical work is unrelated to VA-API encode, so this history moved here rather than
continuing to live inside a project that's actually about something else now. Nothing new
investigated yet in this entry — the four entries above are the starting point, not new findings.

## 2026-09-22 (same day) — The `gpuMode=host` crash was a false diagnosis: driver capability was never actually on

Went hunting for whether "you need a special NVIDIA-flavored Docker" (a claim floating around
online) was real, before touching architecture. It isn't, quite — `nvidia-docker2` (the old
wrapper) is deprecated and archived, fully replaced by `nvidia-container-toolkit`, which runs on
stock Docker CE (already what's installed everywhere in this fleet). But chasing it turned up
something real: `nvidia-container-toolkit` hard-codes `NVIDIA_DRIVER_CAPABILITIES` to
**`utility,compute` only** when it's unset — no `graphics`, no `video`, no `display`. Every prior
test of `gpuMode=host` on NVIDIA (see the three entries above) used bare `--gpus all`, which only
controls GPU *visibility*, never *capabilities* — and `NVIDIA_DRIVER_CAPABILITIES` was never set
anywhere. The earlier "confirmed complete" check of NVIDIA's GBM/EGL/Vulkan pieces reaching the
container was done by reading `nvidia-container-toolkit`'s generated CDI spec on the *host*
(`/var/run/cdi/nvidia.yaml`), not by checking what actually landed inside a *running* container —
that file lists mounts for every capability by construction, so it couldn't have caught this.

Tested on `jgustavo48` (RTX 4060 / Ada, driver 595.91.07, toolkit 1.20.1, driver's own
`nvidia_drm` already has `modeset=Y`). Two real bugs found and fixed on the way to a clean test:

- **`modprobe loop ext4` only loads `loop`.** Classic footgun: `modprobe` treats trailing
  arguments as *parameters* to the first module, not additional module names. `ext4` was silently
  never loaded on this fresh host despite the command appearing to succeed — reproduced the exact
  `apexd-bootstrap`/`No such device` boot failure from the very first NVIDIA entry above, on a
  machine that should've been past that. Fixed with two separate `modprobe` calls. Worth folding
  into `redroid-manager`'s Doctor checks as its own item, distinct from "are the modules loaded at
  all."
- **`/dev/binder*` and `/dev/dri/renderD128` need `chmod 666` from the host / from inside the
  running container respectively** — consistent with bugs already logged in the entries above,
  just re-confirmed on a fresh machine.

With `docker run --gpus all -e NVIDIA_DRIVER_CAPABILITIES=all ...` (graphics libraries confirmed
actually present inside the running container this time — `libnvidia-egl-gbm.so`,
`nvidia_icd.json`, `/usr/lib/x86_64-linux-gnu/gbm/nvidia-drm_gbm.so` all found via `docker exec`,
not just inferred from the host-side CDI file): **the previously-documented crash signature is
gone.** No more `SkiaGLRenderEngine::chooseEglConfig` / `Fatal signal 6`. `surfaceflinger` still
doesn't come up and boot still doesn't complete, but for a completely different, more specific
reason:

```
GRALLOC-GBM: failed to create gbm device
AllocatorHal: failed to open gralloc0 device: Invalid argument
```

**This is real progress, not just a different flavor of the same wall.** Before, Mesa's Android
gralloc was silently declining to even try and falling back to a generic implementation that then
failed to negotiate with NVIDIA's EGL — a vague, one-shot failure. Now redroid's actual vendor
gralloc (`gralloc.redroid.so` / `/vendor/lib64/libgbm.so.1`, tagged `GRALLOC-GBM` in these logs)
is genuinely running and calling `gbm_create_device()` for real, and failing at that specific
call, repeatably.

**Why it fails, reasoned through rather than guessed:** `/vendor/lib64/libgbm.so.1` inside the
container is Mesa's *own* GBM implementation, built against Android's bionic libc as part of
redroid's vendor partition — its `gbm_create_device()` dispatches by matching the DRM driver name
(via `drmGetVersion()`) against Mesa's own compiled-in DRI driver table (`i915`, `radeonsi`,
`amdgpu`, etc.). NVIDIA's driver name isn't in that table, so it fails closed — `EINVAL`, no
usable device. Meanwhile NVIDIA's *actual* real GBM backend
(`/usr/lib/x86_64-linux-gnu/gbm/nvidia-drm_gbm.so`, confirmed present and correctly injected by
CDI) sits under a completely different, glibc-only path — one that Android's bionic-linked vendor
HAL processes have no mechanism to even look in, let alone load a glibc shared object from,
independent of whether the file exists. **This is a real ABI/namespace wall between Android's
guest-side Mesa GBM and NVIDIA's host-side GBM backend, not a missing file or a config knob** —
matches the "NVIDIA doesn't build `nvidia-utils` against bionic" friction point already flagged as
a risk two entries above, now hit directly and concretely instead of just anticipated.

**Where this leaves it:** the driver-capabilities fix is real and worth keeping as standard
practice for every future NVIDIA test (`NVIDIA_DRIVER_CAPABILITIES=all` on every `docker run` from
now on) — it moved the failure from a vague dead end to a precise, understood one. But it also
closes off the hope that this was ever going to be a config-level fix: Android's guest-side gralloc
genuinely cannot talk to NVIDIA's driver directly, cross-libc, cross-namespace. That's exactly the
class of problem `waydroid-nvidia`'s Venus-proxy architecture (see two entries above) was built to
sidestep — route rendering through a host-side process that *can* load NVIDIA's real stack,
instead of expecting the Android guest to do it directly. It remains the strongest lead, now with
direct confirmation (not just analogy) that the guest-direct path is a dead end on this stack.

## 2026-09-22 (same day) — Tier 1: nothing to reuse, but a real separate bug found and a stronger confirmation of the wall

Went looking for whether redroid's own Mesa build already ships some form of host-forwarding
(gfxstream, Venus, anything) before assuming the whole guest/host proxy needs to be built from
scratch — redroid-hwenc's own notes mention this build's Mesa compiling "gfxstream/ANGLE" pieces
for Android. Checked directly inside a running container image (`redroid-jg-15:gapps-official`
on `jgustavo48`): **there is no gfxstream, no Venus, nothing host-forwarding at all.** ANGLE
(`libEGL_angle.so`) is present, but reading `/vendor/bin/gpu_config.sh` directly shows exactly
what it's for — `gpu_setup_guest()` uses it purely as an alternative **software** GLES
implementation (a pick between ANGLE and SwiftShader when no usable GPU is found), completely
unrelated to hardware forwarding. `gpu_setup_host()` only ever sets `ro.hardware.egl=mesa` /
`ro.hardware.gralloc=gbm` — plain native Mesa GBM, no proxy layer of any kind. Nothing to reuse
here; the earlier note was about what Mesa's upstream source tree *can* compile for Android in
general, not what's actually wired into this image. Tier 1 closes clean, just with a "no" instead
of a shortcut.

**Real, separate bug found while reading that script**, worth fixing independent of the NVIDIA
question: `setup_render_node()`'s auto-detect loop only recognizes a fixed driver allow-list
(`i915|amdgpu|nouveau|virtio_gpu|v3d|vc4|msm_drm|panfrost`) when picking a DRI node in `host`
mode. `nouveau` (the FOSS reverse-engineered NVIDIA driver) is in that list — but the proprietary
NVIDIA driver registers itself as `nvidia-drm` in `/sys/kernel/debug/dri/N/name` (confirmed
directly: `nvidia-drm dev=0000:07:00.0`), which matches nothing in the list. Without an explicit
`androidboot.redroid_gpu_node=` override, the auto-detect loop silently never sets
`gralloc.gbm.device` at all on an NVIDIA host, even in explicit `host` mode (whose top-level
branch doesn't check the loop's return value, so `gpu_setup_host()` runs anyway, just with that
property left unset). A real gap worth reporting upstream separately, and worth always passing
`androidboot.redroid_gpu_node=/dev/dri/renderD128` explicitly on NVIDIA hosts going forward.

**But fixing it changes nothing about the actual wall.** Re-tested with both
`androidboot.redroid_gpu_mode=host` and `androidboot.redroid_gpu_node=/dev/dri/renderD128` passed
together (confirmed via `getprop`: `gralloc.gbm.device` correctly set to `/dev/dri/renderD128`,
mode correctly `host`) — `vendor.gralloc-2-0` fails at `gbm_create_device()` with the exact same
`Invalid argument`, crash-loops `surfaceflinger` the same way, boot doesn't complete. Same result
with the device path now provably correct end to end. This rules out "the property just wasn't
set" as an alternate explanation and leaves Tier 0's diagnosis standing on firmer ground: Mesa's
own `libgbm.so.1` (this build's bionic-compiled GBM) simply has no driver backend for NVIDIA's
proprietary stack — only for `nouveau`, the open driver, which isn't what's running here. The
wall holds regardless of which correct device node it's pointed at. Tier 2 (understanding
`waydroid-nvidia`'s Venus-proxy architecture, and specifically whether its Wayland-compositor
requirement is structural or incidental) is next.

## 2026-09-22 (same day) — Tier 2: the Wayland requirement is confirmed incidental, and a correction to Tier 1's "nothing to reuse"

Read `waydroid-nvidia`'s own `docs/architecture.md` directly rather than inferring from the
README's prose. Two outcomes, one of them a real correction to what got written a few hours ago.

**The Wayland/KWin requirement is exactly as incidental as hoped, confirmed by reading the actual
pipeline, not just the marketing description:**

```
Android app ── Vulkan ──▶ guest Mesa Venus (bionic, vulkan.virtio.so)
                              │ Venus protocol over vtest unix socket
                              ▼
                     virglrenderer render server (host)
                              │ real Vulkan
                              ▼
                     NVIDIA proprietary driver ──▶ GPU
                              │ VkImage (block-linear) ─exported▶ dmabuf
                              ▼
       guest gralloc (minigbm vtest backend) imports the dmabuf
                              │
                     hwcomposer.waydroid ──▶ Wayland ──▶ KWin
```

The entire rendering path — guest Venus driver, unix-socket transport, **`virglrenderer`**
(a standalone host process, not KWin) issuing real Vulkan calls against NVIDIA, buffers coming
back as dmabufs — completes with zero involvement from Wayland or KWin. Wayland only shows up in
the *last* arrow, and it's `hwcomposer.waydroid` specifically — Waydroid's own hwcomposer HAL,
written to hand composited frames to a Wayland surface so they appear as a window on the host
desktop — that requires it, not the rendering pipeline itself. The doc's own "why host-side
allocation" section confirms this framing explicitly: the reason buffers must be NVIDIA-native
block-linear is "on a machine whose displays are on the NVIDIA GPU, **KWin** composites on
NVIDIA" — a display-time constraint, not a rendering-time one.

This matters directly for redroid: redroid already has its own hwcomposer HAL
(`vendor.hwcomposer-2-1`, already running today, headless by construction — redroid has never
needed a physical display or a compositor to produce a frame buffer). Nothing about this project
needs `hwcomposer.waydroid`, Wayland, or KWin at all — only the guest-Venus / host-virglrenderer /
gralloc-import chain above it, feeding into redroid's *own*, already-headless hwcomposer instead.

**Correction to this same day's earlier Tier 1 entry:** "nothing to reuse" was too strong.
Searched the actual redroid vendor partition (not just `gpu_config.sh`'s logic) for virtio-gpu/
Venus artifacts and found real ones, already shipped, currently dormant:

```
/vendor/lib64/hw/vulkan.virtio.so     — the exact guest Venus Vulkan driver
/vendor/lib64/dri/virtio_gpu_dri.so   — Mesa's virtio-gpu Gallium/DRI driver
```

`gpu_config.sh` even has a `virtio_gpu` case already (`setprop ro.hardware.vulkan virtio`) — it's
just never reached, because nothing on these test hosts registers a `virtio_gpu` DRI node (redroid
does GPU passthrough via a direct `/dev/dri` bind-mount, not a paravirtualized virtio-gpu device,
so the auto-detect loop never finds one to trigger it). The driver being *present* doesn't mean
the *host-forwarding infrastructure* is present — no `virglrenderer` process, no vtest socket, no
patched minigbm vtest allocation backend exist anywhere in this stack yet — so Tier 1's core
conclusion stands (nothing *wired up and working* to reuse), but "nothing to reuse *at all*" was
inaccurate. The real, corrected picture: the guest half of this bridge is already sitting in the
image; what's missing is everything on the host side, plus (per waydroid-nvidia's own component
map) a real, non-trivial patch to minigbm — their `gbm_mesa_driver/vtest_wrapper.c` is described
as "net-new," i.e. stock minigbm doesn't support vtest-based GPU allocation without it.

**Where this leaves it:** the plan is real and considerably narrower than "port Waydroid's whole
NVIDIA stack." What's actually needed: (1) a host-side `virglrenderer` process running in
vtest/venus mode against the real NVIDIA driver — the project's own patches to `virglrenderer`'s
`vtest/`/`src/venus/` are exactly this piece, (2) redroid's guest props set manually
(`ro.hardware.vulkan=virtio`, `ro.hardware.egl=angle`, `mesa.vn.debug=vtest`,
`mesa.vtest.socket.name=/dev/venus.sock`) instead of relying on the driver-name auto-detect loop,
and (3) minigbm's vtest allocation wrapper, ported from their fork since stock minigbm doesn't
have it. None of it touches Wayland, KWin, or `hwcomposer.waydroid` — redroid keeps its own,
already-headless display path throughout. Tier 3 (minimal headless host-side renderer prototype)
is next: get a bare `virglrenderer` vtest/venus server talking to the real NVIDIA driver, confirmed
working standalone, before wiring anything into redroid's boot process at all.

## 2026-09-22 (same day) — Tier 3: real Venus compute round-trip confirmed on the actual NVIDIA driver, standalone

**Prerequisite discovered first, not assumed**: `waydroid-nvidia`'s own `tests/run-probe.sh`
explicitly requires the **open** NVIDIA kernel module — the closed/proprietary `.ko` has no
DMA-BUF support, and every buffer in this whole approach is one. Checked `jgustavo48` (the RTX
4060 machine, driver 595.91.07): running the closed module (`nvidia-kernel-dkms`). Confirmed with
the user this was worth doing given how central it is (a live gaming/streaming machine, Sunshine
running on it), talked through what does and doesn't change (same userspace either way — GLX/EGL/
Vulkan/CUDA/NVENC libraries are identical, only the `.ko` differs; the one real unknown flagged
was HDMI HDR 4:4:4 output, tied to `nvidia-drm.ko`'s modesetting specifically), then swapped via
NVIDIA's own CUDA apt repo (`nvidia-open` metapackage, pinned to the exact installed 595.91.07 —
Debian's own `contrib` package for this is stuck on a stale, mismatched 550.163.01 and would have
been a real footgun). Dry-run first, confirmed a clean 1:1 swap (`nvidia-kernel-dkms` out,
`nvidia-kernel-open-dkms` in, matching versions, nothing else touched), DKMS build verified
successful for the running kernel *before* rebooting, Secure Boot confirmed disabled (so MOK
signing was a non-issue either way). Rebooted. Came back clean: `NVRM version: ... Open Kernel
Module ...`, `nvidia_drm` modeset still `Y`, Plasma/X11 session, KWin and Sunshine both back up on
their own via autologin/systemd user units, `glxinfo`/`vulkaninfo` both report the real RTX 4060.
No regressions observed; HDR/4:4:4 specifically is the one thing that needs the user's own eyes on
an actual TV connection to fully confirm.

**The actual Tier 3 test**: downloaded the project's own `v0.1.2` GitHub release
(`waydroid-nvidia-host-x86_64-v0.1.2.tar.zst`, checksum verified against the release's
`SHA256SUMS`) rather than building `virglrenderer` from source — it's exactly three files
(`virgl_test_server`, `virgl_render_server`, `libvirglrenderer.so.1`), no NVIDIA-specific host
setup needed beyond the kernel module above. Also found the glibc build of Mesa's virtio/Venus
Vulkan guest driver **already packaged in Debian** (`mesa-vulkan-drivers` ships
`libvulkan_virtio.so` + `virtio_icd.json`) — no need to cross-compile Mesa for this standalone
test at all; that's only required later for the actual Android/bionic guest side.

Two real bugs, found and fixed in order:

1. `virgl_test_server --venus --use-egl-surfaceless --rendernode /dev/dri/renderD128
   --socket-path <path>` starts silently either way — success or failure both produce no output by
   default. The vtest socket got created either way, so "the socket exists" is not proof the
   server is healthy. Needed `VIRGL_LOG_LEVEL=debug` to get real diagnostics at all.
2. With that on, the actual failure was `proxy: failed to exec
   /usr/local/libexec/virgl_render_server: No such file or directory` — `virgl_test_server` forks
   a sandboxed helper process (`virgl_render_server`, the one that actually holds the Vulkan
   context) at a **hardcoded** path, not relative to wherever the binaries happen to sit. The
   release tarball ships it as a plain file alongside the others, expecting the AUR
   package/systemd unit to install it to that path — running ad hoc from an extracted tarball
   needs that done manually. Fixed with a plain `cp` to `/usr/local/libexec/virgl_render_server`
   (creating the dir) plus `libvirglrenderer.so.1` into `/usr/local/lib` + `ldconfig` so the helper
   process's own dependency resolves too.

**With both fixed, the real test — `tests/vnprobe.c`, compiled locally against Debian's system
`libvulkan-dev`** (a real Vulkan compute pipeline: shader module, descriptor set, command buffer,
dispatch, fence wait, mapped-memory readback, checked value-by-value against the expected
computed result — not a toy, the same rigor this project holds itself to elsewhere) **— passed
outright, first real run after both fixes**:

```
device: Virtio-GPU Venus (NVIDIA GeForce RTX 4060)
PASS: 65536 elements computed correctly on 'Virtio-GPU Venus (NVIDIA GeForce RTX 4060)'
```

The full chain — Debian's packaged Mesa Venus Vulkan driver (client) → `VTEST_SOCKET_NAME` unix
socket → `virgl_test_server` → `virgl_render_server` → the real, open-module NVIDIA driver → RTX
4060 → 65536 correctly computed values read back — works standalone, no Wayland, no redroid, no
Android anywhere in the loop yet. This is the exact shape of bridge redroid needs, proven to
actually carry real GPU work end to end on this hardware before touching redroid's boot process at
all. Tier 4 (real integration into redroid) is next: get this same host renderer reachable from
inside a redroid container, with redroid's *own* dormant guest Venus driver
(`vulkan.virtio.so`, found in Tier 2) talking to it instead of this standalone test client.

## 2026-09-22 (same day) — Tier 4, first real finding: the prebuilt gralloc wrapper isn't a drop-in, checked before assuming

Started Tier 4 by checking whether `waydroid-nvidia`'s own guest-side release could shortcut
things the way the host release did for Tier 3. Downloaded and checksum-verified
`waydroid-nvidia-guest-android-x86_64-v0.1.2.tar.zst` and `-guest-prebuilts-v0.1.2.tar.zst`.
Found exactly the file Tier 2 flagged as genuinely missing — `vendor/lib64/libgbm_mesa_wrapper.so`,
the built vtest gralloc backend — plus their own `vulkan.virtio.so` build and (not needed here)
their `hwcomposer.waydroid.so` and a patched LineageOS 20 `surfaceflinger` binary, both specific
to Waydroid's own Android base and not something to mix into redroid's.

**Checked whether it's actually a drop-in before assuming it is**, since redroid's real
`gralloc.gbm.so` was already read directly in Tier 1/2: `strings` on it shows it links straight
against `libgbm.so.1` and calls the standard GBM C ABI (`gbm_create_device`, etc.) — no
indirection. `readelf --dyn-syms` on their `libgbm_mesa_wrapper.so`, by contrast, exports exactly
**one** symbol: `get_gbm_ops` — a custom vtable-style entry point, not the standard GBM ABI at
all. This means their file only works with a gralloc dispatcher that already knows to `dlopen()`
it and call `get_gbm_ops()` — a patch to minigbm's *own* internal backend-selection logic that
lives in Waydroid's fork, not in redroid's stock, unpatched gralloc/minigbm. Dropping the file
into redroid's vendor partition as-is would do nothing; nothing in redroid's `gralloc.gbm.so`
would ever call it.

**Where this actually leaves Tier 4**: the "one real missing piece" framing from Tier 2 was
correct about *what's* missing (a vtest-aware GBM allocation backend) but understated *how* it
has to be delivered for redroid specifically — not as a foreign wrapper file, but as a proper
minigbm build with the vtest backend compiled in *as a normal backend*, exporting the standard
`gbm_*` symbols redroid's `gralloc.gbm.so` already calls, so it becomes a genuine drop-in
replacement for the vendor partition's existing `libgbm.so.1`. That means building minigbm from
source against the Android NDK, with `waydroid-nvidia`'s `src/minigbm-vtest/vtest_wrapper.c` (or
the reasoning in it) adapted into a standard backend rather than used through their custom
dispatch hook — real cross-compile work, not a config change or a binary copy, and meaningfully
more than what Tiers 0-3 needed. Their own `vulkan.virtio.so` build is likely fine to reuse as-is
(same standard Vulkan ICD entry points redroid's own copy already uses) — the gralloc layer is
the one piece that needs real source work. Paused here to decide how deep to go on this build
before spending the hours it needs.

## 2026-09-22 (same day) — Tier 4: a real minigbm backend, built end to end, blocked by a linker namespace mismatch

Went ahead with the real build. Found something that changes the whole plan for the better first:
a full local AOSP checkout for this exact redroid build already exists on disk
(`~/aosp-redroid-15`), including `external/minigbm` — the actual source this image's gralloc HALs
trace back to, and a working build container (`redroid-builder`) already used for prior work.

**Real discovery, checked rather than assumed**: `gralloc.cros.so` and `gralloc.gbm.so` are
**prebuilts** in this tree (`device/redroid-prebuilts/Android.mk`, `LOCAL_SRC_FILES := prebuilts/
$(TARGET_ARCH)/lib/...`) — not built by the normal `m` product build at all. `gralloc.gbm.so`
links Mesa's own `libgbm.so.1` (confirmed earlier); `gralloc.cros.so`, by contrast, has
`backend_amdgpu`/`backend_i915`/`backend_virtgpu`/etc statically linked in — genuinely minigbm's
`drv.c` dispatch, matching this exact `external/minigbm` checkout. Two different gralloc
implementations exist side by side in the vendor partition; redroid's `gpu_config.sh` just always
picks `gbm` (Mesa) in host mode, never `cros` (minigbm). This means the real target for a new
backend is minigbm's `cros` build, reachable via `ro.hardware.gralloc=cros` — not Mesa's `gbm` at
all, and not something `gpu_config.sh` currently ever selects.

**Wrote a new minigbm backend**, registered for DRM driver name `nvidia-drm` (the string
`drmGetVersion()` actually reports for this hardware — confirmed back in Tier 0). Used
`dumb_driver.c`'s existing `INIT_DUMB_DRIVER_WITH_NAME` macro as the starting shape (the simplest
real backend in the tree, already registers `backend_nouveau` the same way) — this first pass
uses minigbm's generic `DRM_IOCTL_MODE_CREATE_DUMB` path (every real KMS driver supports it,
including `nvidia-drm.ko` with `modeset=1`), so it's CPU-mappable linear buffers only, no GPU
acceleration yet — a canary to validate the whole build→deploy→test loop cheaply before writing
the much larger vtest/Venus-based real backend. Registered in `drv.c`'s `drv_backend_list[]`
alongside the existing entries.

**Built successfully with Soong**, end to end, inside `redroid-builder`: `lunch
redroid_x86_64-ap3a-userdebug && m gralloc.minigbm` — first build (Soong analysis phase from a
`out/` that had never actually been built), ~16 minutes, exit 0. Confirmed the new backend's
string (`nvidia-drm`) is genuinely compiled into `libminigbm_gralloc.so` — the real backend
library that the thin `gralloc.minigbm.so` HAL shim dynamically depends on (a two-file split this
Soong config produces, different from the prebuilt's single statically-linked file, discovered by
comparing `NEEDED` entries).

**Deployed for a real test**: copied both new `.so` files (64 and 32-bit) into a fresh container
as `gralloc.cros.so` / `libminigbm_gralloc.so`, plus a patched `gpu_config.sh` that sets
`ro.hardware.gralloc=cros` specifically when the detected driver is `nvidia-drm` (leaving every
other vendor's existing `gbm` selection untouched — a real, driver-conditional change, not a
blanket override). **New, different, more specific failure** — real progress, not a repeat:

```
vndksupport: Could not load /vendor/lib64/hw/gralloc.cros.so from sphal namespace:
  dlopen failed: library "libdmabufheap.so" not found: needed by ... in namespace sphal.
```

`libdmabufheap.so` genuinely exists on the system partition (confirmed present) — this is Android's
linker **namespace** isolation refusing to let a vendor HAL loaded through the `sphal` namespace
see it, not a missing file. This build's Soong defaults for `libminigbm_gralloc` pull in
`libdmabufheap`/`libgralloctypes`/`libnativewindow` as direct dependencies — a newer, Gralloc4-
style dependency set this specific redroid image's actual deployed linker/VINTF configuration
apparently doesn't expose to vendor HALs (the original prebuilt `gralloc.cros.so` never needed any
of these — confirmed via its own, much smaller `NEEDED` list read back in Tier 1). Two real paths
forward, neither chased tonight: (1) find and flip whatever Soong config selects the older,
simpler allocation path (avoiding the new dependency entirely) that the original prebuilt was
clearly built with, or (2) fix the actual VINTF/linkerconfig namespace rule to expose
`libdmabufheap.so` to `sphal`. Real, understood, scoped blocker — not a mystery — paused here for
the day.

## 2026-09-23 — Tier 4: gralloc actually loads and allocates — the wall moved to EGL, exactly where expected

Picked back up on `jgustavo48` (Wake-on-LAN from `jgustavo-server01`, same LAN — reloaded
`loop`/`ext4`/`binder` and re-chmod'd the binder nodes, none of which survive a reboot).

**Path 1 from yesterday (find a Soong flag to skip `libdmabufheap`) turned out to be a dead end,
checked rather than assumed**: removed it from `minigbm_cros_gralloc_defaults`'s `shared_libs` and
rebuilt — real compile failure, `cros_gralloc_driver.h` genuinely `#include`s
`BufferAllocator/BufferAllocator.h` from that library. Yesterday's "nothing in the source
references it" check was wrong because it grepped for the string `dmabufheap`, and the actual
usage is via that header path and a `BufferAllocator` class name — neither contains that
substring. Reverted the removal immediately; lesson logged so the same mistake doesn't repeat.

**Path 2 (fix the namespace) also didn't pan out as expected, but a simpler fix did**: added
`libdmabufheap.so` to `/system/etc/llndk.libraries.txt` and rebooted a fresh container — the
generated `/linkerconfig/ld.config.txt` **did not change at all**; grepping it for `dmabufheap`
came back completely empty, meaning `linkerconfig` isn't sourcing the `sphal` namespace's allowed
list from that plain text file the way its LLNDK role would suggest (or something else about this
image's boot path bypasses that step) — not fully understood, and not worth the time to fully
understand tonight. Skipped straight to the pragmatic fix instead: **same-partition libraries never
cross the namespace boundary at all**, already proven by `libgralloctypes.so` (present on both
`/system` and `/vendor`, never triggered the error). Copied `libdmabufheap.so` (both ABIs) from
`/system/lib*/` straight into `/vendor/lib*/` before boot. That one error disappeared completely.

**A second, different missing library then surfaced** (progress, not a regression):
`gralloc.cros.so` also wants plain `libdrm.so`, while this image only ships the versioned
`libdrm.so.2` (the original prebuilt was built against that older SONAME convention; this Soong
config's default emits the newer unversioned one). Same fix shape, cheaper this time — no need to
copy a whole library, just `ln -s libdrm.so.2 libdrm.so` in both vendor lib dirs; same real ABI,
different name. Created live on an already-crash-looping container and picked up on the very next
restart attempt, no reboot needed.

**With both fixed, `gralloc.cros.so` genuinely loads and works**:

```
MESA: Using gralloc0 CrOS API
```

No more `vndksupport`/`dlopen failed` anywhere in the log — a real, clean load, confirmed on
several consecutive service restarts. `vendor.gralloc-2-0` no longer exits with status 1 at all.
**This is the actual fix for the bug chased since Tier 0** — the buffer-allocation half of the
NVIDIA wall is closed: minigbm's real backend dispatch (`drv_get_backend()` matching `nvidia-drm`)
now succeeds where Mesa's own GBM never could.

**`surfaceflinger` still crashes — but at a different, later, and expected point**:

```
Abort message: 'no suitable EGLConfig found, giving up (... vendor: Android ... Client API: OpenGL_ES)'
```

This is architecturally correct and not a setback: gralloc (buffer *allocation*) and EGL
(GPU *rendering*) are separate components. Fixing gralloc only closes half the wall from Tier 0 —
`ro.hardware.egl` is still `mesa` (`gpu_setup_host()` never touches it), so SurfaceFlinger's
RenderEngine is still asking Mesa's own EGL/GBM implementation to create a context, which still
has no NVIDIA backend, for the exact reason established back in Tier 0. The dumb-buffer canary
backend was never meant to produce real 3D acceleration by itself — it was built specifically to
validate the write→build→deploy loop cheaply before writing the real vtest/Venus backend. It did
exactly that, and did it while also landing a genuine, permanent fix (buffer allocation actually
works now).

**Where this leaves it, precisely scoped**: the remaining wall is EGL/rendering only, and the fix
shape is already known and already proven standalone in Tier 3 — `ro.hardware.egl=angle` (already
shipped in this image) on top of Venus (`ro.hardware.vulkan=virtio`, `mesa.vn.debug=vtest`,
`mesa.vtest.socket.name=...`, matching `waydroid-nvidia`'s own architecture doc) talking to the
same `virgl_test_server`/`virgl_render_server` host process already confirmed working against the
real RTX 4060. Next concrete step: extend the dumb-buffer backend into the real vtest-based GPU
allocator (the actual point of Tier 4), and set the EGL/Vulkan props alongside it, so gralloc and
EGL are both talking to the same host renderer instead of gralloc alone.

## 2026-09-23 (same day) — Tier 4: real Venus-backed gralloc, and RenderEngine genuinely stands up Vulkan on the RTX 4060

Rewrote the backend for real. Yesterday's dumb-buffer canary proved the write→build→deploy loop;
today it got replaced with an actual minigbm `struct backend` (`nvidia_venus.c`, registered as
`backend_nvidia_venus` for driver name `nvidia-drm`, replacing the dumb-buffer registration
entirely so there's no name collision) that allocates over the same vtest wire protocol confirmed
standalone in Tier 3 (`VCMD_CREATE_RENDERER` handshake, `VCMD_RESOURCE_ALLOC_GPU`, fd received via
`SCM_RIGHTS`) — this time implemented against minigbm's real `bo_create`/`bo_import`/`bo_destroy`/
`bo_map`/`bo_unmap`/`resource_info` callbacks instead of a bespoke vtable, so it's a genuine
backend the rest of minigbm's dispatch and the cros_gralloc mapper layer treat like any other.
Read `<sys/system_properties.h>` value `mesa.vtest.socket.name` directly, matching the same
property name Venus's Vulkan side already uses — one socket, one property, both sides agree on it
without any new plumbing.

Also patched `gpu_config.sh` further: `setup_vulkan()` gained an `nvidia-drm` case setting
`ro.hardware.vulkan=virtio` + `mesa.vn.debug=vtest` + `mesa.vtest.socket.name=/dev/venus/venus.sock`
(mirroring `waydroid-nvidia`'s own architecture doc), and `gpu_setup_host()` now sets
`ro.hardware.egl=angle` specifically for `nvidia-drm` instead of `mesa` — ANGLE-on-Venus is the
real GPU path, matching Tier 2's research.

Built clean with Soong (confirmed the actual exit code explicitly this time, not just the tail of
a piped command — yesterday's silent failure taught that lesson). Deployed against a real
`virgl_test_server` this time run with `--multi-clients` (a real boot makes several sequential
connections, unlike the one-shot Tier 3 probe), with its socket bind-mounted from the host into
the container at `/dev/venus/venus.sock` via a shared directory — no code needed to know
Docker exists, just a systeem property naming a path both sides serve.

Also carried forward the two library fixes from earlier today (`libdmabufheap.so` copied into
`/vendor/lib*`, `libdrm.so` symlinked to `libdrm.so.2`) — a fresh container needs both every time,
they're not something the build itself produces.

**Result — real, measured progress on the actual goal, not a canary this time**:

```
ANGLE: Renderer (Vulkan 1.1.274 (NVIDIA Virtio-GPU Venus (NVIDIA GeForce RTX 4060) (0x00002808)))
RenderEngine: renderer: ANGLE (NVIDIA, Vulkan 1.1.274 (... RTX 4060 ...), NVIDIA-24.0.0.8)
```

The "no suitable EGLConfig found" abort from every earlier attempt is **completely gone**.
SurfaceFlinger's RenderEngine now genuinely creates a Vulkan device against the real GPU through
Venus and reports its real name — the EGL/rendering half of the wall chased since Tier 0 is now
also cracked, not just the gralloc/allocation half from earlier today. The host renderer log
confirms it isn't a fluke: `vtest_gpu_alloc: allocating on "NVIDIA GeForce RTX 4060"` — a real
allocation request reached the host and was serviced.

**Still crashes, but three steps deeper and in a precisely diagnosable place**:

```
Abort message: 'output buffer not gpu writeable'
  SkiaRenderEngine::drawLayersInternal -> drawHolePunchLayer -> Cache::primeShaderCache
```

This is Android's shader-cache-priming step doing a real test draw into a gralloc-allocated
buffer, and Skia's own Vulkan import path asserting the buffer isn't usable as a render target.
Scoped to this backend's own `bo_create()`: the `VCMD_ALLOC_GPU_FLAG_MAPPABLE` flag translation
logic (or the `format_modifier` reported back through `resource_info()`) isn't correctly
distinguishing "real GPU-renderable buffer" from "CPU-mappable buffer" for whatever `use_flags`
combination this specific priming call passes — not a mystery, a debuggable flag-mapping bug in
code written today, not an architectural wall. Next step: trace exactly which `use_flags` this
call passes and fix the mapping so a genuinely renderable (non-mappable, real modifier) buffer
comes back for it.

## 2026-09-23 (same day) — Chasing "output buffer not gpu writeable" further: real per-call evidence, ruled out the easy explanations

Added logging (`__android_log_print`, tag `nvidia_venus`) directly to `bo_create()` to see the
real `use_flags` per call instead of guessing. Real data, not a repeat: the failing buffer is a
**720x1280 buffer, `use_flags=0x25`** (`BO_USE_SCANOUT | BO_USE_RENDERING | BO_USE_TEXTURE`) — a
genuine render request, correctly translated to `alloc_flags=0x2` (SCANOUT only, **not**
MAPPABLE) — which should route to the host's real GPU-renderable Vulkan-image path, not the
CPU/udmabuf one (confirmed by reading `virglrenderer-vtest/vtest_gpu_alloc.c`: the two paths are
entirely separate functions, `vtest_gpu_alloc_gpu()` vs `_cpu()`, selected purely by that flag).
The backend logs `bo_create 720x1280: OK fd=8 stride=2880` — success, from inside
`vendor.gralloc-2-0`'s own process — no error anywhere in the allocation path itself.

**Read the actual AOSP assertion source** (`RenderEngine::validateOutputBufferUsage`,
`frameworks/native/libs/renderengine/RenderEngine.cpp`): it checks
`buffer->getUsage() & GraphicBuffer::USAGE_HW_RENDER` on the `GraphicBuffer` C++ object itself —
and `GraphicBuffer::initWithSize()` only sets that field from the *original caller's requested*
usage, and only `if (err == NO_ERROR)` from the allocator. Traced the actual call site
(`Cache::primeShaderCache`, `frameworks/native/libs/renderengine/skia/Cache.cpp:704-708`): the
failing buffer (`dstBuffer`) is explicitly constructed with
`GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE` — the request itself is correct.

**Tried the obvious mitigation, found it doesn't work — and *why* it doesn't is itself real
information**: `SurfaceFlinger.cpp` exposes real, official properties
(`debug.sf.prime_shader_cache.hole_punch`, `.solid_layers`, etc.) gating each individual priming
draw. Set all of them to `false` for the `nvidia-drm` case. Result: the crash's backtrace offset
*shifted* (skipped past `drawHolePunchLayer`, as expected) but the **identical** abort still fires
from a *different* draw call moments later. Checked `Cache.cpp` exhaustively: `dstTexture` (the
same buffer) is used by well over a dozen draw calls, several unconditional
(`drawBlurLayers` gated only by `renderengine->supportsBackgroundBlur()`, not any settable prop)
or gated by properties that don't cover every path. **This rules out "some priming step doesn't
handle this buffer correctly"** — every single draw into this specific buffer fails identically,
which means the buffer itself never becomes genuinely GPU-writable in the first place, regardless
of which code tries to use it first. Disabling priming steps one at a time was never going to be
the real fix; reverted that framing.

**Where this leaves it, honestly**: allocation succeeds (my code, confirmed), the request is
correct (AOSP's code, confirmed), and yet the resulting buffer isn't usable as a render target by
the time ANGLE/Skia gets to it. The likely remaining suspects, not yet checked: (1) something in
`cros_gralloc_buffer.cc`'s own post-allocation handling that doesn't correctly carry render
capability into the `native_handle_t`/AHardwareBuffer descriptor for a *non-DRI* backend it wasn't
originally written against (every existing minigbm backend is a real local DRM driver;
`nvidia_venus` is the first one whose buffers come from a remote process entirely — some assumption
elsewhere in that gralloc layer may implicitly expect that), or (2) something ANGLE's own Vulkan
image-import path checks on the `AHardwareBuffer` (via
`vkGetAndroidHardwareBufferPropertiesANDROID`-style queries) that this backend's buffer doesn't
satisfy even though its Vulkan-side allocation on the host was created with the right
`VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT`. A real, narrower, well-evidenced next step — not a
rabbit hole — for the next session: instrument or trace the cros_gralloc→AHardwareBuffer→ANGLE
import path specifically, since both endpoints (host-side Vulkan image creation, backend's own
`bo_create`) are now independently confirmed correct.

## 2026-09-23 (same day) — Three real, root-cause bugs found and fixed; the wall moves to Vulkan/EGL native-buffer import

Picked the trail back up with the exact plan the last entry laid out: trace the import path
directly instead of guessing further. Found three separate, real, independently-confirmed bugs,
each one exposing the next once fixed — this is the real story of why "output buffer not gpu
writeable" resisted every earlier theory: it was never one bug, it was three stacked in a row,
and each earlier fix attempt was correct but insufficient because the *next* one masked the result.

**Bug 1 — `bo->handle` held a raw fd, not a GEM handle (this backend's own bug).** Reread
`drv_bo_get_plane_fd()` in `drv.c` line by line: it calls
`drmPrimeHandleToFD(bo->drv->fd, bo->handle.u32, ...)` — meaning every minigbm backend is expected
to store a real, *local* GEM handle in `bo->handle`, valid on this driver's own DRM fd, which
`drv_bo_get_plane_fd()` re-exports to a fresh fd on demand. This backend was instead storing the
raw fd received from the host directly as `bo->handle.s32` — a fd number, not a GEM handle,
meaningless to `drmPrimeHandleToFD()` on this device. That call failed with ENOENT every single
time, silently, and **this — not anything about the render request or Skia's shader-cache
priming — was the actual original cause of "output buffer not gpu writeable" all along**: the
allocation was failing deep inside `cros_gralloc_driver::allocate()` (`goto destroy_hnd`, no
distinctive log at that exact site), so `GraphicBuffer::initWithSize()`'s `usage = inUsage` line
never ran, `usage` stayed 0, and the later `validateOutputBufferUsage()` check failed for a buffer
that was never actually allocated — not because it wasn't "gpu writeable" in any deeper sense.
Fixed by importing the fd via `DRM_IOCTL_PRIME_FD_TO_HANDLE` into a real local GEM handle right
in `bo_create()`, and switching `bo_import`/`bo_destroy`/`bo_unmap` to minigbm's own existing
generic helpers (`drv_prime_bo_import`, `drv_gem_bo_destroy`, `drv_bo_munmap`) instead of
reimplementing the same ioctls — they already do exactly this, correctly, and reusing them is both
less code and a stronger correctness guarantee than a hand-rolled equivalent.

**Bug 2 — a real double-free in AOSP's own `cros_gralloc_driver::allocate()`, never triggered
before.** Fixing bug 1 let allocation proceed further than it ever had for this GPU — and hit a
Scudo "invalid chunk state" heap-corruption abort in `allocator@2.0-s`. Traced the crash to
`cros_gralloc_buffer::~cros_gralloc_buffer()` calling `drv_bo_destroy(bo_)` unconditionally
(confirmed by reading its destructor directly) — and `allocate()`'s own `destroy_hnd:` cleanup
label calls `drv_bo_destroy(bo)` *again*, explicitly, on the same `bo`, whenever a failure occurs
*after* the local `buffer` unique_ptr already owns it (e.g. the `initialize_metadata()` failure
path). Once `buffer` goes out of scope at the `return ret;` right after, its destructor destroys
`bo` a second time. This is real AOSP source, unrelated to this backend, and had plausibly never
fired before in this codebase because every previous NVIDIA attempt failed *earlier* (bug 1, before
`buffer` was ever constructed) — genuinely new territory, first execution of this exact code path
on this hardware. Fixed with a one-line guard: only call the explicit `drv_bo_destroy(bo)` at
`destroy_hnd:` `if (!buffer)` — otherwise let the unique_ptr's own destructor handle it exactly
once.

**Bug 3 — `initialize_metadata()` called unconditionally, also real AOSP source, also newly
exposed.** With the double-free fixed, allocation failed cleanly (no crash) with "Failed to
initialize metadata: failed to get metadata region" → "Buffer does not have reserved region." —
traced to `cros_gralloc_driver::allocate()` calling `buffer->initialize_metadata(descriptor)`
unconditionally, while the reserved region backing that metadata is only ever created a few lines
earlier `if (hnd->reserved_region_size > 0)`, itself gated on `descriptor->enable_metadata_fd`.
When the caller (redroid's `gralloc0_alloc()` shim) doesn't request `enable_metadata_fd`, no
region exists to initialize — and nothing ever checked that before trying. This is the clearest
sign yet that `gralloc.cros.so` (minigbm's real backend dispatch) has plausibly **never been
exercised through a real boot on this AOSP tree before** — every earlier redroid GPU
(AMD, Intel) used Mesa's separate `gralloc.gbm.so` in host mode, never `cros`. Fixed by gating the
`initialize_metadata()` call on the same `descriptor->enable_metadata_fd` condition already used
for creating the region in the first place.

**With all three fixed, buffer allocation is now genuinely, fully correct** — confirmed via the
`nvidia_venus` logging: every real format/size combination the caller needs succeeds cleanly, no
crashes, no silent failures; the only remaining "Failed to allocate" is a single harmless format
probe (`format 59`, not one this backend registers support for, exactly as it should behave for
an unsupported format on real hardware too).

**The wall moved one level deeper, to Vulkan/EGL's own native-buffer import — a real, different,
more specific problem than anything above**:

```
D skia: Could not create EGL image, err = (0x300c)   [EGL_BAD_PARAMETER]
```

traced through `GaneshBackendTexture` → `SkiaRenderEngine::mapExternalTextureBuffer` →
`GrAHardwareBufferUtils::MakeGLBackendTexture` (confirmed via the `RenderEngine: ... SkiaGL Backend
(Ganesh)` log line that GL, not Vulkan, is the active Ganesh backend here — meaning ANGLE's own
`eglCreateImageKHR(..., EGL_NATIVE_BUFFER_ANDROID, ...)` is what's failing, internally, presumably
by itself needing to import the AHardwareBuffer into *its own* Vulkan context via
`VK_ANDROID_external_memory_android_hardware_buffer` — a genuinely different Vulkan extension than
the `VK_EXT_external_memory_dma_buf` this whole pipeline (host allocation, Venus transport) is
built around. ANGLE and Skia are both prebuilt/vendored-as-source-but-effectively-fixed here, so
this isn't a bug in code written for this project — it's a real, substantive architecture question
about whether Venus's guest-side Vulkan driver even implements/needs to implement the ANDROID
hardware buffer external-memory extension for AHardwareBuffer reimport to work, distinct from the
dma-buf path already proven working for host-side allocation. Not chased further tonight — a real
frontier, not a rabbit hole, and a good place to pick back up.

## 2026-09-23 (same day) — Traced one level further into Mesa itself: `vn_android.c` exists, the exact rejection point doesn't (yet)

Checked the obvious first question before assuming Venus is missing the extension outright:
`external/mesa3d/src/virtio/vulkan/vn_android.c` is real, present, ~1080 lines, with genuine
`VK_ANDROID_external_memory_android_hardware_buffer` support code
(`vn_android_image_from_anb`, `vn_android_get_ahb_format_properties`, etc.) — so this isn't a
"Venus doesn't support AHardwareBuffer at all" situation. The extension exists; something more
specific about *this* buffer is what it rejects.

Traced the property-query chain it depends on: `vn_android_gralloc_get_buffer_properties()` calls
Mesa's own `u_gralloc` abstraction, which for CrOS gralloc (`u_gralloc_cros_api.c`) dispatches to
the legacy `gralloc_module_t::perform()` op `CROS_GRALLOC_DRM_GET_BUFFER_INFO` — implemented in
`gralloc0.cc`'s `gralloc0_perform()`, which calls this backend's own `resource_info()` directly
(confirmed by reading it) — a completely different, independent code path from the
reserved-region/metadata machinery bugs 2 and 3 fixed earlier today. `vn_android`'s own check —
`if (info.modifier == DRM_FORMAT_MOD_INVALID) { vn_log(...); return false; }` — would explain the
failure exactly if it fired, but on paper this backend's `resource_info()` returns a real,
non-invalid modifier value (whatever the host's `vkGetImageDrmFormatModifierPropertiesEXT`
reported), so nothing here should trip it.

**Checked empirically rather than trusting the paper trail**: redeployed (no rebuild needed —
`vulkan.virtio.so` is an unmodified prebuilt) and grepped fresh logs for `vn_log`'s own output
strings (`u_gralloc_get_buffer_basic_info failed`, `Unexpected DRM_FORMAT_MOD_INVALID`) around the
`Could not create EGL image, err = (0x300c)` failure. **Neither appeared** — meaning either this
exact branch isn't where the rejection happens, or Mesa's own logging at that call site is below
whatever verbosity level reaches logcat by default (not yet confirmed which). Genuinely
undetermined which, tonight — the honest state is "the failure is somewhere in or downstream of
`vn_android_image_from_anb`, not yet isolated to a specific line," not "confirmed to be X."

**Where this leaves it**: real progress in scope (ruled out the reserved-region/metadata bugs as
the cause of *this* specific failure, confirmed the AHardwareBuffer-support code genuinely exists
in this Mesa build), but the exact rejection point inside `vn_android.c`'s import chain is still
open. Next session: either force Mesa's debug logging verbose enough to surface `vn_log` output
reliably, or add a temporary printf directly in `vn_android_image_from_anb_internal` (this is
local, buildable Mesa source, same NDK/build path already used for `vulkan.virtio.so` in
`waydroid-nvidia`'s own `build/mesa/build.sh` recipe) to nail the exact call that returns
non-`VK_SUCCESS`.

## 2026-09-23 (same day) — Build environment for Mesa is ready; traced the real call chain across three codebases; the exact rejection point still isn't pinned down, and now we know why

Set up what's actually needed to build Mesa's guest Venus driver from source, matching
`waydroid-nvidia`'s own documented recipe: installed `meson`/`ninja` on the host, downloaded
Android NDK r27c to `/opt/android-ndk` (this AOSP checkout's own bundled
`prebuilts/ndk/current/` turned out to be headers/sources only, no actual toolchain binaries —
the real NDK needs to come from Google directly). Confirmed the local `external/mesa3d` checkout
here is a genuine, clean git tree (AOSP's own upstream mirror) that should work directly with
`waydroid-nvidia`'s meson cross-file recipe without needing a separate clone.

**Before spending a build cycle, traced the real call chain by hand across three separate
codebases (Mesa, AOSP frameworks, ANGLE) — this by itself corrected a wrong assumption from
earlier tonight**: the crash goes through `EGL_NATIVE_BUFFER_ANDROID`, which in ANGLE's Vulkan
backend (`DisplayVkAndroid::createExternalImageSibling`,
`external/angle/src/libANGLE/renderer/vulkan/android/`) is handled by
`HardwareBufferImageSiblingVkAndroid` — genuinely the **AHardwareBuffer** import path
(`vkGetAndroidHardwareBufferPropertiesANDROID`), not the ANB/WSI-swapchain path
(`vn_android_image_from_anb`) chased earlier — those are two different Vulkan mechanisms that
happen to share the word "native buffer." `ValidateHardwareBuffer`/`initImpl` in
`HardwareBufferImageSiblingVkAndroid.cpp` confirmed `EGL_BAD_PARAMETER` (`0x300c`) is ANGLE's
*generic* fallback for any non-`VK_SUCCESS` result here — it doesn't distinguish which underlying
Vulkan call actually failed, which is exactly why the EGL-level error alone couldn't localize
anything further.

**Checked the two real prerequisites for Venus even offering the AHardwareBuffer extension**,
since `vn_physical_device_get_native_extensions()` only sets
`ANDROID_external_memory_android_hardware_buffer = true` when the *renderer* (the host side)
exposes both `EXT_image_drm_format_modifier` and `EXT_queue_family_foreign`. Wrote a tiny
standalone probe (`list_exts.c`, ~30 lines) reusing the exact same Tier 3 vtest connection method
— no Android boot needed — enumerating `vkEnumerateDeviceExtensionProperties` over the real
Venus/vtest connection. **Both prerequisites are confirmed present**, ruling that out cleanly.
(The probe's own report that `ANDROID_external_memory_android_hardware_buffer` itself is absent is
expected and uninformative — it's gated behind `#if DETECT_OS_ANDROID`, true only in the real NDK
Android build, not in this Debian-packaged `libvulkan_virtio.so` used for the standalone test.)

**Found why every earlier `vn_log` grep came up empty, and it isn't the log content**:
`vn_log()` (`vn_common.c`) tags every message `"MESA-VIRTIO"` at `MESA_LOG_DEBUG` priority — not
`"anb"`, `"vn_android"`, or `"u_gralloc"` as I'd been grepping for (those are inside the *message
text*, which should still substring-match — but a completely clean, tag-scoped, verbose-priority
query (`logcat -d -s MESA-VIRTIO:V`) *also* came up completely empty). Since `MESA-VIRTIO`-tagged
lines are real and appear elsewhere in this same project's history (Tier 0's original crash logs
show them), their total absence here is itself the real finding: **none of `vn_android.c`'s
existing `vn_log()` call sites are being reached at all** for this failure — not the ones in
`vn_android_gralloc_get_buffer_properties`, not the ones in `vn_android_get_ahb_format_properties`,
not the ones in `vn_android_image_from_anb_internal`. The rejection happens *before* any of them —
either inside ANGLE's own code prior to calling into Mesa, or in a Mesa function this session
hasn't read yet.

**Where this leaves it**: three codebases now understood in real detail (ANGLE's dispatch,
Venus's extension negotiation confirmed correctly wired end to end, AOSP's u_gralloc→gralloc0
bridge), the actual failure point still not pinned to a line — but for a well-understood, now much
narrower reason (wrong log tag assumption, corrected) rather than an open mystery. The build
environment is ready and waiting; next session's most direct move is a real Mesa build with a
`fprintf(stderr, ...)` dropped at the very top of `HardwareBufferImageSiblingVkAndroid::initImpl`
peers on the ANGLE side (also locally buildable, `external/angle`) and/or at the top of
`vn_GetAndroidHardwareBufferPropertiesANDROID`'s actual entry point on the Mesa side (not yet
located — `vn_android_get_ahb_format_properties` is the *helper*, not the public Vulkan entry
point that calls it) — bypassing Android's log-level system entirely, since it's demonstrably not
reliable for this specific investigation.

## 2026-09-23 (same day) — Built the instrumented Mesa driver; found the crash isn't in Mesa at all — it's two infra bugs and one real, deeper Venus gap upstream of it

Set up the standalone Mesa cross-build for real this time. `waydroid-nvidia`'s recipe needed two
small fixes not mentioned anywhere: meson 1.3.2 calls the fallback option `force_fallback_for`,
not `allow-fallback-for` (renamed at some point upstream), and this AOSP `external/mesa3d`
checkout needs `python3-mako` (`meson.build` checks for it explicitly, otherwise fails before
configuring anything) and a `libdrm` wrap (`meson wrap install libdrm` — not vendored in this
tree's `subprojects/`). With both fixed, `ninja -C ... src/virtio/vulkan/libvulkan_virtio.so`
built clean on the first real attempt: 244 targets, no errors. Instrumented
`vn_android.c`'s `vn_GetAndroidHardwareBufferPropertiesANDROID` (the real entry point ANGLE calls)
and its helpers with unconditional prints — switched from `fprintf(stderr, ...)` to
`__android_log_print(ANDROID_LOG_ERROR, "REDROID-DIAG", ...)` partway through, since stderr from a
library loaded into `surfaceflinger` has no guaranteed path to `logcat`, while `__android_log_print`
does (confirmed via `strings`/`readelf --dyn-syms`: the tag is embedded, `liblog.so` was already a
linked dependency, `__android_log_print` resolves `UND` against it).

**Never got to test a single one of those prints — the deploy itself would not boot stably long
enough to matter**, and chasing that turned into the real work of this session. Three real,
independently-confirmed problems, in the order they were found:

**1. Two freshly-created test containers sharing one GPU/vtest server crash-loop the whole
Android container, not just Vulkan.** Created a second test container (`jg-t4-diag2`) alongside
the already-running, stable `jg-t4-dbg3` to test the new library — it died with exit code 129
(SIGHUP) every ~15-20s, no matter what was deployed into it, even a byte-for-byte copy of dbg3's
own known-good, untouched `vulkan.virtio.so`. Root-caused by elimination: stopped `dbg3` and the
*exact same* fresh-container recipe survived past 5 minutes immediately. Never fully diagnosed
*why* two guests fighting over one `virgl_test_server --multi-clients` instance destabilizes the
whole container rather than just failing GPU calls cleanly — logged as a real constraint for all
future testing on this box (single GPU, single test container at a time) rather than chased
further, since the instructions returned by the answer already point at the practical fix.

**2. `docker cp`-deployed test containers need `libdrm.so -> libdrm.so.2` reapplied on *every*
fresh container, and skipping it fails silently at the worst possible layer.** Established
routine for `gralloc.cros.so` from Tier 4's own earlier sessions; forgot to reapply it to a new
container built from scratch today. Consequence was not a gralloc error — it was
`vkEnumerateInstanceVersion` itself, the very first Vulkan loader call, returning
`VK_ERROR_OUT_OF_HOST_MEMORY` with **zero indication it was a missing-library problem**, because
`vulkan.virtio.so` (which needs `libdrm.so`, confirmed via `readelf -d`) never got far enough
through `dlopen()` to report why. Re-running the symlink fix without touching anything else made
this specific failure disappear completely on the next `surfaceflinger` restart.

**3. Once both of the above were out of the way, `SurfaceFlinger` chose the GL Skia backend
instead of Vulkan by default — an aconfig flag (`vulkan_renderengine`) gates it, and it isn't
flipped in this build — which is a separate, older bug from the two above (this is why the
original `EGL_BAD_PARAMETER` investigation's own "RenderEngine confirmed creating a real Vulkan
device" note from earlier sessions was evidently against a differently-configured test run, not
this checked-in `gpu_config.sh`).** Forced it directly with
`setprop debug.renderengine.backend skiavkthreaded` (checked first, ahead of the aconfig flag, in
`SurfaceFlinger.cpp`'s `chooseRenderEngineType()`) — added permanently to `gpu_config.sh`'s
`nvidia-drm` branch. This got `SkiaVK Backend (Ganesh)` selected and logged for the first time all
session.

**With all three fixed, hit a new, real, precisely-diagnosed wall — not Mesa, not our own code:**
`RenderEngine`'s `VulkanInterface::init()` now aborts with *"Vulkan device does not support
sufficient external semaphore sync fd features. exportFromImportedHandleTypes 0x0 (needed 0x10)
compatibleHandleTypes 0x0 (needed 0x10) externalSemaphoreFeatures 0x0 (needed 0x3)"* — i.e.
`VK_KHR_external_semaphore_fd`'s `SYNC_FD` handle type, which `SurfaceFlinger` requires
unconditionally for its Android-native-fence compositor pipeline. Confirmed by direct comparison,
same running `virgl_test_server`, same GPU: the **host** NVIDIA driver fully supports it
(`vulkaninfo` on jgustavo48 lists `VK_KHR_external_semaphore_fd` as a real device extension) but
it **never reaches the guest** — `list_exts` against the live vtest connection shows
`VK_KHR_external_semaphore` (the base extension) present but `_fd` absent from the 108 extensions
actually forwarded. Checked this AOSP tree's own `external/virglrenderer/src/venus/vkr_common.c`
(line 97): its static allowlist already has `.KHR_external_semaphore_fd = true` — so either this
generic AOSP mirror isn't the same source `waydroid-nvidia`'s prebuilt v0.1.2 binaries were built
from, or there's a runtime capability check beyond the allowlist that's failing specifically here.
Not yet resolved — this is the real next-session target, and it sits **upstream of** the whole
`EGL_BAD_PARAMETER`/AHardwareBuffer chase from earlier tonight (RenderEngine has to finish
initializing as Vulkan at all before any AHardwareBuffer import would even be attempted), which
means today's fixes make the AHB investigation *reachable* through a real boot for the first time,
rather than resolving it directly.

## 2026-09-24 (same night) — Found waydroid-nvidia's own fix for exactly this, hand-ported it, and it worked: real NVIDIA GPU acceleration booted redroid to the home screen for the first time

Went looking for how `waydroid-nvidia` itself solves the `VK_KHR_external_semaphore_fd` gap,
since it's the same fundamental problem (Venus over vtest, NVIDIA host, container/LXC use case)
this project has been chasing independently. Found the real repo
([`Shiro836/waydroid-nvidia`](https://github.com/Shiro836/waydroid-nvidia) — not the empty
`waydroid-nvidia/waydroid-nvidia` name) and its `patches/` directory, which turned out to contain
exactly this project's next two walls, already solved, as real patches against upstream
virglrenderer and Mesa:

- **`patches/virglrenderer/0001-...sync_file...patch`**: adds a whole new vtest wire command,
  `VCMD_SYNC_EXPORT_SYNC_FILE`, letting the host resolve a pending venus timeline sync point to
  its `VkFence` and export a real kernel `sync_file` fd from it (`vkGetFenceFdKHR` +
  `VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT`) — legal specifically because vtest's target use
  case is client and server sharing the same kernel (LXC/container), unlike a real VM. **Already
  present in the host binaries this project has been using** (`waydroid-nvidia`'s `v0.1.2`
  release, published *after* this patch's date) — confirmed via `strings` on
  `virgl_test_server`/`libvirglrenderer.so.1` showing `vtest_sync_export_sync_file`/
  `vkr_queue_export_fence_sync_file`. No host rebuild needed.
- **`patches/mesa/0001-...sync_fd...patch`**: the matching *guest* side in
  `vn_renderer_vtest.c` — queries `VCMD_PARAM_HAS_VENUS_SYNC_FD`, and when present, sets
  `info->has_external_sync = true` and wires up `sync_ops.export_syncobj`. **This is the piece
  actually missing** — this project's own Mesa build (vanilla AOSP mirror) hardcodes
  `has_external_sync = false` unconditionally, matching yesterday's finding exactly.
- **`patches/mesa/0002-...dma_buf...patch`**: a second, related gap — `bo_ops.create_from_dma_buf`
  is also hardcoded `NULL` in vanilla Mesa's vtest transport. Realized this would matter *before*
  even applying it, since `vn_get_memory_dma_buf_properties()` — deep in the AHardwareBuffer
  import path this project has been chasing since Tier 4 began — calls
  `vn_renderer_bo_create_from_dma_buf()` unconditionally.

Both Mesa patches target upstream desktop Mesa (`gitlab.freedesktop.org/mesa/mesa`, base
`a8ce4d8f`), not this project's AOSP mirror — the two trees have drifted enough (missing
intermediate `VCMD_*` command IDs entirely) that `git apply`/`git am` failed outright. Hand-ported
both onto this tree instead, keeping upstream's exact wire-protocol numbers
(`VCMD_SYNC_EXPORT_SYNC_FILE = 39`, `VCMD_PARAM_HAS_VENUS_SYNC_FD = 3`,
`VCMD_RESOURCE_IMPORT_BLOB = 40`) so it stays wire-compatible with the already-deployed,
already-patched host binaries without needing to touch them. See
[`patches/mesa/`](patches/mesa/) for the ported patch, the two originals kept for reference, and
the full explanation.

**Rebuilt, redeployed, and immediately hit a *third* bug** — this time genuinely new, not
described in either waydroid-nvidia patch: `vn_android_gralloc_shared_present_usage_init_once()`
aborts with `assertion "_vn_android_gralloc.front_rendering_usage" failed`. Traced it to a
32-bit-truncation bug in minigbm's own `gralloc0.cc`: `GRALLOC_DRM_GET_USAGE`'s
`BUFFER_USAGE_FRONT_RENDERING` is `(1ULL << 32)`, but both minigbm's local variable and Mesa's own
call-site output parameter are plain `uint32_t` — the bit is silently lost on **both** ends of
this legacy ABI, so the query always "succeeds" while reporting exactly 0. Rather than widen a
legacy ABI on both sides for a flag this project doesn't need, softened Mesa's assert to accept
that as a legitimate outcome.

**With all three fixes in place, RenderEngine initialized as real Vulkan for the first time ever
in this project**: `Vulkan device supports sufficient external semaphore sync fd features` →
`Success init Vulkan interface in 228.9 ms` → `GaneshVkRenderEngine::create: successfully
initialized GaneshVkRenderEngine`. Immediately after, the exact `REDROID-DIAG` instrumentation
dropped into `vn_android.c` the night before — sitting untested since a build-environment
detour — started firing for real, repeatedly, against real gralloc buffers:
`vn_GetAndroidHardwareBufferPropertiesANDROID called` → `vn_android_get_ahb_format_properties ->
0` → `dma_buf_fd = 25` → `vn_get_memory_dma_buf_properties -> 0 (alloc_size=65536
mem_type_bits=0x3)` → `vn_GetAndroidHardwareBufferPropertiesANDROID SUCCESS`, over and over, for
different buffer sizes/formats. **This is the exact `EGL_BAD_PARAMETER` wall from Tier 4's
opening — resolved, not worked around**: it was never a bug in the AHardwareBuffer import
logic itself, which was correct the whole time; it was blocked from ever running by the two
missing vtest transport capabilities above.

`sys.boot_completed` reached `1`. `surfaceflinger` stayed up, same PID, no restart loop.
`screencap` from inside the guest shows Android's real setup wizard ("Hi there" / language
picker / START button) — composited through real Vulkan RenderEngine, through Venus, through a
real NVIDIA RTX 4060, for the first time in this project. Saved as
[`docs/tier4-first-boot-nvidia-venus.png`](docs/tier4-first-boot-nvidia-venus.png). The image has
a visible horizontal line-tearing/corruption artifact — legible, not a crash, but a real
correctness bug still open, almost certainly a stride/row-pitch mismatch somewhere in this
project's own buffer plumbing (`nvidia_venus.c`'s minigbm backend is the prime suspect, being the
one piece of this whole chain written from scratch rather than ported from a working reference).
**This is Tier 5's actual next target**: find and fix that stride bug, then confirm a real,
undistorted rendered frame.

## 2026-09-24 (later the same night) — Tier 5: ruled out the stride theory, ruled out cache coherency, found a much better-supported lead in the brand-new sync_fd path itself

Started from the leading theory above (a stride/row-pitch bug in `nvidia_venus.c`). Killed it
with one cheap test: took three more `screencap`s back to back of the exact same static "Hi
there" screen. **All three came back with different corruption patterns and different
checksums.** A real stride bug is deterministic — same static frame, same wrong math, same wrong
picture every time. Non-deterministic corruption on unchanging content means a race, not a
logic bug. Checked the actual stride math anyway while there (`vtest_gpu_alloc_cpu`'s host-side
`ALIGN(width * bpp, 256)`, confirmed against real logged values like `720x1280 -> stride=3072`)
— it's correct.

**Next theory: missing CPU/GPU cache coherency on the shared-kernel udmabuf memory.** This
backend had no `bo_invalidate`/`bo_flush` at all (every other minigbm backend has them). Added
them using the generic `DMA_BUF_IOCTL_SYNC` ioctl (the right primitive here specifically because
vtest's target deployment is client and server sharing one kernel — a real dma-buf, not a
virtualized transport, so the standard Linux mechanism applies directly; a device-specific ioctl
like i915's `SET_DOMAIN` wouldn't). Rebuilt (`m gralloc.minigbm` in the `redroid-build-t4`
Soong container — hit one fresh snag: the `libdrm-2.4.134` meson subproject fetched for last
night's standalone Mesa build had its own `Android.bp`, colliding with `external/libdrm`'s;
deleted the extracted subproject directory, since Soong only needs `external/libdrm` and the
meson wrap was purely for the unrelated standalone build), redeployed, retested. **No change** —
same kind of non-deterministic corruption. Added temporary always-on logging to confirm the
hooks even fire: **zero calls, ever**, across a full boot and multiple screenshots. Traced why:
the active lock path on this Android 15 build is the newer AIDL `IMapper` v5
(`cros_gralloc/mapper_stablec/Mapper.cpp`), whose `lock()`/`unlock()` call
`cros_gralloc_driver::lock()`/`unlock()` directly — `drv_bo_invalidate`/`drv_bo_flush` are only
reachable through the *separate*, caller-optional `flushLockedBuffer()`/`rereadLockedBuffer()`
AIDL methods, which nothing in this path calls. Kept the fix anyway (correct regardless, harmless,
useful for any caller that does use those methods) but it's confirmed not the active mechanism —
see `patches/minigbm/README.md`'s update.

**Traced what `cros_gralloc_driver::lock()` actually does instead**: `cros_gralloc_sync_wait(acquire_fence,
...)` — it does wait on a real Android sync fence before returning a CPU pointer, which is the
*correct* design (the producer's fence, not a manual cache flush, is what's supposed to gate a
safe CPU read). Read `cros_gralloc_sync_wait()` itself
(`cros_gralloc_helpers.cc`): `if (fence < 0) return 0;` — a negative fence value is treated as
"already signaled, nothing to wait for." **This is exactly the convention today's own
`vn_queue.c` patch introduced**: `vn_create_sync_file()` now accepts `*out_fd >= -1` as success,
where `-1` means "the host says this already retired." If the *acquire fence* SurfaceFlinger
attaches to the buffer it hands off to the next consumer (`screencap`, in this case) comes from
this exact code path and is `-1` when the GPU work is *not actually done yet*, `screencap` would
read the buffer with zero wait — a textbook non-deterministic torn-read, matching every symptom
observed (mostly-correct content, since the GPU is usually fast enough anyway; occasional
partial corruption, since "usually" isn't "always"; different every capture, since it's a race).

Traced *why* a premature `-1` is plausible without finding a definitive smoking gun yet: guest's
`vn_create_sync_file()` submits the GPU work and, on the very next line, asks to export a
sync_file for that same work — two separate messages over the *same synchronous vtest socket*,
each under its own short-lived `sock_mutex` lock (not one held across both). The host's
`vtest_sync_export_sync_file()` (virglrenderer, `vtest_renderer.c`) answers by scanning
`ctx->timelines[ring].submits` for a still-pending entry that references this exact sync object
at the requested value — if it isn't there, the response is unconditionally "already signaled,"
with no distinction between "genuinely retired already" and "hasn't been registered as pending
yet." Whether the host's own submit-handling registers that bookkeeping entry synchronously
within the same command dispatch (safe) or only later via an async callback (racy) is *not yet
confirmed* — that's the concrete next step, and it requires reading/instrumenting
`virglrenderer`'s own `vkr_queue_sync_submit`/`vtest_submit_cmd2` server-side handling, which
means setting up a build environment for `virglrenderer` itself (this project only has Mesa's
build environment so far; the host binaries in use are `waydroid-nvidia`'s own prebuilt release,
already includes the untouched, still-marked-WIP-by-upstream sync_fd code as-is).

**Tested the cheap version of that theory directly**: added a diagnostic-only 2ms `usleep()` in
guest-side `vtest_sync_export_syncobj()` before asking the host to export the fence, rebuilt,
redeployed, recaptured. **No change** — same kind of corruption, same non-determinism. 2ms is a
very long time for a simple UI composite on an RTX 4060; if the bug were "the host hasn't
finished its own internal bookkeeping list update yet," that gap should have papered over it
completely. Reverted the delay (kept out of the published patch) — this specific mechanism is
very likely not it, though not conclusively ruled out for other timing scales.

**Went straight to the actual bytes instead of theorizing further.** `screencap`'s raw dump mode
(`screencap /path` with no `-p` — a 16-byte header, width/height/format/dataspace, followed by
tightly-packed RGBA, no padding) gives a clean way to inspect real pixel values without a PNG
codec in the way. Confirmed the header: `720x1280`, `RGBA_8888` — matches the buffer already
being traced (`bo_create 720x1280 ... stride=3072`). Read actual pixel bytes along a fixed
column: real, correct Android/Material background blue — `(66, 133, 244, 255)` — alternating with
**exactly `(0, 0, 0, 0)`** — not noise, not stale data, literally untouched, zero-filled memory (a
fresh memfd page that was never written). Measured the run-lengths of zero vs. non-zero pixels
along a row: **every run is an exact multiple of 16 pixels (64 bytes)** — `96, 32, 48, 16, 64, 32,
16, 32, 96, 32, 16, 16, 48, 16, 32, 16, 96, 16, ...`. Across the whole 1280-row buffer: 41 rows
entirely zero, 1 row entirely written, 1238 rows a mix — always quantized to that same 16-pixel
grain.

**This rules out both prior theories outright and points somewhere much more specific.** Not a
stride/row-pitch bug (those corrupt whole-row alignment, not a sub-row 64-byte grid — and the
pattern would be identical every capture, not different each time). Not a simple
"cache/DMA-visibility hasn't happened yet" race either — that would leave *stale old content*
where a write hasn't landed, not a hard `(0,0,0,0)`, and it wouldn't naturally quantize to a fixed
64-byte grid. A 64-byte-aligned, exactly-quantized pattern of "written vs. genuinely never
written at all" is the signature of a **tiled/block-linear GPU memory layout being read back as
if it were plain row-major linear** — i.e. something in this chain (most likely wherever
`vtest_gpu_alloc_cpu`'s "always linear, CPU stride == GPU stride" assumption meets whatever
Vulkan operation actually populates this buffer — a `vkCmdCopyImage`-style blit from
`RenderEngine`'s real rendered frame into this CPU-visible destination) isn't actually writing
in the flat linear order everyone downstream assumes, leaving real content in some 64-byte-wide
columns/blocks and never touching the memory in between.

**Found the exact source within the hour.** `RenderEngine` uses Skia directly
(`GaneshVkRenderEngine`) for its own Vulkan backend — that's a completely separate codebase from
ANGLE (ANGLE is the GLES-over-Vulkan layer used by *apps*, not by `SurfaceFlinger` itself; a wrong
turn earlier in this same session). Skia's own AHardwareBuffer-to-VkImage import
(`external/skia/src/gpu/ganesh/vk/AHardwareBufferVk.cpp`,
`make_vk_backend_texture`) hardcodes `VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;`
unconditionally — with its own `TODO` comment already admitting it: *"Check the supported
tilings... to see if we have to use linear. Add better linear support throughout Ganesh."*
Confirmed empirically this is genuinely the active path (added logging at Mesa's
`vn_android_get_image_builder` — the *different*, ANB-specific helper that correctly builds a
real explicit-modifier chain — and confirmed it never fires for this 720x1280 buffer at all,
only for an unrelated 30x30 one with a real vendor-tiled modifier). This lines up exactly with
the byte-level evidence: `OPTIMAL` here means the real NVIDIA driver (via Venus) writes into this
buffer using its own proprietary block-linear addressing, while every CPU reader (`screencap`,
gralloc's `lock()`) interprets the exact same bytes as plain row-major linear — the 64-byte-grid
pattern is that addressing mismatch made visible.

**Patched it two ways and tested on real hardware — and the obvious fix made things
measurably worse.** Added a check in both Skia's `make_vk_backend_texture` and (for
completeness, since it shares the identical hardcoded pattern with an identical justifying
comment) ANGLE's `AhbDescUsageToVkImageTiling`: when the `AHardwareBuffer`'s usage indicates real
CPU access (`AHARDWAREBUFFER_USAGE_CPU_READ`/`WRITE_MASK` != `NEVER`), request
`VK_IMAGE_TILING_LINEAR` instead — reasoning that a buffer needing genuine CPU byte access simply
cannot be validly `OPTIMAL`, and this project's `vtest_gpu_alloc_cpu` memfd+udmabuf backing has no
real tiled allocation to begin with. Built a full standalone ANGLE (Soong module `libEGL_angle`/
`libGLESv2_angle`, `~15` minutes) to test the ANGLE side (confirmed unused by this path, no visible
effect either way), then rebuilt `surfaceflinger` itself (Skia is statically linked in via
`libskia`/`libskia_renderengine`, so the real fix needs a `surfaceflinger` rebuild, not just a
shared-lib swap) with the `LINEAR` change. **Result: substantially worse** — raw pixel dumps went
from "real content alternating with structured 64-byte zero blocks" to "almost entirely zero,
essentially no legible content at all." The real NVIDIA driver, via Venus, evidently does not
handle `LINEAR` tiling for AHB import correctly at all here — not merely "unsupported for
`INPUT_ATTACHMENT` usage" as Skia's/ANGLE's own comments specifically call out, but broken for
this basic case too. **Reverted both changes cleanly**, rebuilt, redeployed, and confirmed a
fresh boot reproduces exactly the original (better) partially-legible corruption — the revert is
clean and this is genuinely back to Tier 4's baseline, not a regression.

**Where this leaves Tier 5**: the system boots and runs end-to-end with real GPU acceleration —
this isn't a hard blocker, `sys.boot_completed=1` and `surfaceflinger` stays up, and the actual
UI is legible under the corruption. The root cause is now fully identified and located to a
specific line in a specific, buildable file — but the "obvious" fix is empirically proven wrong
on this real hardware/driver combination, which is itself valuable: it rules out a whole category
of solution. What's actually needed is not a tiling-mode switch but an **explicit untiling
readback step** — the real driver's own block-linear layout is opaque to any non-driver reader by
design, so something has to ask the driver itself to produce a genuinely linear copy before the
CPU (or `screencap`, or anything else) touches the memory. Concretely, that likely means: keep
`OPTIMAL` for the image the GPU actually renders into, and add a real `vkCmdCopyImage`-to-a-
separate-genuinely-linear-buffer step (or find and correctly use whatever untiling mechanism
Venus/the host driver already exposes for exactly this) before handing pixels back to a CPU
reader — a distinct, separate buffer/copy step, not a property of the single AHB-imported image.
That's a real, scoped, next-session architectural task now that the wrong path (tiling-mode
switch) is conclusively ruled out.
