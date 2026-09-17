#include "net/http_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/sockets_compat.h"

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} DynBuf;

static int dynbuf_init(DynBuf *b, size_t initial_cap) {
    b->data = (char *)malloc(initial_cap);
    b->len = 0;
    b->cap = b->data ? initial_cap : 0;
    return b->data ? 0 : -1;
}

static int dynbuf_ensure(DynBuf *b, size_t extra) {
    if (b->len + extra <= b->cap) return 0;
    size_t new_cap = b->cap == 0 ? 256 : b->cap * 2;
    if (new_cap < b->len + extra) new_cap = b->len + extra;
    char *nd = (char *)realloc(b->data, new_cap);
    if (!nd) return -1;
    b->data = nd;
    b->cap = new_cap;
    return 0;
}

static int dynbuf_append(DynBuf *b, const char *data, size_t len) {
    if (dynbuf_ensure(b, len) != 0) return -1;
    memcpy(b->data + b->len, data, len);
    b->len += len;
    return 0;
}

static void dynbuf_free(DynBuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static int ci_equal(const char *a, size_t a_len, const char *b) {
    size_t b_len = strlen(b);
    if (a_len != b_len) return 0;
    for (size_t i = 0; i < a_len; ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
    }
    return 1;
}

/* Returns 1 if `headers` (the raw header block, no status line) contains a
   "Transfer-Encoding: chunked" line. */
static int headers_say_chunked(const char *headers, size_t len) {
    const char *p = headers;
    const char *end = headers + len;
    while (p < end) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        const char *line_end = nl ? nl : end;
        const char *colon = (const char *)memchr(p, ':', (size_t)(line_end - p));
        if (colon) {
            size_t name_len = (size_t)(colon - p);
            if (name_len == 17 &&
                ci_equal(p, name_len, "Transfer-Encoding")) {
                const char *v = colon + 1;
                while (v < line_end && (*v == ' ' || *v == '\t')) v++;
                size_t v_len = (size_t)(line_end - v);
                while (v_len > 0 && (v[v_len - 1] == '\r')) v_len--;
                if (ci_equal(v, v_len, "chunked")) return 1;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    return 0;
}

/* Portable memmem: not available in MSVC's CRT. */
static const char *find_bytes(const char *haystack, size_t hay_len, const char *needle,
                               size_t needle_len) {
    if (needle_len == 0 || hay_len < needle_len) return NULL;
    for (size_t i = 0; i <= hay_len - needle_len; ++i) {
        if (memcmp(haystack + i, needle, needle_len) == 0) return haystack + i;
    }
    return NULL;
}

static int decode_chunked(const char *in, size_t in_len, DynBuf *out) {
    size_t pos = 0;
    for (;;) {
        const char *line_end = (const char *)memchr(in + pos, '\n', in_len - pos);
        if (!line_end) return -1;
        size_t size_len = (size_t)(line_end - (in + pos));
        if (size_len > 0 && in[pos + size_len - 1] == '\r') size_len--;

        char size_str[32];
        if (size_len >= sizeof(size_str)) return -1;
        memcpy(size_str, in + pos, size_len);
        size_str[size_len] = '\0';

        size_t chunk_size = (size_t)strtoul(size_str, NULL, 16);
        pos = (size_t)(line_end - in) + 1;

        if (chunk_size == 0) return 0; /* final chunk */
        if (pos + chunk_size > in_len) return -1;

        if (dynbuf_append(out, in + pos, chunk_size) != 0) return -1;
        pos += chunk_size;
        if (pos < in_len && in[pos] == '\r') pos++;
        if (pos < in_len && in[pos] == '\n') pos++;
    }
}

int fb_http_request(int port, const char *method, const char *path,
                     const char *content_type, const char *body, size_t body_len,
                     int timeout_ms, FbHttpResponse *out_resp) {
    memset(out_resp, 0, sizeof(*out_resp));

    FbSocket *sock = fb_sock_connect_loopback(port);
    if (!sock) return -1;

    char header_buf[512];
    int header_n = snprintf(header_buf, sizeof(header_buf),
                             "%s %s HTTP/1.1\r\n"
                             "Host: 127.0.0.1\r\n"
                             "Connection: close\r\n"
                             "%s%s%s"
                             "Content-Length: %zu\r\n"
                             "\r\n",
                             method, path,
                             content_type ? "Content-Type: " : "",
                             content_type ? content_type : "",
                             content_type ? "\r\n" : "",
                             body ? body_len : 0);
    if (header_n < 0 || (size_t)header_n >= sizeof(header_buf)) {
        fb_sock_close(sock);
        return -1;
    }

    if (fb_sock_send(sock, header_buf, (size_t)header_n) < 0) {
        fb_sock_close(sock);
        return -1;
    }
    if (body && body_len > 0) {
        if (fb_sock_send(sock, body, body_len) < 0) {
            fb_sock_close(sock);
            return -1;
        }
    }

    DynBuf raw;
    if (dynbuf_init(&raw, 4096) != 0) {
        fb_sock_close(sock);
        return -1;
    }

    char chunk[4096];
    for (;;) {
        int n = fb_sock_recv(sock, chunk, sizeof(chunk), timeout_ms);
        if (n <= 0) break; /* orderly close, error, or timeout: response is complete or unusable either way */
        if (dynbuf_append(&raw, chunk, (size_t)n) != 0) {
            dynbuf_free(&raw);
            fb_sock_close(sock);
            return -1;
        }
    }
    fb_sock_close(sock);

    if (raw.len == 0) {
        dynbuf_free(&raw);
        return -1;
    }

    /* Status line, e.g. "HTTP/1.1 200 OK\r\n" */
    if (raw.len < 12 || memcmp(raw.data, "HTTP/1.", 7) != 0) {
        dynbuf_free(&raw);
        return -1;
    }
    int status_code = 0;
    const char *sp = (const char *)memchr(raw.data, ' ', raw.len);
    if (sp) status_code = atoi(sp + 1);

    const char *header_end = find_bytes(raw.data, raw.len, "\r\n\r\n", 4);
    if (!header_end) {
        dynbuf_free(&raw);
        return -1;
    }
    const char *body_start = header_end + 4;
    size_t raw_body_len = raw.len - (size_t)(body_start - raw.data);

    const char *first_line_end = (const char *)memchr(raw.data, '\n', raw.len);
    const char *headers_start = first_line_end ? first_line_end + 1 : raw.data;
    size_t headers_len = (size_t)(header_end - headers_start);

    DynBuf final_body;
    if (dynbuf_init(&final_body, raw_body_len > 0 ? raw_body_len : 1) != 0) {
        dynbuf_free(&raw);
        return -1;
    }

    if (headers_say_chunked(headers_start, headers_len)) {
        if (decode_chunked(body_start, raw_body_len, &final_body) != 0) {
            dynbuf_free(&final_body);
            dynbuf_free(&raw);
            return -1;
        }
    } else {
        if (dynbuf_append(&final_body, body_start, raw_body_len) != 0) {
            dynbuf_free(&final_body);
            dynbuf_free(&raw);
            return -1;
        }
    }
    dynbuf_free(&raw);

    dynbuf_append(&final_body, "\0", 1); /* NUL-terminate for text/JSON consumers */
    out_resp->status_code = status_code;
    out_resp->body = final_body.data;
    out_resp->body_len = final_body.len - 1;
    return 0;
}

void fb_http_response_free(FbHttpResponse *resp) {
    free(resp->body);
    resp->body = NULL;
    resp->body_len = 0;
    resp->status_code = 0;
}
