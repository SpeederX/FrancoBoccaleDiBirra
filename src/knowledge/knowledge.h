#ifndef FB_KNOWLEDGE_H
#define FB_KNOWLEDGE_H

#include <stddef.h>

/*
 * A small local RAG store: ingest arbitrary text files (e.g. exported
 * Granola meeting notes -- see /ingest), chunk and embed them via a local
 * embedding model, and retrieve the most relevant chunks for a query by
 * cosine similarity. Deliberately not a vector database -- at a personal
 * corpus scale (hundreds to low thousands of chunks) a flat in-memory
 * array and brute-force search is simpler, has no new dependency, and is
 * fast enough; this stays true to the project's own "own your data,
 * plain files, minimal footprint" ethos rather than pulling in Qdrant/
 * Chroma/etc. for a problem this size doesn't need them for.
 *
 * This is semantic memory (external facts/knowledge, retrieved on
 * relevance) as opposed to diary's episodic memory (your own
 * conversations, consolidated chronologically) -- a genuinely different
 * kind of memory, not a replacement for or extension of diary's pipeline.
 *
 * Persisted as one JSON object per line at <data_dir>/knowledge/index.jsonl
 * (source file, raw text, embedding vector) -- plain and greppable, same
 * philosophy as diary's plain-text pages.
 */

/* Backend-registry name for the always-resident embedding model, spawned
   once at app init alongside the Qwen intent brain (see app.c) --
   excluded from user-facing model counts the same way FB_QWEN_BRAIN_NAME
   is (see command_dispatch.c's count_non_brain_backends). */
#define FB_EMBEDDING_BRAIN_NAME "embedding-brain"

#define FB_KNOWLEDGE_CHUNK_MAX_CHARS 1500
#define FB_KNOWLEDGE_SOURCE_MAX 256
/* 2048 chunks * ~1500 chars is several MB of raw source text -- hundreds
   of meetings' worth at personal-corpus scale. 1024 dims covers the local
   embedding models this app actually targets (768-1024 typical; this
   isn't calling a hosted API with 3072-dim vectors). Together, FbKnowledge
   itself is genuinely large (~12MB) -- it must always be heap-allocated
   (FbApp already is, see main.c), never a stack-local or by-value
   parameter, or it silently blows the default thread stack. */
#define FB_KNOWLEDGE_MAX_CHUNKS 2048
#define FB_KNOWLEDGE_MAX_DIMS 1024

typedef struct {
    char source[FB_KNOWLEDGE_SOURCE_MAX]; /* which ingested file this chunk came from */
    char text[FB_KNOWLEDGE_CHUNK_MAX_CHARS];
    float embedding[FB_KNOWLEDGE_MAX_DIMS];
    int dims;
} FbKnowledgeChunk;

typedef struct {
    char root_dir[512]; /* <data_dir>/knowledge */
    int embed_port;     /* 0 if no embedding model is configured -- see config.h */

    FbKnowledgeChunk chunks[FB_KNOWLEDGE_MAX_CHUNKS];
    int chunk_count;
} FbKnowledge;

/* `k` must point at heap-allocated storage -- FbKnowledge is ~12MB (see
   FB_KNOWLEDGE_MAX_CHUNKS/FB_KNOWLEDGE_MAX_DIMS above), so a stack-local
   or by-value FbKnowledge silently overflows the default thread stack.
   Loads index.jsonl if data_dir/knowledge already exists; otherwise
   starts empty. embed_port == 0 is valid (no embedding model configured)
   -- ingest/search then just report "not available" rather than failing
   oddly, same pattern as diary's qwen_port == 0. */
void fb_knowledge_init(FbKnowledge *k, const char *data_dir, int embed_port);

/* Reads `path` (a single text/markdown file), splits it into paragraph-
   based chunks (merged up to ~FB_KNOWLEDGE_CHUNK_MAX_CHARS so chunks
   aren't tiny), embeds each in as few batched requests as
   FB_EMBED_MAX_BATCH allows, and appends them to the index (both in
   memory and persisted). Returns the number of chunks added, 0 if the
   file was empty/unreadable, -1 if no embedding model is configured. */
int fb_knowledge_ingest_file(FbKnowledge *k, const char *path);

typedef struct {
    char source[FB_KNOWLEDGE_SOURCE_MAX];
    char text[FB_KNOWLEDGE_CHUNK_MAX_CHARS];
    float score; /* cosine similarity, roughly -1..1; higher is more relevant */
} FbKnowledgeMatch;

/* Embeds `query` and returns the `max_results` highest-cosine-similarity
   chunks, descending by score. Returns the count filled (0 if the index
   is empty), or -1 if no embedding model is configured. */
int fb_knowledge_search(FbKnowledge *k, const char *query, FbKnowledgeMatch *out_matches,
                         int max_results);

#endif /* FB_KNOWLEDGE_H */
