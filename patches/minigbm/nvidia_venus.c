/*
 * minigbm backend for NVIDIA-on-redroid: allocates GPU buffers through a
 * host-side vtest/venus render server (virglrenderer) over a unix socket,
 * instead of through this driver's own DRM fd for the actual allocation.
 * The DRM node passed to drv_create() is used for one thing: importing the
 * dma-buf fd we get back from the host into a local GEM handle via
 * DRM_IOCTL_PRIME_FD_TO_HANDLE, because minigbm's generic plumbing
 * (drv_bo_get_plane_fd(), called by the gralloc layer above us to hand the
 * buffer's fd back to callers) assumes every backend's bo->handle is a real
 * local GEM handle it can round-trip through DRM_IOCTL_PRIME_HANDLE_TO_FD,
 * not an arbitrary fd number.
 *
 * Protocol and property names match redroid-nvidia's own standalone Tier 3
 * test (tests/vnprobe.c client against virgl_test_server) and
 * waydroid-nvidia's guest Vulkan driver, which uses the same
 * mesa.vtest.socket.name property and vtest wire format.
 */

#include <drm.h>
#include <errno.h>
#include <linux/dma-buf.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/system_properties.h>
#include <sys/un.h>
#include <unistd.h>
#include <xf86drm.h>

#include <android/log.h>
#define LOG_TAG "nvidia_venus"
#define VLOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#include "drv_helpers.h"
#include "drv_priv.h"
#include "util.h"

#define VTEST_HDR_SIZE 2
#define VCMD_CREATE_RENDERER 8
#define VCMD_RESOURCE_ALLOC_GPU 41
#define VCMD_ALLOC_GPU_FLAG_MAPPABLE (1u << 0)
#define VCMD_ALLOC_GPU_FLAG_SCANOUT (1u << 1)
#define VCMD_RESOURCE_ALLOC_GPU_RESP_SIZE 7

#define DEFAULT_SOCKET "/dev/venus/venus.sock"

struct nvidia_venus_priv {
	pthread_mutex_t mutex;
	int sock;
};

