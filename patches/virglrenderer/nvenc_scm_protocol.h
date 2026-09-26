/*
 * Wire protocol for encoding a plain (non-Venus) dma-buf via NVENC.
 *
 * Why this exists alongside VCMD_ENCODE_RESOURCE: that command resolves a
 * dma-buf through virglrenderer's own resource table via a Venus resource
 * id - correct and proven for any buffer this project's own Venus
 * allocation path (nvidia_venus.c) created, but fundamentally unable to
 * name a buffer that was never registered as a Venus resource in the first
 * place. A real Surface-sourced capture buffer (GraphicBufferSource, the
 * path any screen-recording app uses) is exactly that: a genuine,
 * importable dma-buf, just not one Venus itself ever allocated - confirmed
 * on real hardware 2026-09-26 (DEVLOG), including that
 * DRM_IOCTL_VIRTGPU_RESOURCE_INFO is meaningless against it since
 * /dev/dri/renderD128 here is the real NVIDIA render node, not a virtio-gpu
 * device (there is no virtio-gpu kernel driver anywhere in this
 * container-based, not VM-based, architecture).
 *
 * redroid-hwenc's own VA-API daemon already solved the equivalent problem
 * for its own encoder, by the only mechanism that actually applies here:
 * since redroid shares one real kernel between guest and host
 * (containerization, not virtualization), a dma-buf fd can move directly
 * across that boundary over a plain AF_UNIX socket with SCM_RIGHTS - no
 * Venus/virtio-gpu resource-id abstraction needed at all. This header is
 * this project's own copy of that same shape of protocol, calling straight
 * into vtest_gpu_encode_dmabuf() - the exact same NVENC pipeline
 * VCMD_ENCODE_RESOURCE already uses, since that function only ever needed a
 * dma-buf fd and its layout, never anything Venus-specific.
 *
 * Transport: SOCK_STREAM AF_UNIX. One connection = one encode request/
 * response (matches VA-API's daemon's own scope for the same reason: prove
 * the round trip first, a real streaming protocol is a later concern).
 *
 * Request: sendmsg() with EncodeRequest as the regular data and the dma-buf
 * fd as SCM_RIGHTS ancillary data, in a single call.
 *
 * Response: EncodeResponse header, then (if status == 0) exactly
 * `coded_size` bytes of Annex-B H.264.
 */

#ifndef NVENC_SCM_PROTOCOL_H
#define NVENC_SCM_PROTOCOL_H

#include <stdint.h>

/*
 * This path is only meaningful from the guest's point of view - /dev/venus
 * is the container's own bind-mount of the host's venus-sock directory (the
 * same one venus.sock itself lives in). The host-side listener
 * (nvenc_scm_listener.c) does not use this constant at all: it runs on the
 * real host, where /dev/venus doesn't exist, and instead derives its own
 * bind path from the real venus.sock path it was started with.
 */
#define NVENC_SCM_SOCKET_PATH "/dev/venus/nvenc-scm.sock"

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t drm_format;   /* DRM fourcc, e.g. DRM_FORMAT_ABGR8888 */
    uint32_t stride;       /* bytes */
    uint64_t modifier;     /* DRM format modifier; 0 if not identifiable on
                             * the sending side - the host will re-derive
                             * the real modifier for this format/driver
                             * itself in that case rather than trust a
                             * guessed value (see vtest_gpu_encode.c). */
} EncodeRequest;

typedef struct {
    int32_t status;      /* 0 = ok, negative = -errno */
    uint32_t coded_size; /* bytes of Annex-B H.264 following this header, 0 if status != 0 */
} EncodeResponse;

#endif /* NVENC_SCM_PROTOCOL_H */
