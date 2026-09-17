#include "backend/backend_mgr.h"

#include <stdio.h>
#include <string.h>

#include "net/llama_client.h"
#include "net/sockets_compat.h"
#include "platform/platform.h"
#include "util/paths.h"

/* Health checks are blocking sockets calls made from the UI thread's tick,
   so they're rate-limited per backend rather than attempted every frame —
   otherwise a 33ms-interval render loop could stall visibly on a probe. */
#define FB_HEALTH_PROBE_INTERVAL_MS 400
#define FB_HEALTH_PROBE_TIMEOUT_MS 150
#define FB_BACKEND_MAX_ARGV 32

void fb_backend_mgr_init(FbBackendMgr *mgr) {
    memset(mgr, 0, sizeof(*mgr));
}

FbBackendInstance *fb_backend_mgr_find(FbBackendMgr *mgr, const char *name) {
    for (int i = 0; i < mgr->count; ++i) {
        if (strcmp(mgr->items[i].name, name) == 0) return &mgr->items[i];
    }
    return NULL;
}

int fb_backend_mgr_load(FbBackendMgr *mgr, const char *name, const char *model_path,
                         const char *llama_server_exe, const char *data_dir, int requested_port,
                         const char *const *extra_args) {
    FbBackendInstance *existing = fb_backend_mgr_find(mgr, name);
    if (existing) {
        /* A dead/errored entry shouldn't block retrying /load — clean it up
           and fall through to a fresh spawn instead of making the user
           /unload by hand first. A live one (STARTING/UP) still blocks. */
        if (existing->state == FB_BACKEND_ERROR || fb_proc_has_exited(existing->proc)) {
            fb_backend_mgr_unload(mgr, name);
        } else {
            return -1;
        }
    }
    if (mgr->count >= FB_BACKEND_MGR_MAX) return -1;

    int port = requested_port != 0 ? requested_port : fb_sock_alloc_ephemeral_port();
    if (port <= 0) return -1;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    const char *argv[FB_BACKEND_MAX_ARGV];
    int argc = 0;
    argv[argc++] = llama_server_exe;
    argv[argc++] = "-m";
    argv[argc++] = model_path;
    argv[argc++] = "--port";
    argv[argc++] = port_str;
    argv[argc++] = "--host";
    argv[argc++] = "127.0.0.1";
    if (extra_args) {
        for (int i = 0; extra_args[i] && argc < FB_BACKEND_MAX_ARGV - 1; ++i) {
            argv[argc++] = extra_args[i];
        }
    }
    argv[argc] = NULL;

    char logs_dir[FB_BACKEND_PATH_MAX];
    char log_path[FB_BACKEND_PATH_MAX];   /* stderr: llama-server's diagnostics live here */
    char stdout_path[FB_BACKEND_PATH_MAX]; /* captured for completeness, not surfaced in the UI */
    snprintf(logs_dir, sizeof(logs_dir), "%s/logs", data_dir);
    fb_ensure_dir(logs_dir);
    snprintf(log_path, sizeof(log_path), "%s/%s.log", logs_dir, name);
    snprintf(stdout_path, sizeof(stdout_path), "%s/%s.stdout.log", logs_dir, name);

    FbProcess *proc = fb_proc_spawn(argv, stdout_path, log_path);
    if (!proc) return -1;

    FbBackendInstance *slot = &mgr->items[mgr->count];
    memset(slot, 0, sizeof(*slot));
    strncpy(slot->name, name, FB_BACKEND_NAME_MAX - 1);
    strncpy(slot->model_path, model_path, FB_BACKEND_PATH_MAX - 1);
    strncpy(slot->log_path, log_path, FB_BACKEND_PATH_MAX - 1);
    slot->kind = FB_BACKEND_LLAMA_CPP;
    slot->state = FB_BACKEND_STARTING;
    slot->port = port;
    slot->proc = proc;
    mgr->count++;
    return 0;
}

int fb_backend_mgr_unload(FbBackendMgr *mgr, const char *name) {
    for (int i = 0; i < mgr->count; ++i) {
        if (strcmp(mgr->items[i].name, name) != 0) continue;

        fb_proc_stop(mgr->items[i].proc);
        fb_proc_free(mgr->items[i].proc);

        for (int j = i; j < mgr->count - 1; ++j) mgr->items[j] = mgr->items[j + 1];
        mgr->count--;
        return 0;
    }
    return -1;
}

void fb_backend_mgr_tick(FbBackendMgr *mgr) {
    unsigned long long now = fb_now_ms();

    for (int i = 0; i < mgr->count; ++i) {
        FbBackendInstance *b = &mgr->items[i];
        if (b->state != FB_BACKEND_STARTING && b->state != FB_BACKEND_UP) continue;

        if (fb_proc_has_exited(b->proc)) {
            b->state = FB_BACKEND_ERROR;
            continue;
        }

        if (now - b->last_health_probe_ms < FB_HEALTH_PROBE_INTERVAL_MS) continue;
        b->last_health_probe_ms = now;

        if (fb_llama_health(b->port, FB_HEALTH_PROBE_TIMEOUT_MS)) {
            b->state = FB_BACKEND_UP;
        } else if (b->state == FB_BACKEND_UP) {
            /* was healthy, isn't anymore, but the process is still alive:
               treat as a transient hiccup rather than flapping to STARTING. */
        }
    }
}

void fb_backend_mgr_shutdown(FbBackendMgr *mgr) {
    for (int i = 0; i < mgr->count; ++i) {
        fb_proc_stop(mgr->items[i].proc);
        fb_proc_free(mgr->items[i].proc);
    }
    mgr->count = 0;
}
