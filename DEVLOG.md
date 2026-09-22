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
