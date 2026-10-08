#include "meeting/parakeet_stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define OPEN_LIBRARY(path) LoadLibraryA(path)
#define LOAD_SYMBOL(handle, name) GetProcAddress(handle, name)
#define CLOSE_LIBRARY(handle) FreeLibrary(handle)
typedef HMODULE Library;
#else
#include <dlfcn.h>
#define OPEN_LIBRARY(path) dlopen(path, RTLD_NOW | RTLD_LOCAL)
#define LOAD_SYMBOL(handle, name) dlsym(handle, name)
#define CLOSE_LIBRARY(handle) dlclose(handle)
typedef void *Library;
#endif

/* Flat C ABI v10 from parakeet.cpp v0.6.1 include/parakeet_capi.h.
   Options are NULL (upstream defaults); contexts/session stay opaque. */
struct FbParakeetStream {
    Library library;
    int (*abi)(void);
    void *(*load)(const char *);
    void *(*load_component)(const char *, const char *);
    void (*free_context)(void *);
    const char *(*load_error)(void);
    void *(*begin)(void *, void *, void *, const void *);
    char *(*feed)(void *, const float *, int, int);
    const char *(*last_error)(void *);
    void (*free_string)(char *);
    void (*free_session)(void *);
    void *asr;
    void *diar;
    void *session;
};

#define RESOLVE(member, symbol) do { \
    void (*address)(void) = (void (*)(void))LOAD_SYMBOL(stream->library, symbol); \
    if (!address) { snprintf(error, capacity, "Parakeet library lacks %s", symbol); goto failed; } \
    memcpy(&stream->member, &address, sizeof(address)); \
} while (0)

FbParakeetStream *fb_parakeet_stream_open(const char *library, const char *asr, const char *diar,
                                         char *error, size_t capacity) {
    FbParakeetStream *stream = calloc(1, sizeof(*stream));
    if (!stream) {
        snprintf(error, capacity, "could not allocate the Parakeet session");
        return NULL;
    }
    stream->library = OPEN_LIBRARY(library);
    if (!stream->library) {
        snprintf(error, capacity, "could not load the Parakeet library; use /meeting setup library <path>");
        goto failed;
    }
    RESOLVE(abi, "parakeet_capi_abi_version");
    RESOLVE(load, "parakeet_capi_load");
    RESOLVE(load_component, "parakeet_capi_load_component");
    RESOLVE(free_context, "parakeet_capi_free");
    RESOLVE(load_error, "parakeet_capi_load_error");
    RESOLVE(begin, "parakeet_capi_scene_stream_begin");
    RESOLVE(feed, "parakeet_capi_scene_stream_feed_json");
    RESOLVE(last_error, "parakeet_capi_scene_stream_last_error");
    RESOLVE(free_string, "parakeet_capi_free_string");
    RESOLVE(free_session, "parakeet_capi_scene_stream_free");
    if (stream->abi() != 10) {
        snprintf(error, capacity, "this integration requires Parakeet C ABI 10 (v0.6.1)");
        goto failed;
    }
    stream->asr = stream->load(asr);
    if (!stream->asr) {
        snprintf(error, capacity, "ASR: %.200s", stream->load_error());
        goto failed;
    }
    stream->diar = stream->load_component(diar, "diar");
    if (!stream->diar) {
        stream->diar = stream->load(diar);
    }
    if (!stream->diar) {
        snprintf(error, capacity, "diarization: %.180s", stream->load_error());
        goto failed;
    }
    stream->session = stream->begin(stream->asr, stream->diar, NULL, NULL);
    if (!stream->session) {
        snprintf(error, capacity, "could not begin the Parakeet scene session; check ASR/diar model kinds");
        goto failed;
    }
    return stream;
failed:
    fb_parakeet_stream_free(stream);
    return NULL;
}

char *fb_parakeet_stream_feed(FbParakeetStream *stream, const float *pcm, int frames, int final,
                              char *error, size_t capacity) {
    char *json = stream->feed(stream->session, pcm, frames, final);
    if (!json) {
        snprintf(error, capacity, "Parakeet stream: %.200s", stream->last_error(stream->session));
        return NULL;
    }
    size_t size = strlen(json) + 1;
    char *copy = malloc(size);
    if (copy) {
        memcpy(copy, json, size);
    } else {
        snprintf(error, capacity, "could not allocate the transcription update");
    }
    stream->free_string(json);
    return copy;
}

void fb_parakeet_stream_free(FbParakeetStream *stream) {
    if (!stream) {
        return;
    }
    if (stream->session) {
        stream->free_session(stream->session);
    }
    if (stream->asr) {
        stream->free_context(stream->asr);
    }
    if (stream->diar) {
        stream->free_context(stream->diar);
    }
    if (stream->library) {
        CLOSE_LIBRARY(stream->library);
    }
    free(stream);
}
