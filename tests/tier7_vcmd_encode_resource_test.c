/* Standalone test client for VCMD_ENCODE_RESOURCE - no Android/redroid
 * involved, same "no Android boot needed" spirit as tests/list_exts.c and
 * Tier 3's vnprobe.c. Speaks the raw vtest wire protocol directly (like
 * minigbm's nvidia_venus.c does for VCMD_RESOURCE_ALLOC_GPU) rather than
 * going through a real Vulkan/Venus ICD, since the goal here is to exercise
 * the new command's server-side plumbing, not re-prove the Vulkan/CUDA/NVENC
 * math (already confirmed twice - see tests/tier7_nvenc_*.c).
 *
 * Sequence: connect -> VCMD_CREATE_RENDERER -> VCMD_RESOURCE_ALLOC_GPU (get a
 * real dma_buf fd on the render GPU) -> VCMD_CONTEXT_INIT(capset=1) ->
 * VCMD_RESOURCE_IMPORT_BLOB (register that exact fd as a tracked resource,
 * getting a real res_id back - standing in for however Venus would normally
 * end up owning a resource) -> VCMD_ENCODE_RESOURCE(res_id) -> H.264 bytes.
 *
 * Build: gcc -O1 -o test_encode_resource test_encode_resource.c
 * Run: VTEST_SOCKET_NAME=/path/to/vtest.sock ./test_encode_resource
 * (start virgl_test_server first, e.g. ./virgl_test_server --venus)
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define VTEST_HDR_SIZE 2
#define VTEST_CMD_LEN 0
#define VTEST_CMD_ID 1
#define VTEST_CMD_DATA_START 2

#define VCMD_CREATE_RENDERER 8
#define VCMD_CONTEXT_INIT 20
#define VCMD_RESOURCE_IMPORT_BLOB 40
#define VCMD_RESOURCE_ALLOC_GPU 41
#define VCMD_ENCODE_RESOURCE 45

#define VCMD_CONTEXT_INIT_SIZE 1
#define VCMD_CONTEXT_INIT_CAPSET_ID 0

#define VCMD_RESOURCE_ALLOC_GPU_SIZE 4
#define VCMD_RESOURCE_ALLOC_GPU_WIDTH 0
#define VCMD_RESOURCE_ALLOC_GPU_HEIGHT 1
#define VCMD_RESOURCE_ALLOC_GPU_FORMAT 2
#define VCMD_RESOURCE_ALLOC_GPU_FLAGS 3
#define VCMD_RESOURCE_ALLOC_GPU_RESP_SIZE 7
#define VCMD_RESOURCE_ALLOC_GPU_RESP_STATUS 0
#define VCMD_RESOURCE_ALLOC_GPU_RESP_STRIDE 1
#define VCMD_RESOURCE_ALLOC_GPU_RESP_MAP_STRIDE 2
#define VCMD_RESOURCE_ALLOC_GPU_RESP_MODIFIER_LO 3
#define VCMD_RESOURCE_ALLOC_GPU_RESP_MODIFIER_HI 4
#define VCMD_RESOURCE_ALLOC_GPU_RESP_SIZE_LO 5
#define VCMD_RESOURCE_ALLOC_GPU_RESP_SIZE_HI 6

#define VCMD_RESOURCE_IMPORT_BLOB_SIZE 2
#define VCMD_RESOURCE_IMPORT_BLOB_SIZE_LO 0
#define VCMD_RESOURCE_IMPORT_BLOB_SIZE_HI 1

#define VCMD_ENCODE_RESOURCE_SIZE 7
#define VCMD_ENCODE_RESOURCE_RES_ID 0
#define VCMD_ENCODE_RESOURCE_WIDTH 1
#define VCMD_ENCODE_RESOURCE_HEIGHT 2
#define VCMD_ENCODE_RESOURCE_FORMAT 3
#define VCMD_ENCODE_RESOURCE_STRIDE 4
#define VCMD_ENCODE_RESOURCE_MODIFIER_LO 5
#define VCMD_ENCODE_RESOURCE_MODIFIER_HI 6
#define VCMD_ENCODE_RESOURCE_RESP_SIZE 2
#define VCMD_ENCODE_RESOURCE_RESP_STATUS 0
#define VCMD_ENCODE_RESOURCE_RESP_BYTES 1

#define DRM_FORMAT_XRGB8888 0x34325258

static int sock_write_all(int fd, const void *buf, size_t len)
{
   const char *p = buf;
   while (len) {
      ssize_t n = write(fd, p, len);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) return -1;
      p += n; len -= (size_t)n;
   }
   return 0;
}

static int sock_read_all(int fd, void *buf, size_t len)
{
   char *p = buf;
   while (len) {
      ssize_t n = read(fd, p, len);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) return -1;
      p += n; len -= (size_t)n;
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
   do { n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC); } while (n < 0 && errno == EINTR);
   if (n <= 0) return -1;

   struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
   if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) return -1;
   int fd;
   memcpy(&fd, CMSG_DATA(c), sizeof(fd));
   return fd;
}

static int sock_send_fd(int sock, int fd)
{
   char data = 0;
   struct iovec iov = { &data, 1 };
   char ctrl[CMSG_SPACE(sizeof(int))];
   struct msghdr msg = { 0 };
   msg.msg_iov = &iov;
   msg.msg_iovlen = 1;
   msg.msg_control = ctrl;
   msg.msg_controllen = sizeof(ctrl);

   struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
   c->cmsg_level = SOL_SOCKET;
   c->cmsg_type = SCM_RIGHTS;
   c->cmsg_len = CMSG_LEN(sizeof(int));
   memcpy(CMSG_DATA(c), &fd, sizeof(fd));

   ssize_t n;
   do { n = sendmsg(sock, &msg, 0); } while (n < 0 && errno == EINTR);
   return n < 0 ? -1 : 0;
}

#define DIE(msg) do { fprintf(stderr, "FAIL: %s (line %d, errno=%s)\n", msg, __LINE__, strerror(errno)); exit(1); } while (0)

int main(void)
{
   const char *sock_path = getenv("VTEST_SOCKET_NAME");
   if (!sock_path)
      sock_path = "/tmp/.virgl_test";

   int sock = socket(AF_UNIX, SOCK_STREAM, 0);
   if (sock < 0) DIE("socket");
   struct sockaddr_un addr = { .sun_family = AF_UNIX };
   strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
   if (connect(sock, (struct sockaddr *)&addr, sizeof(addr))) DIE("connect");
   fprintf(stderr, "connected to %s\n", sock_path);

   /* VCMD_CREATE_RENDERER */
   {
      const char name[8] = "t7test";
      const uint32_t hdr[VTEST_HDR_SIZE] = { sizeof(name), VCMD_CREATE_RENDERER };
      if (sock_write_all(sock, hdr, sizeof(hdr)) || sock_write_all(sock, name, sizeof(name)))
         DIE("CREATE_RENDERER");
   }
   fprintf(stderr, "CREATE_RENDERER OK\n");

   /* VCMD_RESOURCE_ALLOC_GPU: 256x256 XRGB8888, GPU-only (no MAPPABLE flag) */
   int alloc_fd = -1;
   uint64_t alloc_size = 0;
   uint32_t alloc_stride = 0;
   uint64_t alloc_modifier = 0;
   const uint32_t W = 256, H = 256;
   {
      const uint32_t req[VTEST_HDR_SIZE + VCMD_RESOURCE_ALLOC_GPU_SIZE] = {
         VCMD_RESOURCE_ALLOC_GPU_SIZE, VCMD_RESOURCE_ALLOC_GPU,
         W, H, DRM_FORMAT_XRGB8888, 0,
      };
      if (sock_write_all(sock, req, sizeof(req))) DIE("ALLOC_GPU write");
      uint32_t resp[VTEST_HDR_SIZE + VCMD_RESOURCE_ALLOC_GPU_RESP_SIZE];
      if (sock_read_all(sock, resp, sizeof(resp))) DIE("ALLOC_GPU read");
      uint32_t status = resp[VTEST_CMD_DATA_START + VCMD_RESOURCE_ALLOC_GPU_RESP_STATUS];
      if (status) { fprintf(stderr, "ALLOC_GPU status=%u\n", status); exit(1); }
      alloc_stride = resp[VTEST_CMD_DATA_START + VCMD_RESOURCE_ALLOC_GPU_RESP_STRIDE];
      alloc_modifier = resp[VTEST_CMD_DATA_START + VCMD_RESOURCE_ALLOC_GPU_RESP_MODIFIER_LO] |
                      ((uint64_t)resp[VTEST_CMD_DATA_START + VCMD_RESOURCE_ALLOC_GPU_RESP_MODIFIER_HI] << 32);
      alloc_size = resp[VTEST_CMD_DATA_START + VCMD_RESOURCE_ALLOC_GPU_RESP_SIZE_LO] |
                  ((uint64_t)resp[VTEST_CMD_DATA_START + VCMD_RESOURCE_ALLOC_GPU_RESP_SIZE_HI] << 32);
      alloc_fd = sock_recv_fd(sock);
      if (alloc_fd < 0) DIE("ALLOC_GPU recv fd");
   }
   fprintf(stderr, "RESOURCE_ALLOC_GPU OK: fd=%d, size=%llu, stride=%u, modifier=0x%llx\n",
           alloc_fd, (unsigned long long)alloc_size, alloc_stride, (unsigned long long)alloc_modifier);

   /* VCMD_CONTEXT_INIT(capset=VIRTGPU_DRM_CAPSET_VENUS=4) - a plain
    * capset-less context (lazily created with capset_id=0) crashes inside
    * virgl_renderer_context_export_fence() when RESOURCE_IMPORT_BLOB's own
    * synchronous barrier runs against it; that call needs a real Venus
    * render context behind ctx_id. Confirmed via gdb backtrace - a
    * pre-existing requirement of RESOURCE_IMPORT_BLOB, not something new. */
   {
      const uint32_t req[VTEST_HDR_SIZE + VCMD_CONTEXT_INIT_SIZE] = {
         VCMD_CONTEXT_INIT_SIZE, VCMD_CONTEXT_INIT, 4,
      };
      if (sock_write_all(sock, req, sizeof(req))) DIE("CONTEXT_INIT write");
   }
   fprintf(stderr, "CONTEXT_INIT(VENUS) OK (no reply expected for this command)\n");
   usleep(500000); /* racy theory test: give the venus context's own async setup time to finish */

   /* VCMD_RESOURCE_IMPORT_BLOB: register the fd we already have, get a res_id */
   uint32_t res_id = 0;
   {
      const uint32_t req[VTEST_HDR_SIZE + VCMD_RESOURCE_IMPORT_BLOB_SIZE] = {
         VCMD_RESOURCE_IMPORT_BLOB_SIZE, VCMD_RESOURCE_IMPORT_BLOB,
         (uint32_t)alloc_size, (uint32_t)(alloc_size >> 32),
      };
      if (sock_write_all(sock, req, sizeof(req))) DIE("IMPORT_BLOB write hdr");
      if (sock_send_fd(sock, alloc_fd)) DIE("IMPORT_BLOB send fd");
      uint32_t resp[VTEST_HDR_SIZE + 1];
      if (sock_read_all(sock, resp, sizeof(resp))) DIE("IMPORT_BLOB read");
      res_id = resp[VTEST_CMD_DATA_START];
      if (!res_id) { fprintf(stderr, "IMPORT_BLOB: res_id=0 (failure)\n"); exit(1); }
   }
   fprintf(stderr, "RESOURCE_IMPORT_BLOB OK: res_id=%u\n", res_id);

   /* VCMD_ENCODE_RESOURCE(res_id, width, height, format, stride, modifier) -
    * the caller supplies the layout directly, same as a real Codec2
    * component would from its own gralloc metadata (see vtest_protocol.h's
    * comment on why: virgl_renderer_resource_get_info_ext() doesn't know
    * about opaque host3d blob resources). */
   {
      const uint32_t req[VTEST_HDR_SIZE + VCMD_ENCODE_RESOURCE_SIZE] = {
         VCMD_ENCODE_RESOURCE_SIZE, VCMD_ENCODE_RESOURCE, res_id,
         W, H, DRM_FORMAT_XRGB8888, alloc_stride,
         (uint32_t)alloc_modifier, (uint32_t)(alloc_modifier >> 32),
      };
      if (sock_write_all(sock, req, sizeof(req))) DIE("ENCODE_RESOURCE write");
      uint32_t resp[VTEST_HDR_SIZE + VCMD_ENCODE_RESOURCE_RESP_SIZE];
      if (sock_read_all(sock, resp, sizeof(resp))) DIE("ENCODE_RESOURCE read hdr");
      uint32_t status = resp[VTEST_CMD_DATA_START + VCMD_ENCODE_RESOURCE_RESP_STATUS];
      uint32_t nbytes = resp[VTEST_CMD_DATA_START + VCMD_ENCODE_RESOURCE_RESP_BYTES];
      fprintf(stderr, "ENCODE_RESOURCE reply: status=%u bytes=%u\n", status, nbytes);
      if (status) { fprintf(stderr, "ENCODE_RESOURCE FAILED, status=%u\n", status); exit(1); }
      if (nbytes) {
         uint8_t *buf = malloc(nbytes);
         if (sock_read_all(sock, buf, nbytes)) DIE("ENCODE_RESOURCE read payload");
         FILE *f = fopen("/tmp/tier7_vcmd_encode_out.h264", "wb");
         fwrite(buf, 1, nbytes, f);
         fclose(f);
         fprintf(stderr, "wrote %u bytes to /tmp/tier7_vcmd_encode_out.h264\n", nbytes);
         free(buf);
      }
   }

   fprintf(stderr, "\n*** RESULT: VCMD_ENCODE_RESOURCE round-trip OK over the real vtest wire\n");
   fprintf(stderr, "protocol, end to end - no Android/redroid involved. ***\n");
   return 0;
}
