/*
 * Standalone AF_UNIX/SCM_RIGHTS listener for encoding a plain (non-Venus)
 * dma-buf via NVENC. See nvenc_scm_protocol.h for the full "why".
 *
 * Runs as its own thread inside virgl_test_server, entirely independent of
 * the main vtest wire-protocol loop - this is a completely separate
 * transport for a completely different class of buffer, not an extension
 * of VCMD_ENCODE_RESOURCE's own handling. Calls straight into
 * vtest_gpu_encode_dmabuf(), the exact same NVENC pipeline
 * VCMD_ENCODE_RESOURCE already uses (that function only ever needed a
 * dma-buf fd and its layout, nothing Venus-specific) - protected by that
 * module's own internal mutex, so this listener needs none of its own.
 */

#include "nvenc_scm_listener.h"

#include <errno.h>
#include <libgen.h>
#include <pthread.h>
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
   if (status == 0 && size > 0)
      write(conn_fd, buf, size);
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

static void *
listener_thread(void *arg)
{
   char *path = (char *)arg; /* malloc'd by the starter, owned by this thread */

   unlink(path);

   int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
   if (listen_fd < 0) {
      fprintf(stderr, "nvenc_scm_listener: socket() failed: %s\n", strerror(errno));
      free(path);
      return NULL;
   }
   struct sockaddr_un addr = {0};
   addr.sun_family = AF_UNIX;
   strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
   if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
      fprintf(stderr, "nvenc_scm_listener: bind(%s) failed: %s\n", path, strerror(errno));
      close(listen_fd);
      free(path);
      return NULL;
   }
   chmod(path, 0666);
   if (listen(listen_fd, 4) != 0) {
      fprintf(stderr, "nvenc_scm_listener: listen() failed: %s\n", strerror(errno));
      close(listen_fd);
      free(path);
      return NULL;
   }
   fprintf(stderr, "nvenc_scm_listener: listening on %s\n", path);
   free(path);

   while (1) {
      int conn_fd = accept(listen_fd, NULL, NULL);
      if (conn_fd < 0) {
         if (errno == EINTR)
            continue;
         fprintf(stderr, "nvenc_scm_listener: accept() failed: %s\n", strerror(errno));
         break;
      }
      handle_connection(conn_fd);
      close(conn_fd);
   }

   close(listen_fd);
   return NULL;
}

void
vtest_nvenc_scm_listener_start(const char *venus_socket_path)
{
   /* dirname() may modify its argument - work on a private copy, twice
    * (once for the dirname() call itself, since some implementations
    * return a pointer into their input). */
   char *dir_buf = strdup(venus_socket_path ? venus_socket_path : ".");
   const char *dir = dirname(dir_buf);
   char *path = malloc(strlen(dir) + strlen("/nvenc-scm.sock") + 1);
   sprintf(path, "%s/nvenc-scm.sock", dir);
   free(dir_buf);

   pthread_t tid;
   if (pthread_create(&tid, NULL, listener_thread, path) == 0)
      pthread_detach(tid);
   else {
      fprintf(stderr, "nvenc_scm_listener: pthread_create failed: %s\n", strerror(errno));
      free(path);
   }
}
