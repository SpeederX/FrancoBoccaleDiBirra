#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform/platform.h"
#ifdef _OPENMP
#include <omp.h>
#endif
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif
typedef struct { double time; int chunk; } Scene;
EXPORT int parakeet_capi_abi_version(void) { return 10; }
EXPORT void *parakeet_capi_load(const char *path) {
#ifdef _OPENMP
#pragma omp parallel num_threads(2)
    {
        volatile int worker = omp_get_thread_num();
        (void)worker;
    }
#endif
    return strcmp(path, "badload") == 0 ? NULL : malloc(1);
}
EXPORT void *parakeet_capi_load_component(const char *path, const char *component) {
    (void)component;
    return parakeet_capi_load(path);
}
EXPORT void parakeet_capi_free(void *context) { free(context); }
EXPORT const char *parakeet_capi_load_error(void) { return "fixture model load failure"; }
EXPORT void *parakeet_capi_scene_stream_begin(void *asr, void *diar, void *tagger, const void *opts) {
    (void)asr; (void)diar; (void)tagger; (void)opts;
    return calloc(1, sizeof(Scene));
}
EXPORT char *parakeet_capi_scene_stream_feed_json(void *session, const float *pcm, int frames, int final) {
    (void)pcm; (void)final;
    if (frames < 0 || frames > 320000) {
        return NULL;
    }
    Scene *scene = session;
    fb_sleep_ms(80);
    char json[1024];
    if (frames) {
        snprintf(json, sizeof(json), "{\"utterances\":[{\"speaker\":%d,\"text\":\"block %d\",\"start\":%.2f,\"end\":%.2f}]}",
                 scene->chunk % 2 ? 7 : 3, scene->chunk + 1, scene->time, scene->time + (double)frames / 16000);
        scene->time += (double)frames / 16000;
        scene->chunk++;
    } else {
        snprintf(json, sizeof(json), "{\"utterances\":[]}");
    }
    char *copy = malloc(strlen(json) + 1);
    strcpy(copy, json);
    return copy;
}
EXPORT const char *parakeet_capi_scene_stream_last_error(void *session) {
    (void)session;
    return "fixture received more than 20 seconds";
}
EXPORT void parakeet_capi_free_string(char *text) { free(text); }
EXPORT void parakeet_capi_scene_stream_free(void *session) { free(session); }
