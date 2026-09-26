/*
 * See nvenc_scm_listener.c / nvenc_scm_protocol.h.
 */

#ifndef NVENC_SCM_LISTENER_H
#define NVENC_SCM_LISTENER_H

/*
 * Starts the SCM_RIGHTS listener on its own detached thread. Safe to call
 * once at server startup; never blocks the caller.
 *
 * venus_socket_path: the real host filesystem path the main vtest server was
 *     started with (its own --socket-path argument, e.g.
 *     ~/waydroid-nvidia-test/venus-sock/venus.sock) - this listener binds
 *     its own socket in the *same directory*, since that's the directory
 *     actually bind-mounted into the container as /dev/venus (unlike
 *     /dev/venus itself, which only exists from the guest's point of view -
 *     this process runs on the real host, where no such path exists).
 */
void vtest_nvenc_scm_listener_start(const char *venus_socket_path);

#endif /* NVENC_SCM_LISTENER_H */
