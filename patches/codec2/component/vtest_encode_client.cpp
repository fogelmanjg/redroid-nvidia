#define LOG_TAG "nvenc_vtest_client"

#include "vtest_encode_client.h"

#include <drm/virtgpu_drm.h>
#include <errno.h>
#include <log/log.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/system_properties.h>
#include <sys/un.h>
#include <unistd.h>
#include <xf86drm.h>

/* Mirrors nvidia_venus.c's own constants/helpers exactly - kept independent
 * (not #include-shared with minigbm) since this runs in a different process
 * with its own build target, and the pieces needed here are small. */
#define DEFAULT_SOCKET "/dev/venus/venus.sock"

/* From redroid-nvidia's vtest/vtest_protocol.h - only the subset this
 * client needs. See patches/virglrenderer/README.md for the full command
 * definitions and rationale. */
#define VTEST_HDR_SIZE 2
#define VTEST_CMD_LEN 0
#define VTEST_CMD_ID 1
#define VTEST_CMD_DATA_START 2

#define VCMD_CREATE_RENDERER 8
#define VCMD_ENCODE_RESOURCE 45
#define VCMD_ENCODE_RESOURCE_SIZE 7
#define VCMD_ENCODE_RESOURCE_RESP_SIZE 2
#define VCMD_ENCODE_RESOURCE_RESP_STATUS 0
#define VCMD_ENCODE_RESOURCE_RESP_BYTES 1

static int sock_write_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int sock_read_all(int fd, void *buf, size_t len) {
    char *p = (char *)buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int vtest_connect(void) {
    char path[PROP_VALUE_MAX];
    if (__system_property_get("mesa.vtest.socket.name", path) <= 0)
        strncpy(path, DEFAULT_SOCKET, sizeof(path) - 1);

    int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0) return -1;

    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr))) {
        close(sock);
        return -1;
    }

    const char name[8] = "nvenc";
    const uint32_t hdr[VTEST_HDR_SIZE] = {sizeof(name), VCMD_CREATE_RENDERER};
    if (sock_write_all(sock, hdr, sizeof(hdr)) || sock_write_all(sock, name, sizeof(name))) {
        close(sock);
        return -1;
    }
    return sock;
}

uint32_t vtest_encode_resolve_res_id(int render_node_fd, int dmabuf_fd) {
    struct drm_prime_handle prime_handle = {};
    prime_handle.fd = dmabuf_fd;
    if (drmIoctl(render_node_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime_handle)) {
        ALOGE("DRM_IOCTL_PRIME_FD_TO_HANDLE failed: %s", strerror(errno));
        return 0;
    }

    struct drm_virtgpu_resource_info info = {};
    info.bo_handle = prime_handle.handle;
    if (drmIoctl(render_node_fd, DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &info)) {
        ALOGE("DRM_IOCTL_VIRTGPU_RESOURCE_INFO failed: %s", strerror(errno));
        return 0;
    }
    return info.res_handle;
}

int vtest_encode_resource(uint32_t res_id, uint32_t width, uint32_t height,
                          uint32_t drm_format, uint32_t stride, uint64_t modifier,
                          uint8_t **out_buf, uint32_t *out_len) {
    int sock = vtest_connect();
    if (sock < 0) {
        ALOGE("vtest_connect failed: %s", strerror(errno));
        return -ENOTCONN;
    }

    const uint32_t req[VTEST_HDR_SIZE + VCMD_ENCODE_RESOURCE_SIZE] = {
            VCMD_ENCODE_RESOURCE_SIZE,
            VCMD_ENCODE_RESOURCE,
            res_id,
            width,
            height,
            drm_format,
            stride,
            (uint32_t)modifier,
            (uint32_t)(modifier >> 32),
    };
    if (sock_write_all(sock, req, sizeof(req))) {
        ALOGE("VCMD_ENCODE_RESOURCE write failed: %s", strerror(errno));
        close(sock);
        return -EIO;
    }

    uint32_t resp[VTEST_HDR_SIZE + VCMD_ENCODE_RESOURCE_RESP_SIZE];
    if (sock_read_all(sock, resp, sizeof(resp))) {
        ALOGE("VCMD_ENCODE_RESOURCE read header failed: %s", strerror(errno));
        close(sock);
        return -EIO;
    }
    uint32_t status = resp[VTEST_CMD_DATA_START + VCMD_ENCODE_RESOURCE_RESP_STATUS];
    uint32_t nbytes = resp[VTEST_CMD_DATA_START + VCMD_ENCODE_RESOURCE_RESP_BYTES];
    if (status) {
        ALOGE("VCMD_ENCODE_RESOURCE host status=%u", status);
        close(sock);
        return -EIO;
    }

    uint8_t *buf = (uint8_t *)malloc(nbytes);
    if (!buf) {
        close(sock);
        return -ENOMEM;
    }
    if (nbytes && sock_read_all(sock, buf, nbytes)) {
        ALOGE("VCMD_ENCODE_RESOURCE read payload failed: %s", strerror(errno));
        free(buf);
        close(sock);
        return -EIO;
    }
    close(sock);

    *out_buf = buf;
    *out_len = nbytes;
    return 0;
}
