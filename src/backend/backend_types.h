#ifndef FB_BACKEND_TYPES_H
#define FB_BACKEND_TYPES_H

#include "platform/proc.h"

/* LLAMA_CPP is 0 so a zero-initialized/legacy FbConfigModel (no "kind"
   field in an old config.json) still means llama.cpp, not a silent
   reinterpretation as something else.
   SILIANG is SpeederX's own modified llama.cpp build -- same server CLI
   shape as upstream llama.cpp (-m/--port/--host), just a different binary,
   so backend_mgr.c's argv-building treats it identically to LLAMA_CPP and
   only the resolved exe path differs (see config.h's fb_resolve_siliang).
   That "same flags as upstream" assumption is unverified until it's
   actually run against a real Siliang binary -- flag any flag mismatch
   the moment that happens rather than assuming this comment is still
   accurate.
   VLLM and SGLANG are unrelated, real Linux/CUDA-first engines with their
   own CLI shape entirely (see backend_mgr.c's per-kind argv building and
   config.h's fb_resolve_vllm_exe/fb_resolve_sglang_python) -- and the
   platform caveat that comes with that. */
typedef enum {
    FB_BACKEND_LLAMA_CPP = 0,
    FB_BACKEND_SILIANG,
    FB_BACKEND_VLLM,
    FB_BACKEND_SGLANG
} FbBackendKind;

typedef enum {
    FB_BACKEND_STOPPED,
    FB_BACKEND_STARTING,
    FB_BACKEND_UP,
    FB_BACKEND_ERROR
} FbBackendState;

#define FB_BACKEND_NAME_MAX 128
#define FB_BACKEND_PATH_MAX 512

typedef struct {
    char name[FB_BACKEND_NAME_MAX];
    char model_path[FB_BACKEND_PATH_MAX];
    FbBackendKind kind;
    FbBackendState state;
    int port;
    FbProcess *proc;
    unsigned long long last_health_probe_ms; /* rate-limits blocking health checks */
    char log_path[FB_BACKEND_PATH_MAX]; /* combined stdout+stderr of the spawned process */
} FbBackendInstance;

#endif /* FB_BACKEND_TYPES_H */
