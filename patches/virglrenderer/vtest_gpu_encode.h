/*
 * Host-side NVENC encoder for VCMD_ENCODE_RESOURCE.
 *
 * Takes a dma_buf fd for an EXISTING resource (as returned by
 * virgl_renderer_resource_export_blob() - this module never creates or
 * modifies any Venus/virgl resource itself), plain-imports it into a private
 * Vulkan device, copies it (GPU-to-GPU, both images OPTIMAL/tiled
 * throughout) into a persistent, dual-exportable image this module owns,
 * and encodes that copy via NVENC - CUDA external-memory interop, not a
 * VA-API-style dma-buf import. See redroid-nvidia's README/DEVLOG (Tier 7,
 * 2026-09-25) for why the copy step is required (a driver-reported
 * incompatibility between DMA_BUF and OPAQUE_FD external memory handle types
 * for one and the same allocation) and why it doesn't need Tier 5's fix
 * (both images stay OPTIMAL; nothing here is ever CPU-mapped).
 */

#ifndef VTEST_GPU_ENCODE_H
#define VTEST_GPU_ENCODE_H

#include <stdint.h>

/*
 * Encodes one frame from the given resource's current content.
 *
 * fd: a dma_buf fd for the resource (consumed on success or failure - the
 *     caller should not use it afterward either way).
 * drm_format, modifier, stride: the resource's real layout, as reported by
 *     virgl_renderer_resource_get_info_ext().
 * out_buf/out_len: on success, *out_buf points at an internal buffer valid
 *     until the next call on this thread (copy it before calling again),
 *     holding *out_len bytes of Annex-B H.264.
 *
 * Returns 0 on success, or a negative errno.
 */
int vtest_gpu_encode_dmabuf(int fd, uint32_t width, uint32_t height,
                            uint32_t drm_format, uint64_t modifier,
                            uint32_t stride, const uint8_t **out_buf,
                            uint32_t *out_len);

/*
 * Asks the driver for its own real modifier for a DRM format - used by the
 * SCM_RIGHTS listener (nvenc_scm_listener.c) for buffers whose sender has
 * no reliable modifier of its own to report. Returns 0
 * (DRM_FORMAT_MOD_LINEAR) if nothing better is found.
 */
uint64_t vtest_gpu_encode_discover_modifier(uint32_t drm_format);

#endif /* VTEST_GPU_ENCODE_H */
