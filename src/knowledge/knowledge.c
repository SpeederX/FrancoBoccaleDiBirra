#include "knowledge/knowledge.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "net/llama_client.h"
#include "util/fileutil.h"
#include "util/paths.h"

/* Per-ingest-call working buffer, not sized to the whole index -- one
   meeting transcript realistically chunks into tens of pieces, not
   thousands; 512 * 1.5KB (~768KB) covers a genuinely long document
   without a multi-megabyte stack frame. */
#define FB_KNOWLEDGE_INGEST_MAX_CHUNKS_PER_FILE 512

static void index_path(const FbKnowledge *k, char *out, size_t out_cap) {
    snprintf(out, out_cap, "%s/index.jsonl", k->root_dir);
}

void fb_knowledge_init(FbKnowledge *k, const char *data_dir, int embed_port) {
    memset(k, 0, sizeof(*k));
    k->embed_port = embed_port;
    snprintf(k->root_dir, sizeof(k->root_dir), "%s/knowledge", data_dir);
    fb_ensure_dir(k->root_dir);

    char path[600];
    index_path(k, path, sizeof(path));
    size_t len = 0;
    char *text = fb_read_file(path, &len);
    if (!text) return;

    char *line = text;
    while (line && *line && k->chunk_count < FB_KNOWLEDGE_MAX_CHUNKS) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        if (line[0] != '\0') {
            cJSON *obj = cJSON_Parse(line);
            if (obj) {
                cJSON *source = cJSON_GetObjectItemCaseSensitive(obj, "source");
                cJSON *chunk_text = cJSON_GetObjectItemCaseSensitive(obj, "text");
                cJSON *embedding = cJSON_GetObjectItemCaseSensitive(obj, "embedding");
                if (cJSON_IsString(source) && cJSON_IsString(chunk_text) && cJSON_IsArray(embedding)) {
                    FbKnowledgeChunk *c = &k->chunks[k->chunk_count];
                    memset(c, 0, sizeof(*c));
                    snprintf(c->source, sizeof(c->source), "%s", source->valuestring);
                    snprintf(c->text, sizeof(c->text), "%s", chunk_text->valuestring);
                    cJSON *f;
                    cJSON_ArrayForEach(f, embedding) {
                        if (c->dims >= FB_KNOWLEDGE_MAX_DIMS) break;
                        if (cJSON_IsNumber(f)) c->embedding[c->dims++] = (float)f->valuedouble;
                    }
                    k->chunk_count++;
                }
                cJSON_Delete(obj);
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    free(text);
}

static void append_chunk_to_disk(const FbKnowledge *k, const FbKnowledgeChunk *c) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "source", c->source);
    cJSON_AddStringToObject(obj, "text", c->text);
    cJSON *embedding = cJSON_AddArrayToObject(obj, "embedding");
    for (int i = 0; i < c->dims; ++i) {
        cJSON_AddItemToArray(embedding, cJSON_CreateNumber(c->embedding[i]));
    }
    char *line = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!line) return;

    char path[600];
    index_path(k, path, sizeof(path));
    FILE *f = fopen(path, "ab");
    if (f) {
        fputs(line, f);
        fputc('\n', f);
        fclose(f);
    }
    cJSON_free(line);
}

/* Paragraph-based chunking: splits on blank lines, then greedily merges
   consecutive paragraphs up to FB_KNOWLEDGE_CHUNK_MAX_CHARS so chunks
   stay reasonably dense rather than one-paragraph-each (a lone "Attendees:
   Alice, Bob" line as its own chunk carries little retrievable meaning on
   its own). A single paragraph longer than the cap is hard-truncated --
   good enough for meeting notes/summaries, which are rarely one giant
   unbroken paragraph. */
static int chunk_text(const char *text, char out_chunks[][FB_KNOWLEDGE_CHUNK_MAX_CHARS],
                       int max_chunks) {
    int count = 0;
    size_t off = 0;
    size_t cur_len = 0;
    out_chunks[0][0] = '\0';

    while (text[off] != '\0' && count < max_chunks) {
        size_t para_start = off;
        while (text[off] != '\0' && !(text[off] == '\n' && text[off + 1] == '\n')) off++;
        size_t para_len = off - para_start;
        while (text[off] == '\n') off++; /* skip the blank-line separator */

        while (para_len > 0 &&
               (text[para_start + para_len - 1] == '\n' || text[para_start + para_len - 1] == '\r' ||
                text[para_start + para_len - 1] == ' ')) {
            para_len--;
        }
        if (para_len == 0) continue;

        if (cur_len > 0 && cur_len + 2 + para_len > FB_KNOWLEDGE_CHUNK_MAX_CHARS - 1) {
            count++;
            if (count >= max_chunks) break;
            out_chunks[count][0] = '\0';
            cur_len = 0;
        }

        char *dst = out_chunks[count];
        if (cur_len > 0) {
            dst[cur_len++] = '\n';
            dst[cur_len++] = '\n';
        }
        size_t room = FB_KNOWLEDGE_CHUNK_MAX_CHARS - 1 - cur_len;
        size_t copy_len = para_len > room ? room : para_len;
        memcpy(dst + cur_len, text + para_start, copy_len);
        cur_len += copy_len;
        dst[cur_len] = '\0';
    }
    if (cur_len > 0) count++;
    return count;
}

