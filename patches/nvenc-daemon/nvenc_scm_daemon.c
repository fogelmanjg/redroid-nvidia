/*
 * Standalone host-side daemon for encoding a plain (non-Venus) dma-buf via
 * NVENC over SCM_RIGHTS. See nvenc_scm_protocol.h for the full "why".
 *
 * This used to run as a thread inside virgl_test_server itself
 * (nvenc_scm_listener.c, started via pthread_create() from vtest_main.c) -
 * moved out into its own process after a real, confirmed regression: mixing
 * a long-lived background pthread into a process that also fork()s (as
 * virgl_test_server does internally, once per Venus context, to spawn its
 * own virgl_render_server child) is a classic, well-known hazard. If the
 * SCM thread happens to hold any libc-internal lock (malloc's arena lock,
 * the dynamic linker's own lock from a dlopen()) at the exact moment
 * another part of the process calls fork(), the child inherits that lock
 * already held, forever, since only the forking thread survives into the
 * child - any later call in the child needing that same lock (essentially
 * any malloc()) deadlocks silently. Confirmed as the actual cause on real
 * hardware: normal rendering (nothing to do with this component at all)
 * went from occasionally crashing to consistently rendering a blank white
 * screen the day this thread was added - reverting to the pristine,
 * unmodified virgl_test_server binary immediately restored correct
 * rendering. Running this as a genuinely separate process removes the
 * hazard entirely (no shared address space, no shared locks), and happens
 * to match redroid-hwenc's own VA-API daemon architecture exactly - see
 * the main README's discussion of unifying the two projects' approach.
 *
 * Deliberately still calls straight into vtest_gpu_encode_dmabuf() - this
 * daemon's own persistent encoder session lives here, independent of
 * virgl_test_server's own (used for VCMD_ENCODE_RESOURCE) - the two
 * transports were always independent, this just makes the process boundary
 * match that independence.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "nvenc_scm_protocol.h"
#include "vtest_gpu_encode.h"

static int
recv_request(int conn_fd, EncodeRequest *req, int *out_fd)
{
   char cmsg_buf[CMSG_SPACE(sizeof(int))];
   struct iovec iov = { .iov_base = req, .iov_len = sizeof(*req) };
   struct msghdr msg = {0};
   msg.msg_iov = &iov;
   msg.msg_iovlen = 1;
   msg.msg_control = cmsg_buf;
   msg.msg_controllen = sizeof(cmsg_buf);

   ssize_t n = recvmsg(conn_fd, &msg, 0);
   if (n != (ssize_t)sizeof(*req))
      return -1;

   struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
   if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS)
      return -1;
   memcpy(out_fd, CMSG_DATA(cmsg), sizeof(int));
   return 0;
}

static void
send_response(int conn_fd, int32_t status, const uint8_t *buf, uint32_t size)
{
   EncodeResponse resp = { .status = status, .coded_size = (status == 0) ? size : 0 };
   if (write(conn_fd, &resp, sizeof(resp)) != (ssize_t)sizeof(resp))
      return;
   if (status == 0 && size > 0) {
      if (write(conn_fd, buf, size) < 0) { /* best effort */ }
   }
}

static void
handle_connection(int conn_fd)
{
   EncodeRequest req;
   int fd = -1;
   if (recv_request(conn_fd, &req, &fd) != 0) {
      send_response(conn_fd, -EINVAL, NULL, 0);
      return;
   }

   uint64_t modifier = req.modifier;
   if (!modifier)
      modifier = vtest_gpu_encode_discover_modifier(req.drm_format);

   if (req.force_idr)
      vtest_gpu_encode_force_idr();

   const uint8_t *coded = NULL;
   uint32_t coded_len = 0;
   /* vtest_gpu_encode_dmabuf() consumes fd either way. */
   int ret = vtest_gpu_encode_dmabuf(fd, req.width, req.height, req.drm_format, modifier,
                                      req.stride, &coded, &coded_len);
   if (ret) {
      send_response(conn_fd, ret, NULL, 0);
      return;
   }
   send_response(conn_fd, 0, coded, coded_len);
}

int
main(int argc, char **argv)
{
   if (argc != 2) {
      fprintf(stderr, "usage: %s <socket-path>\n", argv[0]);
      return 1;
   }
   const char *socket_path = argv[1];

   /* This daemon never forks and never spawns threads - a real dma-buf fd
    * leak or a stuck peer should only ever affect its own single
    * connection handler, never require special signal handling here. */
   signal(SIGPIPE, SIG_IGN);

   unlink(socket_path);

   int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
   if (listen_fd < 0) {
      perror("socket");
      return 1;
   }
   struct sockaddr_un addr = {0};
   addr.sun_family = AF_UNIX;
   strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
   if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
      fprintf(stderr, "bind(%s) failed: %s\n", socket_path, strerror(errno));
      return 1;
   }
   chmod(socket_path, 0666);
   if (listen(listen_fd, 4) != 0) {
      perror("listen");
      return 1;
   }
   fprintf(stderr, "nvenc_scm_daemon: listening on %s\n", socket_path);

   while (1) {
      int conn_fd = accept(listen_fd, NULL, NULL);
      if (conn_fd < 0) {
         if (errno == EINTR)
            continue;
         perror("accept");
         break;
      }
      handle_connection(conn_fd);
      close(conn_fd);
   }

   close(listen_fd);
   return 0;
}
