/*
 * Guest-side (bionic) client for redroid-nvidia's VCMD_ENCODE_RESOURCE - the
 * host-side vtest command that encodes an existing Venus resource to H.264
 * via NVENC (see redroid-nvidia's patches/virglrenderer/vtest_gpu_encode.c
 * and its README.md for the host side, confirmed end to end on real
 * hardware from a standalone client before this one existed).
 *
 * Two real, distinct steps, deliberately kept as free functions rather than
 * folded into the Codec2 component itself, since either one could be reused
 * on its own (screencap-style debugging, other future Codec2 components):
 *
 *   1. Resolve a gralloc buffer's dma-buf fd to the Venus/virtio-gpu
 *      *global* resource id NVENC needs to reference (a different id space
 *      than the local GEM handle minigbm's nvidia_venus.c already computes
 *      for its own purposes) - via the guest kernel's own
 *      DRM_IOCTL_VIRTGPU_RESOURCE_INFO, a standard virtio-gpu ioctl, no
 *      Venus/minigbm changes needed for this half either.
 *   2. Open a vtest connection (same socket/property nvidia_venus.c
 *      connects to - a *different* connection is fine, virglrenderer's
 *      resource table is global across connections, not per-context; see
 *      redroid-nvidia's DEVLOG 2026-09-25 for how that was confirmed) and
 *      issue VCMD_ENCODE_RESOURCE.
 */

#ifndef NVENC_CODEC2_VTEST_ENCODE_CLIENT_H
#define NVENC_CODEC2_VTEST_ENCODE_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Resolves a dma-buf fd (as found in a gralloc native_handle_t) to its
 * Venus/virtio-gpu global resource id.
 *
 * render_node_fd: an already-open fd to the same render node minigbm's
 *     nvidia_venus.c backend uses (a *separate* open() of the same device
 *     is fine - DRM_IOCTL_VIRTGPU_RESOURCE_INFO reports a property of the
 *     resource itself, not of the caller's local handle table).
 *
 * Returns the resource id (> 0) on success, or 0 on failure.
 */
uint32_t vtest_encode_resolve_res_id(int render_node_fd, int dmabuf_fd);

/*
 * Encodes the given resource's current content to H.264 via NVENC, opening
 * a fresh vtest connection for the call (cheap; this project's protocol is
 * one connection per request, same as VCMD_RESOURCE_ALLOC_GPU's own client
 * in minigbm - a real streaming/persistent-connection protocol is future
 * work once this baseline is proven from inside a real Codec2 component).
 *
 * out_buf: on success, receives a malloc'd buffer the caller must free()
 *     holding *out_len bytes of Annex-B H.264.
 *
 * Returns 0 on success, or a negative errno.
 */
int vtest_encode_resource(uint32_t res_id, uint32_t width, uint32_t height,
                          uint32_t drm_format, uint32_t stride, uint64_t modifier,
                          uint8_t **out_buf, uint32_t *out_len);

/*
 * Encodes a plain dma-buf that is NOT a Venus resource - confirmed on real
 * hardware 2026-09-26 (see DEVLOG) that a genuine Surface-sourced capture
 * buffer (GraphicBufferSource, the path any screen-recording app uses) is
 * exactly this: a real, importable dma-buf that was simply never allocated
 * through this project's own Venus path, so vtest_encode_resolve_res_id()
 * above cannot find a resource id for it (there is nothing wrong to fix
 * there - the resource genuinely was never registered as one). Sends the fd
 * directly over a plain AF_UNIX socket with SCM_RIGHTS instead - valid here
 * specifically because redroid shares one real kernel between guest and
 * host (containerization, not virtualization), the same mechanism
 * redroid-hwenc's own VA-API daemon already uses. See
 * ../../virglrenderer/nvenc_scm_protocol.h for the host side.
 *
 * dmabuf_fd: borrowed, same as vtest_encode_resolve_res_id()'s own dmabuf_fd
 *     parameter above - never closed by this function. SCM_RIGHTS gives the
 *     host its own independent kernel-level dup.
 * modifier: pass 0 if not identifiable (as for the generic, non-cros_gralloc
 *     native_handle_t this project's own NvencEncComponent falls back to
 *     parsing) - the host re-derives the real modifier for this format
 *     itself in that case rather than trust a guessed value.
 * forceIdr: pass true on this component instance's own first call. The
 *     host's persistent encoder session only re-initializes NVENC (where
 *     repeatSPSPPS lives) on a resolution change, never on a genuinely new
 *     streaming session at the same resolution - without this, a second,
 *     unrelated client connecting at the same size silently continues the
 *     first client's session (P-frames, no SPS/PPS) instead of a fresh IDR.
 *
 * Returns 0 on success, or a negative errno.
 */
int vtest_encode_via_scm(int dmabuf_fd, uint32_t width, uint32_t height, uint32_t drm_format,
                         uint32_t stride, uint64_t modifier, bool forceIdr, uint8_t **out_buf,
                         uint32_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* NVENC_CODEC2_VTEST_ENCODE_CLIENT_H */
