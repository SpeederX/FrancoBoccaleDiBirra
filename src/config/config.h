#ifndef FB_CONFIG_H
#define FB_CONFIG_H

#include <stddef.h>

#include "backend/backend_types.h"

#define FB_CONFIG_MAX_MODELS 64

typedef struct {
    char name[128];
    char path[512];
    /* Which engine this model targets. A model registered by an older
       config.json with no "kind" field reads back as 0 (FB_BACKEND_LLAMA_CPP)
       -- the correct legacy behavior, not a guess: everything registered
       before this field existed genuinely was a llama.cpp .gguf. */
    FbBackendKind kind;
} FbConfigModel;

typedef struct {
    FbConfigModel models[FB_CONFIG_MAX_MODELS];
    int model_count;
    char last_browse_dir[512]; /* where the /add file picker last left off */
    char lang_pref[8]; /* user preference only -- Orukeet auto-detects spoken language */

    /* Persistent, user-editable equivalents of the FB_* env vars below --
       hand-edit <data_dir>/config.json once instead of setting an env var
       every session. Empty means "not set here"; the matching env var, if
       set, always wins over these (see each fb_resolve_* function). */
    char llama_server_path[512];
    char siliang_exe[512];   /* SpeederX's llama.cpp fork -- same CLI shape as llama-server */
    char vllm_exe[512];      /* vLLM's own `vllm serve` CLI, Linux/CUDA-first */
    char sglang_python[512]; /* python interpreter to run `-m sglang.launch_server` with */
    char sherpa_exe[512];
    char orukeet_dir[512];
    char qwen_model[512];
    int qwen_port; /* 0 = not set here */
    char calibr_root[512];

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

/* Returns 1 and copies the path if `name` is registered, else 0. out_kind
   may be NULL if the caller only needs the path (e.g. an existence check
   before dispatching /load, which re-resolves everything itself). */
int fb_config_find_model(const FbConfig *cfg, const char *name, char *out_path, size_t out_cap,
                          FbBackendKind *out_kind);

/* Adds a new entry, or updates the path/kind if `name` already exists.
   Returns 0 on success, -1 if the registry is full. */
int fb_config_add_model(FbConfig *cfg, const char *name, const char *path, FbBackendKind kind);

/* Returns 0 on success, -1 if `name` wasn't registered. */
int fb_config_remove_model(FbConfig *cfg, const char *name);

/* Resolves the llama-server binary. Priority: FB_LLAMA_SERVER env var,
   then cfg->llama_server_path (config.json), then a bare binary name left
   for the OS's own PATH search to resolve at spawn time (CreateProcess and
   posix_spawnp both do this for a bare name with no path separators — no
   need to hand-roll a PATH scan). Returns 0 and fills out_path; this
   always succeeds (the OS spawn call is where an unresolvable name
   actually surfaces as an error). */
int fb_resolve_llama_server(const FbConfig *cfg, char *out_path, size_t out_cap);

/* Same env-var -> config.json -> bare-PATH-name resolution pattern as
   fb_resolve_llama_server, for SpeederX's Siliang fork (FB_SILIANG_EXE /
   cfg->siliang_exe / FB_SILIANG_DEFAULT_NAME) -- the default name is a
   guess (backend_types.h flags this), correct it once the real binary
   name is known. */
int fb_resolve_siliang_exe(const FbConfig *cfg, char *out_path, size_t out_cap);

/* Same pattern, for vLLM's own CLI (FB_VLLM_EXE / cfg->vllm_exe /
   FB_VLLM_DEFAULT_NAME -- "vllm", the `vllm serve` entry point). vLLM
   itself is Linux/CUDA-first; resolving a path here doesn't mean it'll
   actually run on this machine. */
int fb_resolve_vllm_exe(const FbConfig *cfg, char *out_path, size_t out_cap);

/* Same pattern, for the python interpreter SGLang's server runs under
   (FB_SGLANG_PYTHON / cfg->sglang_python / FB_SGLANG_PYTHON_DEFAULT_NAME)
   -- backend_mgr.c invokes it as `<this> -m sglang.launch_server ...`,
   there's no standalone sglang binary. Same Linux/CUDA-first caveat as
   vLLM. */
int fb_resolve_sglang_python(const FbConfig *cfg, char *out_path, size_t out_cap);

/* Resolves sherpa-onnx-offline and the four Orukeet model files. Priority
   for each: its FB_* env var, then the matching config.json field, then an
   auto-detected match under <data_dir>/external/ (searched recursively,
   bounded depth) -- this is where a first-run setup drops the sherpa-onnx
   SDK and Orukeet release archives, so a fresh checkout with nothing
   configured at all still finds them. Orukeet expects the fixed filenames
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

/* Path to a small Qwen instruct GGUF for the resident intent brain.
   Priority: FB_QWEN_MODEL env var, then cfg->qwen_model. Deliberately has
   no auto-detected fallback: a models folder can hold GGUFs ranging from
   under 1B to tens of billions of parameters, and silently picking one for
   an always-resident "lightweight" brain would be exactly the wrong kind
   of surprising. Returns 0 if set to an existing file, -1 otherwise (not
   configured is a normal, expected state for a fresh install, not an
   error). */
int fb_resolve_qwen_model(const FbConfig *cfg, char *out_path, size_t cap);

/* FB_QWEN_PORT env var, then cfg->qwen_port, then FB_QWEN_DEFAULT_PORT.
   Always succeeds -- this is a fixed, predictable address by design (see
   backend_mgr's requested_port), not something that needs a "not
   configured" state. */
int fb_resolve_qwen_port(const FbConfig *cfg);

#endif /* FB_CONFIG_H */
