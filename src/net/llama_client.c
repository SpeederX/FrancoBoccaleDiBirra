#include "net/llama_client.h"

#include "net/http_client.h"

int fb_llama_health(int port, int timeout_ms) {
    FbHttpResponse resp;
    if (fb_http_request(port, "GET", "/health", NULL, NULL, 0, timeout_ms, &resp) != 0) {
        return 0;
    }
    int ok = (resp.status_code == 200);
    fb_http_response_free(&resp);
    return ok;
}
