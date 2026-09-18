#ifndef FB_DIARY_JOB_H
#define FB_DIARY_JOB_H

#include <stddef.h>

#include "platform/thread.h"

/*
 * A one-shot background summarization request: send some text plus an
 * instruction, get back a distilled result. Used for both of diary mode's
 * consolidation steps -- sleep (a closed page -> long-term memory) and
 * checkpoint (long-term memory -> a more compact version of itself) -- so
 * it deliberately carries no turn history of its own, unlike chat/chat_job.h
 * which is a live multi-turn conversation. Same background-thread pattern
 * as chat_job.h: state is the hand-off, response is valid once state is
 * DONE or ERROR, tick/consume/shutdown follow the same contract.
 */

typedef enum {
    FB_DIARY_JOB_IDLE = 0,
    FB_DIARY_JOB_RUNNING,
    FB_DIARY_JOB_DONE,
    FB_DIARY_JOB_ERROR
} FbDiaryJobState;

#define FB_DIARY_JOB_RESULT_MAX 6000

typedef struct {
    volatile FbDiaryJobState state;
    char result[FB_DIARY_JOB_RESULT_MAX];
    FbThread *thread;
} FbDiaryJob;

/* The exact instructions the real pipeline uses -- exposed (not just
   private #defines in diary_job.c) so a dry run (the --sweep-summarize CLI
   tool) can send the *identical* framing a real sleep/checkpoint pass
   would, not an approximation of it. GENERIC is for content that isn't a
   diary page or long-term memory at all (--source=file:... in a sweep). */
#define FB_DIARY_SLEEP_INSTRUCTION                                                              \
    "You are a diary's memory consolidation process, running silently in the background. "      \
    "Read the diary page below (a record of a conversation) and write a short, dense summary "  \
    "capturing what happened, what was decided, what mattered, and anything worth remembering " \
    "long-term. Write only the summary itself, no preamble, no headers."

#define FB_DIARY_CHECKPOINT_INSTRUCTION                                                        \
    "You are compressing accumulated long-term diary memory that has grown too large. Read the " \
    "memory below and rewrite it more concisely, preserving all genuinely important facts, "    \
    "decisions, and recurring themes, dropping redundant or stale detail. Write only the "      \
    "compressed memory itself, no preamble, no headers."

#define FB_DIARY_GENERIC_SUMMARY_INSTRUCTION                                                   \
    "Summarize the text below concisely, preserving the genuinely important facts, decisions, " \
    "and ideas. Write only the summary itself, no preamble, no headers."

void fb_diary_job_init(FbDiaryJob *job);

/* instruction is the system-role framing ("distill this diary page...",
   "compress this long-term memory..."); content is the user-role text to
   process. Returns 0 if started, -1 if already running or the thread
   failed to start. */
int fb_diary_job_start(FbDiaryJob *job, int port, const char *instruction, const char *content);

/* Same "call every frame, DONE/ERROR is sticky until consumed" contract as
   fb_chat_tick/fb_chat_consume. */
void fb_diary_job_tick(FbDiaryJob *job);
void fb_diary_job_consume(FbDiaryJob *job);
void fb_diary_job_shutdown(FbDiaryJob *job);

/* The same request, sent directly and blocking until it returns -- no
   background thread, no FbDiaryJob state machine. For a caller with no UI
   thread to keep responsive (the --sweep-summarize CLI tool), which is
   also why this is worth exposing separately rather than just calling
   fb_diary_job_start and spin-waiting on fb_diary_job_tick. Always fills
   out_result (bounded by out_cap) -- a human-readable error message on
   failure, safe to display either way. Returns 0 on success, -1 on
   failure. */
int fb_diary_job_run_sync(int port, const char *instruction, const char *content, char *out_result,
                           size_t out_cap);

#endif /* FB_DIARY_JOB_H */
