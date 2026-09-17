/* Not yet runtime-tested (development happened on Windows this pass); it
   must be exercised on a real Linux/macOS machine before being relied on. */

#include "net/sockets_compat.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct FbSocket {
    int fd;
};

void fb_sock_platform_init(void) {}
void fb_sock_platform_cleanup(void) {}

FbSocket *fb_sock_connect_loopback(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return NULL;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return NULL;
    }

    FbSocket *sock = (FbSocket *)malloc(sizeof(FbSocket));
    if (!sock) {
        close(fd);
        return NULL;
    }
    sock->fd = fd;
    return sock;
}

int fb_sock_send(FbSocket *sock, const char *data, size_t len) {
    return (int)send(sock->fd, data, len, 0);
}

int fb_sock_recv(FbSocket *sock, char *buf, size_t cap, int timeout_ms) {
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return (int)recv(sock->fd, buf, cap, 0);
}

void fb_sock_close(FbSocket *sock) {
    if (!sock) return;
    close(sock->fd);
    free(sock);
}

int fb_sock_alloc_ephemeral_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_in assigned;
    socklen_t len = sizeof(assigned);
    if (getsockname(fd, (struct sockaddr *)&assigned, &len) != 0) {
        close(fd);
        return -1;
    }

    int port = ntohs(assigned.sin_port);
    close(fd);
    return port;
}
