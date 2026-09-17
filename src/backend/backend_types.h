#ifndef FB_BACKEND_TYPES_H
#define FB_BACKEND_TYPES_H

#include "platform/proc.h"

typedef enum {
    FB_BACKEND_LLAMA_CPP,
    FB_BACKEND_VLLM /* reserved for a later phase; not implemented */
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
