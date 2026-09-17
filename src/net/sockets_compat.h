#ifndef FB_SOCKETS_COMPAT_H
#define FB_SOCKETS_COMPAT_H

#include <stddef.h>

/*
 * Opaque loopback TCP socket. Exactly one of platform/win/sock_win.c or
 * platform/posix/sock_posix.c is compiled in (CMake source-list selection),
 * each fully implementing this header with zero shared #ifdef branching —
 * same pattern as tui/screen.h.
 */

typedef struct FbSocket FbSocket;

/* WSAStartup/WSACleanup on Windows, no-ops on POSIX. Call once for the
   whole process lifetime, bracketing main(). */
void fb_sock_platform_init(void);
void fb_sock_platform_cleanup(void);

/* Connects to 127.0.0.1:port. A refused loopback connection returns
   quickly (immediate RST), so no separate connect timeout is needed here.
   Returns NULL on failure. */
FbSocket *fb_sock_connect_loopback(int port);

/* Returns bytes sent, or -1 on error. */
int fb_sock_send(FbSocket *sock, const char *data, size_t len);

/* Returns bytes received (0 = orderly close), or -1 on error/timeout. */
int fb_sock_recv(FbSocket *sock, char *buf, size_t cap, int timeout_ms);

void fb_sock_close(FbSocket *sock);

/* Binds an ephemeral TCP port on 127.0.0.1, reads back which port the OS
   assigned, then immediately releases it. There is an inherent (and, in
   this ecosystem, already-accepted — calibr's own offload calibration uses
   the identical bind-port-0/read-back/close/reuse pattern) TOCTOU race
   between this call returning and the real server binding that same port;
   acceptable for a locally-spawned process on a single-user machine.
   Returns -1 on failure. */
int fb_sock_alloc_ephemeral_port(void);

#endif /* FB_SOCKETS_COMPAT_H */
