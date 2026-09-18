#include "platform/thread.h"

#include <windows.h>
#include <stdlib.h>

struct FbThread {
    HANDLE handle;
};

typedef struct {
    FbThreadFunc fn;
    void *arg;
} ThreadTrampolineArgs;

static DWORD WINAPI trampoline(LPVOID param) {
    ThreadTrampolineArgs *args = (ThreadTrampolineArgs *)param;
    args->fn(args->arg);
    free(args);
    return 0;
}

FbThread *fb_thread_start(FbThreadFunc fn, void *arg) {
    ThreadTrampolineArgs *args = (ThreadTrampolineArgs *)malloc(sizeof(ThreadTrampolineArgs));
    if (!args) return NULL;
    args->fn = fn;
    args->arg = arg;

    HANDLE h = CreateThread(NULL, 0, trampoline, args, 0, NULL);
    if (!h) {
        free(args);
        return NULL;
    }

    FbThread *t = (FbThread *)malloc(sizeof(FbThread));
    if (!t) {
        /* Thread is already running at this point; let it finish on its
           own (it frees `args` itself) rather than leaking that memory --
           we just can't hand the caller a handle to join later. */
        CloseHandle(h);
        return NULL;
    }
    t->handle = h;
    return t;
}

void fb_thread_join(FbThread *thread) {
    if (!thread) return;
    WaitForSingleObject(thread->handle, INFINITE);
    CloseHandle(thread->handle);
    free(thread);
}
