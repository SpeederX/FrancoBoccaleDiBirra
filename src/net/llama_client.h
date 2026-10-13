#ifndef FB_LLAMA_CLIENT_H
#define FB_LLAMA_CLIENT_H

/* Returns 1 if llama-server on 127.0.0.1:port responds successfully to
   /health, 0 otherwise (including "not up yet" and "connection refused"). */
int fb_llama_health(int port, int timeout_ms);

/* Embeddings, for knowledge/knowledge.h's retrieval. Confirmed directly
   against a running llama-server (embeddinggemma-300M): launched with
   --embedding, POST /v1/embeddings with {"input": "text" | ["text",...]}
   returns {"data":[{"index":N,"embedding":[floats...]},...]} -- the
   standard OpenAI shape, one vector per input, batched in one request
   rather than one request per chunk. */
#define FB_EMBED_MAX_BATCH 32
#define FB_EMBED_MAX_DIMS 2048

typedef struct {
    float values[FB_EMBED_MAX_DIMS];
    int dims;
} FbEmbeddingVector;

/* Embeds up to FB_EMBED_MAX_BATCH texts in one request. Returns the
   number of vectors filled into out_vectors (in the same order as
   texts), or -1 on total failure (connection/parse error, or more than
   FB_EMBED_MAX_BATCH texts given). All-or-nothing -- there's no partial-
   batch failure mode to report individually. */
int fb_backend_embed_batch(int port, const char *const *texts, int text_count,
                            FbEmbeddingVector *out_vectors, int timeout_ms);

#endif /* FB_LLAMA_CLIENT_H */
