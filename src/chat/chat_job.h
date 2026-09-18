#ifndef FB_CHAT_JOB_H
#define FB_CHAT_JOB_H

#include <stddef.h>

#include "platform/thread.h"

typedef enum {
    FB_CHAT_IDLE = 0,
    FB_CHAT_RUNNING,
    FB_CHAT_DONE,
    FB_CHAT_ERROR
} FbChatState;

#define FB_CHAT_TURN_MAX 2000
/* A bounded window, not real memory: old turns are dropped once this fills,
   both to keep the request payload (and the target model's context usage)
   from growing without limit and to match this console's own "stay
   lightweight" ethos. 20 turns is 10 user/assistant round trips. */
#define FB_CHAT_MAX_TURNS 20
#define FB_CHAT_RESPONSE_MAX 4096

typedef struct {
    char role[16]; /* "user" or "assistant" */
    char text[FB_CHAT_TURN_MAX];
} FbChatTurn;

typedef struct {
    /* Only ever written by the background thread's very last statement
       (see chat_job.c) -- that write is the hand-off point after which the
       render thread may read `response`. No lock: ownership of `response`
       moves render->background at fb_chat_start, background->render at
       that write, one direction at a time, same as mic_capture.c's
       g_recording flag. */
    volatile FbChatState state;

    FbChatTurn turns[FB_CHAT_MAX_TURNS];
    int turn_count;

    int port; /* set by fb_chat_start; read-only to the background thread */
    char response[FB_CHAT_RESPONSE_MAX]; /* valid once state is DONE or ERROR */

    /* Telemetry from the most recently completed request (0 if a request
       hasn't finished yet, or the server didn't report a field) -- this is
       "last request", not cumulative: prompt_tokens already reflects the
       whole conversation sent so far since fb_chat_start includes full
       turn history, but the *_per_sec figures are only for that one
       round trip. Persists across requests so the UI has something to
       show between them, not just during. */
    int n_ctx;
    int prompt_tokens, completion_tokens, total_tokens;
    double prompt_tok_per_sec, predicted_tok_per_sec;

    FbThread *thread;
} FbChatJob;

void fb_chat_job_init(FbChatJob *job);

/* Drops all turn history -- call when the chat target model changes (a new
   /load), since a different model has no business inheriting another
   model's conversation. */
void fb_chat_reset(FbChatJob *job);

/* Appends a turn directly to history with no request involved -- for
   restoring saved context into a live conversation (e.g. diary's "reopen an
   archived page"), as opposed to fb_chat_start's turn-plus-request. role is
   "user" or "assistant". Subject to the same FB_CHAT_MAX_TURNS drop-oldest
   bound as any other turn. */
void fb_chat_inject_turn(FbChatJob *job, const char *role, const char *text);

/* Appends user_text as a user turn and starts a background request to
   127.0.0.1:port carrying the full turn history. Returns 0 if started,
   -1 if a request is already running (caller should tell the user to
   wait) or the thread failed to start. */
int fb_chat_start(FbChatJob *job, int port, const char *user_text);

/* Call once per frame. While RUNNING, this is a cheap no-op poll. The
   first time it observes DONE or ERROR, it joins the finished background
   thread and (on DONE) folds the reply into turn history for future
   requests -- but leaves job->state and job->response alone otherwise, so
   the caller can read the result (e.g. to log it) before calling
   fb_chat_consume(). Safe to call every frame regardless of state. */
void fb_chat_tick(FbChatJob *job);

/* Resets state back to IDLE after the caller has read a DONE/ERROR result
   from fb_chat_tick. Call exactly once per such result, after reading
   job->response -- otherwise the same result gets processed again. */
void fb_chat_consume(FbChatJob *job);

/* Blocks until any in-flight request finishes. Call before freeing/
   invalidating the FbApp this job lives in -- otherwise a still-running
   background thread would write into freed memory once it completes. */
void fb_chat_shutdown(FbChatJob *job);

#endif /* FB_CHAT_JOB_H */