static int sock_write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	while (len) {
		ssize_t n = write(fd, p, len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int sock_read_all(int fd, void *buf, size_t len)
{
	char *p = buf;
	while (len) {
		ssize_t n = read(fd, p, len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int sock_recv_fd(int sock)
{
	char data;
	struct iovec iov = { &data, 1 };
	char ctrl[CMSG_SPACE(sizeof(int))];
	struct msghdr msg = { 0 };
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = ctrl;
	msg.msg_controllen = sizeof(ctrl);

	ssize_t n;
	do {
		n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
	} while (n < 0 && errno == EINTR);
	if (n <= 0)
		return -1;

	struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
	if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
		return -1;
	int fd;
	memcpy(&fd, CMSG_DATA(c), sizeof(fd));
	return fd;
}

static int nvidia_venus_connect(void)
{
	char path[PROP_VALUE_MAX];
	if (__system_property_get("mesa.vtest.socket.name", path) <= 0)
		strncpy(path, DEFAULT_SOCKET, sizeof(path) - 1);

	int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (sock < 0)
		return -1;

	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr))) {
		close(sock);
		return -1;
	}

	const char name[8] = "gralloc";
	const uint32_t hdr[VTEST_HDR_SIZE] = { sizeof(name), VCMD_CREATE_RENDERER };
	if (sock_write_all(sock, hdr, sizeof(hdr)) || sock_write_all(sock, name, sizeof(name))) {
		close(sock);
		return -1;
	}
	return sock;
}

static const uint32_t nvidia_venus_formats[] = {
	DRM_FORMAT_R8,	      DRM_FORMAT_RGB565,      DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XBGR8888, DRM_FORMAT_ABGR8888,    DRM_FORMAT_ABGR2101010,
	DRM_FORMAT_ABGR16161616F,
};

static int nvidia_venus_init(struct driver *drv)
{
	struct nvidia_venus_priv *priv = calloc(1, sizeof(*priv));
	if (!priv)
		return -ENOMEM;
	pthread_mutex_init(&priv->mutex, NULL);
	priv->sock = -1;
	drv->priv = priv;

	drv_add_combinations(drv, nvidia_venus_formats, ARRAY_SIZE(nvidia_venus_formats),
			     &LINEAR_METADATA, BO_USE_RENDER_MASK | BO_USE_SCANOUT | BO_USE_TEXTURE_MASK);

	return drv_modify_linear_combinations(drv);
}

static void nvidia_venus_close(struct driver *drv)
{
	struct nvidia_venus_priv *priv = drv->priv;
	if (priv->sock >= 0)
		close(priv->sock);
	free(priv);
	drv->priv = NULL;
}

static int nvidia_venus_bo_create(struct bo *bo, uint32_t width, uint32_t height, uint32_t format,
				  uint64_t use_flags)
{
	struct nvidia_venus_priv *priv = bo->drv->priv;

	uint32_t flags = 0;
	if (use_flags & (BO_USE_SW_READ_OFTEN | BO_USE_SW_WRITE_OFTEN | BO_USE_SW_READ_RARELY |
			 BO_USE_SW_WRITE_RARELY | BO_USE_LINEAR))
		flags |= VCMD_ALLOC_GPU_FLAG_MAPPABLE;
	if (use_flags & BO_USE_SCANOUT)
		flags |= VCMD_ALLOC_GPU_FLAG_SCANOUT;
	VLOGE("bo_create %ux%u format=0x%08x use_flags=0x%llx -> alloc_flags=0x%x", width, height,
	      format, (unsigned long long)use_flags, flags);

	const uint32_t req[VTEST_HDR_SIZE + 4] = {
		4, VCMD_RESOURCE_ALLOC_GPU, width, height, format, flags,
	};
	uint32_t resp[VTEST_HDR_SIZE + VCMD_RESOURCE_ALLOC_GPU_RESP_SIZE];

	pthread_mutex_lock(&priv->mutex);

	int fd = -1;
	for (int attempt = 0; attempt < 2; attempt++) {
		if (priv->sock < 0)
			priv->sock = nvidia_venus_connect();
		if (priv->sock < 0)
			break;
		if (sock_write_all(priv->sock, req, sizeof(req)) == 0 &&
		    sock_read_all(priv->sock, resp, sizeof(resp)) == 0)
			goto have_resp;
		close(priv->sock);
		priv->sock = -1;
	}
	pthread_mutex_unlock(&priv->mutex);
	VLOGE("bo_create %ux%u: no vtest connection", width, height);
	return -ENODEV;

have_resp:;
	const uint32_t *d = &resp[VTEST_HDR_SIZE];
	const uint32_t status = d[0];
	if (status) {
		pthread_mutex_unlock(&priv->mutex);
		VLOGE("bo_create %ux%u: host status %u", width, height, status);
		return -(int)status;
	}

	fd = sock_recv_fd(priv->sock);
	pthread_mutex_unlock(&priv->mutex);
	if (fd < 0) {
		VLOGE("bo_create %ux%u: fd receive failed", width, height);
		return -EIO;
	}

	struct drm_prime_handle prime_handle = { .fd = fd };
	int ret = drmIoctl(bo->drv->fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime_handle);
	close(fd);
	if (ret) {
		VLOGE("bo_create %ux%u: PRIME_FD_TO_HANDLE failed: %s", width, height,
		      strerror(errno));
		return -errno;
	}
	VLOGE("bo_create %ux%u: OK handle=%u stride=%u", width, height, prime_handle.handle, d[1]);

	uint32_t stride = d[1];
	uint64_t modifier = d[3] | (uint64_t)d[4] << 32;

	bo->meta.width = width;
	bo->meta.height = height;
	bo->meta.format = format;
	bo->meta.tiling = 0;
	bo->meta.num_planes = 1;
	bo->meta.offsets[0] = 0;
	bo->meta.strides[0] = stride;
	bo->meta.sizes[0] = stride * height;
	bo->meta.total_size = bo->meta.sizes[0];
	bo->meta.format_modifier = modifier;
	bo->meta.use_flags = use_flags;

	bo->handle.u32 = prime_handle.handle;
	return 0;
}

/*
 * bo_import fills bo->meta itself (unlike bo_create's caller, cros_gralloc
 * re-derives metadata from the imported handle on its own for imports), so
 * only the fd->handle conversion matters here. drv_prime_bo_import already
 * does exactly that via DRM_IOCTL_PRIME_FD_TO_HANDLE against bo->drv->fd -
 * the same local render node this backend was created against - so it's a
 * correct, direct reuse rather than reimplementing the same ioctl.
 */

static void *nvidia_venus_bo_map(struct bo *bo, struct vma *vma, uint32_t map_flags)
{
	int fd = drv_bo_get_plane_fd(bo, 0);
	if (fd < 0)
		return MAP_FAILED;

	void *addr = mmap(NULL, bo->meta.total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (addr == MAP_FAILED)
		return MAP_FAILED;

	vma->length = bo->meta.total_size;
	return addr;
}

/*
 * The memory backing this bo (a host memfd exported through /dev/udmabuf,
 * for MAPPABLE allocations - see vtest_gpu_alloc_cpu on the host side) is
 * real host RAM shared with the guest's Vulkan/Venus GPU work through the
 * same kernel, not a virtualized transport - so DMA_BUF_IOCTL_SYNC is the
 * correct, generic synchronization primitive here (unlike a device-specific
 * ioctl such as i915's SET_DOMAIN, which assumes a local device backing the
 * buffer). Genuinely correct minigbm behavior for a dma-buf-backed CPU path
 * regardless of whether any given caller happens to invoke it.
 *
 * NOTE(redroid): added while chasing non-deterministic visual corruption in
 * screencap output (Tier 5) - confirmed via logging that cros_gralloc's
 * IMapper v5 lock()/unlock() path (mapper_stablec/Mapper.cpp) never actually
 * calls drv_bo_invalidate()/drv_bo_flush() for this buffer, so this fix
 * alone did NOT resolve that corruption. Kept because it's correct anyway,
 * and useful for any caller that does explicitly call
 * flushLockedBuffer()/rereadLockedBuffer(). The real corruption's leading
 * hypothesis, per DEVLOG's 2026-09-24 entry, is a host-side race in the
 * brand-new VCMD_SYNC_EXPORT_SYNC_FILE path: vn_create_sync_file() submits
 * GPU work and immediately asks to export a sync_file for it over the same
 * synchronous vtest socket, and the host's vtest_sync_export_sync_file()
 * reports "already signaled" (skipping any wait) whenever it doesn't find a
 * matching pending timeline submit - a lookup that could plausibly race the
 * host's own bookkeeping for that just-submitted work.
 */
static int nvidia_venus_bo_sync(struct bo *bo, uint32_t map_flags, uint64_t dma_buf_sync_end)
{
	int fd = drv_bo_get_plane_fd(bo, 0);
	if (fd < 0)
		return -errno;

	uint64_t flags = dma_buf_sync_end;
	if (map_flags & BO_MAP_READ)
		flags |= DMA_BUF_SYNC_READ;
	if (map_flags & BO_MAP_WRITE)
		flags |= DMA_BUF_SYNC_WRITE;

	struct dma_buf_sync sync = { .flags = flags };
	int ret = drmIoctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
	close(fd);
	if (ret) {
		VLOGE("DMA_BUF_IOCTL_SYNC flags=0x%llx failed: %s", (unsigned long long)flags,
		      strerror(errno));
		return -errno;
	}
	VLOGE("DMA_BUF_IOCTL_SYNC flags=0x%llx OK", (unsigned long long)flags);
	return 0;
}

static int nvidia_venus_bo_invalidate(struct bo *bo, struct mapping *mapping)
{
	return nvidia_venus_bo_sync(bo, mapping->vma->map_flags, DMA_BUF_SYNC_START);
}

static int nvidia_venus_bo_flush(struct bo *bo, struct mapping *mapping)
{
	return nvidia_venus_bo_sync(bo, mapping->vma->map_flags, DMA_BUF_SYNC_END);
}

static int nvidia_venus_resource_info(struct bo *bo, uint32_t strides[DRV_MAX_PLANES],
				      uint32_t offsets[DRV_MAX_PLANES], uint64_t *format_modifier)
{
	strides[0] = bo->meta.strides[0];
	offsets[0] = bo->meta.offsets[0];
	*format_modifier = bo->meta.format_modifier;
	return 0;
}

const struct backend backend_nvidia_venus = {
	.name = "nvidia-drm",
	.init = nvidia_venus_init,
	.close = nvidia_venus_close,
	.bo_create = nvidia_venus_bo_create,
	.bo_import = drv_prime_bo_import,
	.bo_destroy = drv_gem_bo_destroy,
	.bo_release = drv_gem_bo_destroy,
	.bo_map = nvidia_venus_bo_map,
	.bo_unmap = drv_bo_munmap,
	.bo_invalidate = nvidia_venus_bo_invalidate,
	.bo_flush = nvidia_venus_bo_flush,
	.resource_info = nvidia_venus_resource_info,
	.resolve_format_and_use_flags = drv_resolve_format_and_use_flags_helper,
};
