#ifndef FB_THREAD_H
#define FB_THREAD_H

/*
 * Minimal cross-platform background thread, chosen by CMake source-list
 * selection (platform/win/thread_win.c vs platform/posix/thread_posix.c) --
 * same pattern as platform/proc.h. Exists for exactly one job: running a
 * blocking network call (a chat completion against a loaded model, which
 * can take far longer than the ~1s the Qwen intent brain's tiny classify
 * calls do) off the render thread, so the UI keeps ticking while it waits.
 *
 * There is no message-passing here -- the thread function is expected to
 * write its result into memory the caller already owns (see chat/chat_job.h)
 * and flip a state flag the caller polls once per frame, the same
 * lightweight pattern mic_capture.c already uses for its audio callback
 * thread. fb_thread_join exists only for shutdown: if a request is still
 * in flight when the app is closing, the caller must join before freeing
 * that memory, or the thread would write into it after it's gone.
 */

typedef void (*FbThreadFunc)(void *arg);

typedef struct FbThread FbThread; /* opaque, platform-defined */

/* Returns NULL on failure (caller should treat that as "couldn't start",
   e.g. report a chat request as failed rather than crash). */
FbThread *fb_thread_start(FbThreadFunc fn, void *arg);

/* Blocks until fn returns, then releases the handle. Safe to call once per
   successfully-started thread; never call twice on the same handle. */
void fb_thread_join(FbThread *thread);

typedef struct FbMutex FbMutex;
FbMutex *fb_mutex_create(void);
void fb_mutex_lock(FbMutex *mutex);
void fb_mutex_unlock(FbMutex *mutex);
void fb_mutex_free(FbMutex *mutex);

#endif /* FB_THREAD_H */
