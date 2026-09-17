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

/* Resolves the llama-server binary: the FB_LLAMA_SERVER env var if set,
   otherwise a bare binary name left for the OS's own PATH search to
   resolve at spawn time (CreateProcess and posix_spawnp both do this for a
   bare name with no path separators — no need to hand-roll a PATH scan).
   Returns 0 and fills out_path; this always succeeds (the OS spawn call is
   where an unresolvable name actually surfaces as an error). */
int fb_resolve_llama_server(char *out_path, size_t out_cap);

/* Resolves sherpa-onnx-offline (env FB_SHERPA_ONNX_EXE, else PATH search)
   and the four Orukeet model files under env FB_ORUKEET_DIR (expects the
   fixed filenames tokens.txt/encoder.int8.onnx/decoder.int8.onnx/
   joiner.int8.onnx, matching the model's own release layout). Returns 0
   only if FB_ORUKEET_DIR is set and all four files exist — unlike the
   llama-server binary, there's no PATH-equivalent fallback for a model
   directory, so this is a real "not configured yet" failure the caller
   must handle, not just an OS-spawn-time concern. */
typedef struct {
    char sherpa_exe[512];
    char tokens_path[512];
    char encoder_path[512];
    char decoder_path[512];
    char joiner_path[512];
} FbTranscribeConfig;

int fb_resolve_transcribe_config(FbTranscribeConfig *out);

#define FB_QWEN_DEFAULT_PORT 8090

/* FB_QWEN_MODEL env var: path to a small Qwen instruct GGUF for the
   resident intent brain. Returns 0 if set, -1 otherwise (not configured
   is a normal, expected state for a fresh install, not an error). */
int fb_resolve_qwen_model(char *out_path, size_t cap);

/* FB_QWEN_PORT env var, or FB_QWEN_DEFAULT_PORT if unset/invalid. Always
   succeeds -- this is a fixed, predictable address by design (see
   backend_mgr's requested_port), not something that needs a "not
   configured" state. */
int fb_resolve_qwen_port(void);

#endif /* FB_CONFIG_H */