int fb_knowledge_ingest_file(FbKnowledge *k, const char *path) {
    if (k->embed_port == 0) return -1;

    size_t len = 0;
    char *text = fb_read_file(path, &len);
    if (!text || len == 0) {
        free(text);
        return 0;
    }

    /* A per-call working buffer, not sized to the whole index -- see
       FB_KNOWLEDGE_INGEST_MAX_CHUNKS_PER_FILE's own comment. static, not
       a local array, so it doesn't blow the stack either way. */
    static char chunks[FB_KNOWLEDGE_INGEST_MAX_CHUNKS_PER_FILE][FB_KNOWLEDGE_CHUNK_MAX_CHARS];
    int room = FB_KNOWLEDGE_MAX_CHUNKS - k->chunk_count;
    int cap = room < FB_KNOWLEDGE_INGEST_MAX_CHUNKS_PER_FILE ? room
                                                              : FB_KNOWLEDGE_INGEST_MAX_CHUNKS_PER_FILE;
    int chunk_count = cap > 0 ? chunk_text(text, chunks, cap) : 0;
    free(text);
    if (chunk_count == 0) return 0;

    const char *source = path;
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *sep = (slash && bslash) ? (slash > bslash ? slash : bslash) : (slash ? slash : bslash);
    if (sep) source = sep + 1;

    int added = 0;
    for (int base = 0; base < chunk_count; base += FB_EMBED_MAX_BATCH) {
        int batch_n = chunk_count - base;
        if (batch_n > FB_EMBED_MAX_BATCH) batch_n = FB_EMBED_MAX_BATCH;

        const char *texts[FB_EMBED_MAX_BATCH];
        for (int i = 0; i < batch_n; ++i) texts[i] = chunks[base + i];

        FbEmbeddingVector vectors[FB_EMBED_MAX_BATCH];
        int filled = fb_backend_embed_batch(k->embed_port, texts, batch_n, vectors, 30000);
        if (filled <= 0) continue; /* this batch failed; still try the rest */

        for (int i = 0; i < batch_n && k->chunk_count < FB_KNOWLEDGE_MAX_CHUNKS; ++i) {
            FbKnowledgeChunk *c = &k->chunks[k->chunk_count];
            memset(c, 0, sizeof(*c));
            snprintf(c->source, sizeof(c->source), "%s", source);
            snprintf(c->text, sizeof(c->text), "%s", chunks[base + i]);
            c->dims = vectors[i].dims;
            memcpy(c->embedding, vectors[i].values, sizeof(float) * (size_t)vectors[i].dims);

            append_chunk_to_disk(k, c);
            k->chunk_count++;
            added++;
        }
    }
    return added;
}

static float cosine_similarity(const float *a, int a_dims, const float *b, int b_dims) {
    if (a_dims != b_dims || a_dims == 0) return -1.0f;
    double dot = 0.0, norm_a = 0.0, norm_b = 0.0;
    for (int i = 0; i < a_dims; ++i) {
        dot += (double)a[i] * (double)b[i];
        norm_a += (double)a[i] * (double)a[i];
        norm_b += (double)b[i] * (double)b[i];
    }
    if (norm_a <= 0.0 || norm_b <= 0.0) return -1.0f;
    return (float)(dot / (sqrt(norm_a) * sqrt(norm_b)));
}

int fb_knowledge_search(FbKnowledge *k, const char *query, FbKnowledgeMatch *out_matches,
                         int max_results) {
    if (k->embed_port == 0) return -1;
    if (k->chunk_count == 0 || max_results <= 0) return 0;

    const char *texts[1] = {query};
    FbEmbeddingVector qvec;
    if (fb_backend_embed_batch(k->embed_port, texts, 1, &qvec, 30000) <= 0) return -1;

    /* Brute-force: score every chunk, keep a running top-N via insertion
       into a small sorted array -- fine at this scale (thousands of
       chunks, tens of results), no need for a heap. */
    int found = 0;
    for (int i = 0; i < k->chunk_count; ++i) {
        float score = cosine_similarity(qvec.values, qvec.dims, k->chunks[i].embedding,
                                         k->chunks[i].dims);

        int insert_at = found < max_results ? found : -1;
        for (int j = 0; j < found && j < max_results; ++j) {
            if (score > out_matches[j].score) {
                insert_at = j;
                break;
            }
        }
        if (insert_at < 0) continue;

        int shift_end = found < max_results ? found : max_results - 1;
        for (int j = shift_end; j > insert_at; --j) out_matches[j] = out_matches[j - 1];

        snprintf(out_matches[insert_at].source, sizeof(out_matches[insert_at].source), "%s",
                 k->chunks[i].source);
        snprintf(out_matches[insert_at].text, sizeof(out_matches[insert_at].text), "%s",
                 k->chunks[i].text);
        out_matches[insert_at].score = score;
        if (found < max_results) found++;
    }
    return found;
}
