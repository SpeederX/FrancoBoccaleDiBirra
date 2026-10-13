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

/* 1500 chars was the original guess; tested against a real meeting
   transcript it produced a ~536-token chunk against a local
   embedding-server default n_ubatch of 512, which the server flatly
   rejects ("input too large to process") -- silently dropping that
   chunk. Real speech (short words, filler like "uh, well, um") tokenizes
   less efficiently than clean prose, so this stays well under budget
   even on dense transcript text (~2-2.5 chars/token worst case still
   lands under 512 tokens at this cap). */
#define FB_KNOWLEDGE_CHUNK_MAX_CHARS 1000
/* How much of a closed chunk's tail gets carried into the start of the
   next one (~15% of the cap, the usual RAG rule of thumb). Without this,
   a sentence or exchange that happens to fall right on a chunk boundary
   is legible in neither chunk alone; with it, every chunk after the
   first also comes with a guaranteed floor of real preceding context
   (not just a lone short line), which is the same mechanism that keeps
   chunks from ending up too small to be independently retrievable. */
#define FB_KNOWLEDGE_CHUNK_OVERLAP_CHARS 150
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
    /* Links related ingests together -- e.g. a meeting's raw transcript
       and its summary are two different files but the *same* underlying
       event, not independent facts. Empty if ungrouped (backward
       compatible with chunks ingested before this existed). See
       fb_knowledge_ingest_file's group parameter. */
    char group[FB_KNOWLEDGE_SOURCE_MAX];
    /* 1 if this chunk is a distilled summary of `group`, 0 if it's raw
       source content. At most one summary chunk is expected per group (a
       summary is usually short enough to be one chunk), but this isn't
       enforced -- fb_knowledge_find_summary just returns the first
       match. */
    int is_summary;
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
   file was empty/unreadable, -1 if no embedding model is configured.
   `group` links this ingest to others about the same underlying thing
   (e.g. a meeting's transcript and its summary) -- pass NULL/"" to
   auto-derive one from the filename (stripping a trailing
   "-transcript"/"-summary"/"-notes" and extension, so
   "acme-2026-09-19-transcript.txt" and "acme-2026-09-19-summary.txt"
   land in the same group without the caller doing anything), or pass an
   explicit id to control it directly. `is_summary` marks every chunk
   from this ingest as the group's distilled summary rather than raw
   content -- see fb_knowledge_find_summary. */
int fb_knowledge_ingest_file(FbKnowledge *k, const char *path, const char *group, int is_summary);

typedef struct {
    char source[FB_KNOWLEDGE_SOURCE_MAX];
    char text[FB_KNOWLEDGE_CHUNK_MAX_CHARS];
    float score; /* cosine similarity, roughly -1..1; higher is more relevant */
    char group[FB_KNOWLEDGE_SOURCE_MAX]; /* empty if the matched chunk is ungrouped */
    int is_summary;
} FbKnowledgeMatch;

/* Embeds `query` and returns the `max_results` highest-cosine-similarity
   chunks, descending by score. Returns the count filled (0 if the index
   is empty), or -1 if no embedding model is configured. Pure similarity
   ranking -- doesn't pull in a matched chunk's group-mates; callers that
   want "also give me this group's summary" call fb_knowledge_find_summary
   themselves (see app.c's inject_knowledge_context for the pattern). */
int fb_knowledge_search(FbKnowledge *k, const char *query, FbKnowledgeMatch *out_matches,
                         int max_results);

/* Looks up the summary chunk for `group` (the first indexed chunk with a
   matching group and is_summary != 0). Returns 1 and fills *out if found,
   0 if `group` is empty, unknown, or has no summary chunk. Lets a caller
   that just matched a raw/transcript chunk also surface the meeting's
   overall summary alongside the specific detail, so a linked pair reads
   as one connected thing rather than two unrelated hits. */
int fb_knowledge_find_summary(FbKnowledge *k, const char *group, FbKnowledgeMatch *out);

#endif /* FB_KNOWLEDGE_H */
