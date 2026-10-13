#ifndef FB_CONFIG_H
#define FB_CONFIG_H

#include <stddef.h>

#define FB_CONFIG_MAX_MODELS 64

typedef struct {
    char name[128];
    char path[512];
} FbConfigModel;

typedef struct {
    FbConfigModel models[FB_CONFIG_MAX_MODELS];
    int model_count;
    char last_browse_dir[512]; /* where the /add file picker last left off */
    char lang_pref[8]; /* user preference only -- Orukeet auto-detects spoken language */

    /* Hand-edit <data_dir>/config.json to set these -- the single source
       of truth for every path/port below (see each fb_resolve_* function).
       Empty means "not set here". */
    char llama_server_path[512];
    char sherpa_exe[512];
    char orukeet_dir[512];
    char qwen_model[512];
    int qwen_port; /* 0 = not set here */
    char calibr_root[512];
    char meeting_parakeet_exe[512];
    char meeting_asr_model[512];
    char meeting_diar_model[512];
    char meeting_ffmpeg_exe[512];
    char meeting_llm_model[128];
    char meeting_folder[512];
    char meeting_parakeet_library[512];
    int meeting_capture_source;
    char meeting_microphone[256];
    char meeting_output_device[256];
    char meeting_tts_python[512];
    char meeting_tts_script[512];
    char meeting_tts_model[512];
    int meeting_llm_port;
    char embedding_model[512]; /* GGUF for knowledge/knowledge.h's local RAG store */
    int embedding_port;        /* 0 = not set here */

    /* Diary mode's page/memory thresholds -- 0 means "use the built-in
       default" (see diary/diary.h). Each is clamped to that module's hard
       buffer cap regardless of what's set here, so a bad value can't
       overflow a fixed-size buffer. */
    int diary_page_close_chars;
    int diary_checkpoint_chars;
    int diary_idle_close_minutes;
} FbConfig;

/* Loads <data_dir>/config.json. A missing file is normal (an empty,
   zeroed *cfg) and returns 0; only a present-but-corrupt file returns -1. */
int fb_config_load(FbConfig *cfg, const char *data_dir);

int fb_config_save(const FbConfig *cfg, const char *data_dir);

/* Returns 1 and copies the path if `name` is registered, else 0. */
int fb_config_find_model(const FbConfig *cfg, const char *name, char *out_path, size_t out_cap);

/* Adds a new entry, or updates the path if `name` already exists. Returns
   0 on success, -1 if the registry is full. */
int fb_config_add_model(FbConfig *cfg, const char *name, const char *path);

/* Returns 0 on success, -1 if `name` wasn't registered. */
int fb_config_remove_model(FbConfig *cfg, const char *name);

/* Resolves the llama-server binary. Priority: cfg->llama_server_path
   (config.json), then a bare binary name left for the OS's own PATH search
   to resolve at spawn time (CreateProcess and posix_spawnp both do this
   for a bare name with no path separators — no need to hand-roll a PATH
   scan). Returns 0 and fills out_path; this always succeeds (the OS spawn
   call is where an unresolvable name actually surfaces as an error). */
int fb_resolve_llama_server(const FbConfig *cfg, char *out_path, size_t out_cap);

/* Resolves sherpa-onnx-offline and the four Orukeet model files. Priority
   for each: the matching config.json field, then an auto-detected match
   under <data_dir>/external/ (searched recursively, bounded depth) -- this
   is where a first-run setup drops the sherpa-onnx SDK and Orukeet release
   archives, so a fresh checkout with nothing configured at all still finds
   them. Orukeet expects the fixed filenames
   tokens.txt/encoder.int8.onnx/decoder.int8.onnx/joiner.int8.onnx
   (matching the model's own release layout) all present in the same
   directory. Returns 0 only if the exe is found/resolvable and all four
   Orukeet files exist somewhere -- unlike the llama-server binary, there's
   no PATH-equivalent fallback for a model directory, so this is a real
   "not configured yet" failure the caller must handle, not just an
   OS-spawn-time concern. */
typedef struct {
    char sherpa_exe[512];
    char tokens_path[512];
    char encoder_path[512];
    char decoder_path[512];
    char joiner_path[512];
} FbTranscribeConfig;

int fb_resolve_transcribe_config(const FbConfig *cfg, const char *data_dir, FbTranscribeConfig *out);

#define FB_QWEN_DEFAULT_PORT 8090

/* Path to a small Qwen instruct GGUF for the resident intent brain, from
   cfg->qwen_model (config.json). Deliberately has no auto-detected
   fallback: a models folder can hold GGUFs ranging from under 1B to tens
   of billions of parameters, and silently picking one for an
   always-resident "lightweight" brain would be exactly the wrong kind of
   surprising. Returns 0 if set to an existing file, -1 otherwise (not
   configured is a normal, expected state for a fresh install, not an
   error). */
int fb_resolve_qwen_model(const FbConfig *cfg, char *out_path, size_t cap);

/* cfg->qwen_port if set, else FB_QWEN_DEFAULT_PORT. Always succeeds --
   this is a fixed, predictable address by design (see backend_mgr's
   requested_port), not something that needs a "not configured" state. */
int fb_resolve_qwen_port(const FbConfig *cfg);

#define FB_EMBEDDING_DEFAULT_PORT 8091

/* Same pattern and same reasoning as fb_resolve_qwen_model -- a GGUF for
   the local RAG store's embedding backend (knowledge/knowledge.h), no
   auto-detected fallback since guessing an embedding model out of a
   models folder would be exactly as wrong as guessing a chat one.
   Uses cfg->embedding_model from config.json.
   Returns 0 if set to an existing file, -1 otherwise (not configured is
   normal -- the whole knowledge feature is then just unavailable). */
int fb_resolve_embedding_model(const FbConfig *cfg, char *out_path, size_t cap);

/* cfg->embedding_port if set, else
   FB_EMBEDDING_DEFAULT_PORT. Always succeeds, same reasoning as
   fb_resolve_qwen_port. */
int fb_resolve_embedding_port(const FbConfig *cfg);

#endif /* FB_CONFIG_H */
