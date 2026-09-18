#include "platform/thread.h"

#include <pthread.h>
#include <stdlib.h>

struct FbThread {
    pthread_t handle;
};

typedef struct {
    FbThreadFunc fn;
    void *arg;
} ThreadTrampolineArgs;

static void *trampoline(void *param) {
    ThreadTrampolineArgs *args = (ThreadTrampolineArgs *)param;
    args->fn(args->arg);
    free(args);
    return NULL;
}

FbThread *fb_thread_start(FbThreadFunc fn, void *arg) {
    ThreadTrampolineArgs *args = (ThreadTrampolineArgs *)malloc(sizeof(ThreadTrampolineArgs));
    if (!args) return NULL;
    args->fn = fn;
    args->arg = arg;

    FbThread *t = (FbThread *)malloc(sizeof(FbThread));
    if (!t) {
        free(args);
        return NULL;
    }

    if (pthread_create(&t->handle, NULL, trampoline, args) != 0) {
        free(t);
        free(args);
        return NULL;
    }
    return t;
}

void fb_thread_join(FbThread *thread) {
    if (!thread) return;
    pthread_join(thread->handle, NULL);
    free(thread);
}
