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
                    /* group/is_summary are optional fields -- absent on
                       chunks written before this existed, which just
                       load back as ungrouped (memset above already
                       zeroed them). */
                    cJSON *group = cJSON_GetObjectItemCaseSensitive(obj, "group");
                    if (cJSON_IsString(group)) snprintf(c->group, sizeof(c->group), "%s", group->valuestring);
                    cJSON *is_summary = cJSON_GetObjectItemCaseSensitive(obj, "is_summary");
                    if (cJSON_IsBool(is_summary)) c->is_summary = cJSON_IsTrue(is_summary) ? 1 : 0;
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
    cJSON_AddStringToObject(obj, "group", c->group);
    cJSON_AddBoolToObject(obj, "is_summary", c->is_summary);
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

/* Line-based chunking: splits on every newline (not just a blank-line
   paragraph break), then greedily merges consecutive non-empty lines up
   to FB_KNOWLEDGE_CHUNK_MAX_CHARS so chunks stay dense rather than
   one-line-each. A blank line still ends up acting like a paragraph
   break (it contributes no content, so the next line starts fresh in
   the current chunk unless the cap is already hit) -- but unlike an
   earlier, blank-line-only version of this function, a real meeting
   transcript (one speaker turn per line, CRLF-terminated, no blank
   lines anywhere in the body -- confirmed against real Granola exports)
   now actually gets split into multiple chunks instead of being treated
   as one giant "paragraph" and silently truncated to the first ~1500
   characters. A single line longer than the cap is hard-truncated. */
static int chunk_text(const char *text, char out_chunks[][FB_KNOWLEDGE_CHUNK_MAX_CHARS],
                       int max_chunks) {
    int count = 0;
    size_t off = 0;
    size_t cur_len = 0;
    out_chunks[0][0] = '\0';

    while (text[off] != '\0' && count < max_chunks) {
        size_t line_start = off;
        while (text[off] != '\0' && text[off] != '\n') off++;
        size_t line_len = off - line_start;
        if (text[off] == '\n') off++;

        while (line_len > 0 && (text[line_start + line_len - 1] == '\r' ||
                                 text[line_start + line_len - 1] == ' ')) {
            line_len--;
        }
        if (line_len == 0) continue;

        if (cur_len > 0 && cur_len + 1 + line_len > FB_KNOWLEDGE_CHUNK_MAX_CHARS - 1) {
            count++;
            if (count >= max_chunks) break;
            out_chunks[count][0] = '\0';
            cur_len = 0;
        }

        char *dst = out_chunks[count];
        if (cur_len > 0) dst[cur_len++] = '\n';
        size_t room = FB_KNOWLEDGE_CHUNK_MAX_CHARS - 1 - cur_len;
        size_t copy_len = line_len > room ? room : line_len;
        memcpy(dst + cur_len, text + line_start, copy_len);
        cur_len += copy_len;
        dst[cur_len] = '\0';
    }
    if (cur_len > 0) count++;
    return count;
}

/* Derives a default group id from a bare filename (no directory) by
   stripping the extension and one trailing "-transcript"/"-summary"/
   "-notes" (or "_"-separated) tag, so "acme-2026-09-19-transcript.txt"
   and "acme-2026-09-19-summary.txt" land in the same group with zero
   caller effort. Falls back to the whole filename (minus extension) if
   no known tag is present -- still a stable, reproducible group of one
   until a matching counterpart is ingested. */
static void derive_group_from_filename(const char *filename, char *out, size_t out_cap) {
    char base[FB_KNOWLEDGE_SOURCE_MAX];
    snprintf(base, sizeof(base), "%s", filename);

    char *dot = strrchr(base, '.');
    if (dot && dot != base) *dot = '\0';

    static const char *tags[] = {"-transcript", "_transcript", "-summary", "_summary",
                                  "-notes",      "_notes"};
    size_t base_len = strlen(base);
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); ++i) {
        size_t tag_len = strlen(tags[i]);
        if (base_len > tag_len && strcmp(base + base_len - tag_len, tags[i]) == 0) {
            base[base_len - tag_len] = '\0';
            break;
        }
    }
    snprintf(out, out_cap, "%s", base);
}

int fb_knowledge_ingest_file(FbKnowledge *k, const char *path, const char *group, int is_summary) {
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

    char resolved_group[FB_KNOWLEDGE_SOURCE_MAX];
    if (group && group[0] != '\0') {
        snprintf(resolved_group, sizeof(resolved_group), "%s", group);
    } else {
        derive_group_from_filename(source, resolved_group, sizeof(resolved_group));
    }

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
            snprintf(c->group, sizeof(c->group), "%s", resolved_group);
            c->is_summary = is_summary ? 1 : 0;
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
        snprintf(out_matches[insert_at].group, sizeof(out_matches[insert_at].group), "%s",
                 k->chunks[i].group);
        out_matches[insert_at].is_summary = k->chunks[i].is_summary;
        out_matches[insert_at].score = score;
        if (found < max_results) found++;
    }
    return found;
}

int fb_knowledge_find_summary(FbKnowledge *k, const char *group, FbKnowledgeMatch *out) {
    if (!group || group[0] == '\0') return 0;
    for (int i = 0; i < k->chunk_count; ++i) {
        FbKnowledgeChunk *c = &k->chunks[i];
        if (!c->is_summary || strcmp(c->group, group) != 0) continue;
        snprintf(out->source, sizeof(out->source), "%s", c->source);
        snprintf(out->text, sizeof(out->text), "%s", c->text);
        snprintf(out->group, sizeof(out->group), "%s", c->group);
        out->is_summary = 1;
        out->score = 1.0f; /* not a similarity result -- pulled in by group, not scored */
        return 1;
    }
    return 0;
}
