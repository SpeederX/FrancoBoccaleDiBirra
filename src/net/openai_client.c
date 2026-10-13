#include "net/openai_client.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "net/http_client.h"

int fb_backend_health(int port, int timeout_ms) {
    FbHttpResponse resp;
    if (fb_http_request(port, "GET", "/health", NULL, NULL, 0, timeout_ms, &resp) != 0) {
        return 0;
    }
    int ok = (resp.status_code == 200);
    fb_http_response_free(&resp);
    return ok;
}

int fb_backend_embed_batch(int port, const char *const *texts, int text_count,
                            FbEmbeddingVector *out_vectors, int timeout_ms) {
    if (text_count <= 0 || text_count > FB_EMBED_MAX_BATCH) return -1;

    cJSON *root = cJSON_CreateObject();
    cJSON *input = cJSON_AddArrayToObject(root, "input");
    for (int i = 0; i < text_count; ++i) {
        cJSON_AddItemToArray(input, cJSON_CreateString(texts[i]));
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return -1;

    FbHttpResponse resp;
    int rc = fb_http_request(port, "POST", "/v1/embeddings", "application/json", body, strlen(body),
                              timeout_ms, &resp);
    cJSON_free(body);
    if (rc != 0) return -1;

    cJSON *outer = cJSON_Parse(resp.body);
    fb_http_response_free(&resp);
    if (!outer) return -1;

    int filled = 0;
    cJSON *data = cJSON_GetObjectItemCaseSensitive(outer, "data");
    if (cJSON_IsArray(data)) {
        cJSON *item;
        cJSON_ArrayForEach(item, data) {
            if (filled >= text_count) break;
            cJSON *idx_j = cJSON_GetObjectItemCaseSensitive(item, "index");
            int idx = cJSON_IsNumber(idx_j) ? (int)idx_j->valuedouble : filled;
            if (idx < 0 || idx >= text_count) continue;

            cJSON *emb = cJSON_GetObjectItemCaseSensitive(item, "embedding");
            if (!cJSON_IsArray(emb)) continue;

            FbEmbeddingVector *v = &out_vectors[idx];
            v->dims = 0;
            cJSON *f;
            cJSON_ArrayForEach(f, emb) {
                if (v->dims >= FB_EMBED_MAX_DIMS) break;
                if (cJSON_IsNumber(f)) v->values[v->dims++] = (float)f->valuedouble;
            }
            filled++;
        }
    }
    cJSON_Delete(outer);
    return filled > 0 ? filled : -1;
}
