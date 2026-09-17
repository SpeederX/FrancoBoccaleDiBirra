#ifndef FB_BACKEND_MGR_H
#define FB_BACKEND_MGR_H

#include "backend/backend_types.h"

#define FB_BACKEND_MGR_MAX 16

typedef struct {
    FbBackendInstance items[FB_BACKEND_MGR_MAX];
    int count;
} FbBackendMgr;

void fb_backend_mgr_init(FbBackendMgr *mgr);

/* Spawns llama-server for model_path, in FB_BACKEND_STARTING state.
   requested_port == 0 means "allocate a fresh OS-assigned ephemeral port"
   (the normal case for user-loaded models); a nonzero value pins it to
   that exact port instead (used for the resident Qwen intent brain, which
   needs a predictable address). extra_args, if non-NULL, is a NULL-
   terminated array of additional flags appended after the standard
   -m/--port/--host ones (used for the intent brain's "--reasoning off" —
   confirmed empirically necessary: without it, Qwen3-family "thinking"
   models burn the whole token budget on internal reasoning and never
   produce the structured-output answer at all). Its stdout+stderr are
   captured to <data_dir>/logs/<name>.log (the only way to see why a load
   failed). Returns 0 on success, -1 on failure (name already loaded,
   registry full, or spawn failed). */
int fb_backend_mgr_load(FbBackendMgr *mgr, const char *name, const char *model_path,
                         const char *llama_server_exe, const char *data_dir, int requested_port,
                         const char *const *extra_args);

/* Stops and removes the named backend. Returns 0 on success, -1 if not found. */
int fb_backend_mgr_unload(FbBackendMgr *mgr, const char *name);

FbBackendInstance *fb_backend_mgr_find(FbBackendMgr *mgr, const char *name);

/* Polls /health for every STARTING backend, flipping it to UP once ready
   or ERROR if the process exited first. Call once per UI frame. */
void fb_backend_mgr_tick(FbBackendMgr *mgr);

/* Stops every backend and frees their process handles. */
void fb_backend_mgr_shutdown(FbBackendMgr *mgr);

#endif /* FB_BACKEND_MGR_H */
