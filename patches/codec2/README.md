# codec2 — the guest side: `c2.hardware.encoder.h264` for real

A real `C2Component` for `c2.hardware.encoder.h264`, wired to redroid-nvidia's own
`VCMD_ENCODE_RESOURCE` (host-side NVENC — see [`../virglrenderer/README.md`](../virglrenderer/README.md)
and the main `README.md`'s Tier 7 section, both confirmed end to end on real hardware before this
module was written).

## Where this lives

Built and confirmed compiling/linking inside `~/aosp-redroid-15` (this project's own AOSP checkout
on `server01`, the same tree Tier 4's minigbm/gralloc work used — see `device/redroid-prebuilts/`
for how `gpu_config.sh`/prebuilts are assembled), at `external/nvenc_codec2/`. These files here are
that module's source, vendored into this repo the same way `patches/minigbm/nvidia_venus.c` is —
a full copy of an authored file, not a diff against a third-party tree.

```
external/nvenc_codec2/
├── component/
│   ├── vtest_encode_client.{h,cpp}   — resolve a gralloc buffer's Venus resource id, then
│   │                                    issue VCMD_ENCODE_RESOURCE over a fresh vtest connection
│   ├── NvencEncComponent.{h,cpp}     — the actual C2Component (SimpleC2Component)
│   └── Android.bp                    — cc_library_static "libnvenc_codec2_component"
└── service/
    ├── service.cpp                   — registers IComponentStore/default, createComponent()
    │                                    returns a real NvencEncComponent
    ├── Android.bp                    — cc_binary "android.hardware.media.c2-nvenc-service"
    ├── android.hardware.media.c2-nvenc-service.rc
    ├── manifest_media_c2_nvenc.xml
    └── seccomp_policy/               — standard AOSP default policy set, unmodified so far
```

To reproduce: drop this directory as `external/nvenc_codec2` in the same AOSP checkout, then
(inside the `redroid-build-persist` build container, or equivalent — `lunch
redroid_x86_64-ap3a-userdebug`):

```sh
m libnvenc_codec2_component                    # component only
m android.hardware.media.c2-nvenc-service       # full service binary
```

Both **confirmed building clean** (64-bit and 32-bit, no warnings) against this exact tree.

## Structural reference: redroid-hwenc's `VaapiEncComponent`

This is a direct adaptation of redroid-hwenc's own `tier5-vaapi-daemon/codec2-component/
VaapiEncComponent.{h,cpp}` (also present in this same AOSP checkout, at `external/vaapi_codec2/` —
both hardware-encoder projects were developed against it). Same `SimpleC2Component` base, same
`C2GraphicBlock`/dma-buf extraction shape, same `service.cpp`/`Android.bp`/`.rc`/manifest
structure. The transport differs, and so does one specific finding:

- **No daemon, no dma-buf forwarding.** VA-API's daemon needed the Android side to hand it a raw
  dma-buf fd (`SCM_RIGHTS`) because the buffer lived only in the container's own memory. Here, the
  buffer already lives on the host (allocated via minigbm's `nvidia_venus.c` →
  `vtest_gpu_alloc_gpu()`) — the component only needs the buffer's *Venus resource id*, resolved
  via the guest kernel's own `DRM_IOCTL_VIRTGPU_RESOURCE_INFO` (a standard virtio-gpu ioctl, no
  Venus/minigbm changes needed), then asks the host to encode that resource directly.
- **No empirical handle reverse-engineering needed.** `VaapiEncComponent`'s own comments describe
  real detective work: a Surface-sourced frame arrived as some *other*, non-`cros_gralloc_handle`
  native_handle_t, with field offsets (width/height/stride) found by dumping raw ints and matching
  them against known buffer dimensions. This project's own gralloc HAL (minigbm's `cros_gralloc`,
  confirmed active back in Tier 4's own bringup) produces a real, well-defined
  `cros_gralloc_handle_t` (`external/minigbm/cros_gralloc/cros_gralloc_handle.h`) for every buffer
  — `NvencEncComponent::process()` reads `fds[0]`/`width`/`height`/`strides[0]`/`format`/
  `format_modifier` straight off it, with the same validation (`numFds`/`numInts` size check,
  `magic` check) `cros_gralloc_convert_handle()` itself does. Whether a *Surface*-sourced encoder
  input buffer (screen-record/cast, as opposed to a hand-built `C2Work`) arrives this same way on
  this project's stack specifically is the next thing to confirm on real hardware — flagged below,
  not assumed.
- **One real Soong gotcha, already known from the sibling project**: `shared_libs` declared on the
  `cc_library_static` component don't propagate to the final `cc_binary` service automatically —
  `libdrm` (needed for `drmIoctl()`) had to be listed again explicitly on the service's own
  `Android.bp`, exactly the same issue `VaapiEncComponent`'s own service Android.bp already
  documented for `libcodec2_soft_common`/`libstagefright_foundation`. Confirmed via a real
  `ld.lld: error: undefined symbol: drmIoctl` on the first build attempt, fixed on the second.

## A real, load-bearing confirmation along the way

`virgl_renderer_resource_export_blob()` (what `VCMD_ENCODE_RESOURCE`'s host-side handler calls)
resolves a resource id through `virgl_resource_lookup()` — a lookup with **no `ctx_id` parameter
at all**, confirmed by reading virglrenderer's own source
(`src/virglrenderer.c:virgl_renderer_resource_export_blob`). This means the resource table is
**global** across the whole vtest server process, not scoped per client connection the way
`vtest_resource_import_blob()`'s own per-context bookkeeping might suggest. That's what makes this
component's design valid at all: it opens its *own*, brand-new vtest connection (via
`vtest_encode_client.cpp`'s `vtest_connect()`) — completely separate from whatever connection
minigbm's `nvidia_venus.c` or the guest's real Venus/Mesa driver maintains — and can still reference
a resource id that connection never created or imported itself.

## What's not confirmed yet (the natural next checkpoint)

Everything above is confirmed at the *build* level (compiles, links, against the real project
tree) and via source-level reasoning (the resource-id global-scope finding, the real gralloc
handle struct). **Not yet tested**: actually deploying this service into a running redroid
container and confirming, on real hardware —

1. `dumpsys media.c2` / `IComponentStore::listComponents()` recognizes `c2.hardware.encoder.h264`
   (redroid-hwenc's own Tier 4 checkpoint bar) — needs the stock AOSP default Codec2 service
   disabled/replaced in this device's product packages first, exactly as redroid-hwenc's own
   deployment required (see this file's service.cpp comment on why only one `IComponentStore`
   named `"default"` can be registered at a time).
2. That a real `HW_VIDEO_ENCODER`-usage buffer's native_handle_t really is a `cros_gralloc_handle_t`
   in practice for a Surface-sourced frame specifically (screen-record/`scrcpy`, not just a
   hand-built `C2Work` test) — the one assumption in `NvencEncComponent::process()` not yet
   exercised against a real frame.
3. The actual encode round trip from a real app/`screenrecord` through `MediaCodec` →
   `Codec2Client` → this component → `VCMD_ENCODE_RESOURCE` → NVENC → real, correct video —
   redroid-hwenc's own Tier 5.6-5.8 equivalent, and the point where the whole Tier 7 arc closes.
