#include "net/sockets_compat.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdlib.h>
#include <string.h>

struct FbSocket {
    SOCKET fd;
};

void fb_sock_platform_init(void) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
}

void fb_sock_platform_cleanup(void) { WSACleanup(); }

FbSocket *fb_sock_connect_loopback(int port) {
    SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) return NULL;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(fd);
        return NULL;
    }

    FbSocket *sock = (FbSocket *)malloc(sizeof(FbSocket));
    if (!sock) {
        closesocket(fd);
        return NULL;
    }
    sock->fd = fd;
    return sock;
}

int fb_sock_send(FbSocket *sock, const char *data, size_t len) {
    int n = send(sock->fd, data, (int)len, 0);
    return n;
}

int fb_sock_recv(FbSocket *sock, char *buf, size_t cap, int timeout_ms) {
    DWORD tv = (DWORD)timeout_ms;
    setsockopt(sock->fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    int n = recv(sock->fd, buf, (int)cap, 0);
    return n;
}

void fb_sock_close(FbSocket *sock) {
    if (!sock) return;
    closesocket(sock->fd);
    free(sock);
}

int fb_sock_alloc_ephemeral_port(void) {
    SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(fd);
        return -1;
    }

    struct sockaddr_in assigned;
    int len = sizeof(assigned);
    if (getsockname(fd, (struct sockaddr *)&assigned, &len) != 0) {
        closesocket(fd);
        return -1;
    }

    int port = ntohs(assigned.sin_port);
    closesocket(fd);
    return port;
}
