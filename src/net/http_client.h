#ifndef FB_HTTP_CLIENT_H
#define FB_HTTP_CLIENT_H

#include <stddef.h>

typedef struct {
    int status_code; /* 0 if the response couldn't be parsed at all */
    char *body;       /* malloc'd, NUL-terminated; NULL if empty */
    size_t body_len;
} FbHttpResponse;

/*
 * Blocking HTTP/1.1 request to 127.0.0.1:port, always sent with
 * "Connection: close" so the server closing the socket marks the end of
 * the response — this lets the client simply read until EOF instead of
 * tracking Content-Length itself, and still transparently decodes
 * Transfer-Encoding: chunked (llama-server's cpp-httplib base can reply
 * chunked even for small JSON bodies).
 *
 * method is "GET" or "POST". body/body_len/content_type may be NULL/0 for
 * a request with no body. Returns 0 on success (out_resp filled; caller
 * must fb_http_response_free it), -1 on connect/timeout/parse failure.
 */
int fb_http_request(int port, const char *method, const char *path,
                     const char *content_type, const char *body, size_t body_len,
                     int timeout_ms, FbHttpResponse *out_resp);

void fb_http_response_free(FbHttpResponse *resp);

#endif /* FB_HTTP_CLIENT_H */
