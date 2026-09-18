#include "chat/chat_job.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "net/http_client.h"

/* Chat completions can take far longer than the Qwen brain's tiny classify
   calls (see platform/thread.h's header comment) -- generous but still
   bounded, so a genuinely hung server doesn't wedge the background thread
   forever. */
#define FB_CHAT_TIMEOUT_MS 120000

static void append_turn(FbChatJob *job, const char *role, const char *text) {
    if (job->turn_count >= FB_CHAT_MAX_TURNS) {
        for (int i = 1; i < FB_CHAT_MAX_TURNS; ++i) job->turns[i - 1] = job->turns[i];
        job->turn_count = FB_CHAT_MAX_TURNS - 1;
    }
    FbChatTurn *t = &job->turns[job->turn_count++];
    snprintf(t->role, sizeof(t->role), "%s", role);
    snprintf(t->text, sizeof(t->text), "%s", text);
}

void fb_chat_job_init(FbChatJob *job) {
    memset(job, 0, sizeof(*job));
    job->state = FB_CHAT_IDLE;
}

void fb_chat_reset(FbChatJob *job) { job->turn_count = 0; }

void fb_chat_inject_turn(FbChatJob *job, const char *role, const char *text) {
    append_turn(job, role, text);
}

/* Owns a private snapshot of the turns to send: the background thread
   never touches job->turns directly, so there's no race with a later
   fb_chat_tick appending the reply while a *different*, newer request
   might already be building its own snapshot. */
typedef struct {
    FbChatJob *job;
    FbChatTurn turns[FB_CHAT_MAX_TURNS];
    int turn_count;
    int port;
} ChatRequestArgs;

/* /props's n_ctx is cheap and local, so this just re-fetches it every
   request rather than caching -- simplest way to stay correct if the
   target model ever changes out from under a stale cache. */
static int fetch_n_ctx(int port) {
    FbHttpResponse resp;
    int rc = fb_http_request(port, "GET", "/props", NULL, NULL, 0, 5000, &resp);
    if (rc != 0) return 0;

    int n_ctx = 0;
    cJSON *root = cJSON_Parse(resp.body);
    fb_http_response_free(&resp);
    if (root) {
        cJSON *dgs = cJSON_GetObjectItemCaseSensitive(root, "default_generation_settings");
        cJSON *ctx = dgs ? cJSON_GetObjectItemCaseSensitive(dgs, "n_ctx") : NULL;
        if (cJSON_IsNumber(ctx)) n_ctx = (int)ctx->valuedouble;
        cJSON_Delete(root);
    }
    return n_ctx;
}

static void chat_worker(void *arg) {
    ChatRequestArgs *args = (ChatRequestArgs *)arg;
    FbChatJob *job = args->job;

    job->n_ctx = fetch_n_ctx(args->port);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", "chat");
    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    for (int i = 0; i < args->turn_count; ++i) {
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", args->turns[i].role);
        cJSON_AddStringToObject(msg, "content", args->turns[i].text);
        cJSON_AddItemToArray(messages, msg);
    }
    cJSON_AddNumberToObject(root, "temperature", 0.7);
    cJSON_AddNumberToObject(root, "max_tokens", 512);
    /* Confirmed directly against a real Qwen3.5 instance: with no target-
       model flag equivalent to the brain's --reasoning off (that's a
       launch-time flag, and chat/diary's target is whatever the user
       loaded via /load, not something we control the launch args of), a
       short message like "wtf" burned the *entire* max_tokens budget on
       reasoning_content and returned empty content -- a real, reproduced
       blank-reply bug, not a one-off. This is the per-request equivalent
       for reasoning-capable chat templates (the Jinja variable is named
       exactly this in Qwen's own template); harmless no-op for templates
       that don't reference it. */
    cJSON *tmpl_kwargs = cJSON_AddObjectToObject(root, "chat_template_kwargs");
    cJSON_AddBoolToObject(tmpl_kwargs, "enable_thinking", 0);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    FbChatState result_state = FB_CHAT_ERROR;
    int empty_reply = 0;
    if (body) {
        FbHttpResponse resp;
        int rc = fb_http_request(args->port, "POST", "/v1/chat/completions", "application/json",
                                  body, strlen(body), FB_CHAT_TIMEOUT_MS, &resp);
        cJSON_free(body);

        if (rc == 0) {
            cJSON *outer = cJSON_Parse(resp.body);
            fb_http_response_free(&resp);
            if (outer) {
                cJSON *choices = cJSON_GetObjectItemCaseSensitive(outer, "choices");
                cJSON *choice0 = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
                cJSON *message =
                    choice0 ? cJSON_GetObjectItemCaseSensitive(choice0, "message") : NULL;
                cJSON *content =
                    message ? cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;
                /* Empty (not just missing) content is a real, reproduced
                   failure mode -- a reasoning model can spend its whole
                   token budget on reasoning_content and never emit an
                   answer (see the chat_template_kwargs comment above,
                   which is the actual fix; this is the fallback for
                   whatever that doesn't catch) -- treat it as a failure
                   instead of silently "succeeding" with a blank reply. */
                if (cJSON_IsString(content) && content->valuestring[0] != '\0') {
                    snprintf(job->response, sizeof(job->response), "%s", content->valuestring);
                    result_state = FB_CHAT_DONE;
                } else if (cJSON_IsString(content)) {
                    empty_reply = 1;
                }

                /* Best-effort: llama-server includes these in every
                   non-streamed /v1/chat/completions response (confirmed
                   directly against a running instance), but nothing here
                   depends on them being present. */
                cJSON *usage = cJSON_GetObjectItemCaseSensitive(outer, "usage");
                if (usage) {
                    cJSON *pt = cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens");
                    cJSON *ct = cJSON_GetObjectItemCaseSensitive(usage, "completion_tokens");
                    cJSON *tt = cJSON_GetObjectItemCaseSensitive(usage, "total_tokens");
                    if (cJSON_IsNumber(pt)) job->prompt_tokens = (int)pt->valuedouble;
                    if (cJSON_IsNumber(ct)) job->completion_tokens = (int)ct->valuedouble;
                    if (cJSON_IsNumber(tt)) job->total_tokens = (int)tt->valuedouble;
                }
                cJSON *timings = cJSON_GetObjectItemCaseSensitive(outer, "timings");
                if (timings) {
                    cJSON *pps = cJSON_GetObjectItemCaseSensitive(timings, "prompt_per_second");
                    cJSON *dps = cJSON_GetObjectItemCaseSensitive(timings, "predicted_per_second");
                    if (cJSON_IsNumber(pps)) job->prompt_tok_per_sec = pps->valuedouble;
                    if (cJSON_IsNumber(dps)) job->predicted_tok_per_sec = dps->valuedouble;
                }
                cJSON_Delete(outer);
            }
        }
    }

    if (result_state == FB_CHAT_ERROR) {
        snprintf(job->response, sizeof(job->response),
                 empty_reply ? "model returned an empty reply (a reasoning model spending its "
                               "whole token budget on internal reasoning is the usual cause)"
                             : "request failed or timed out");
    }
    free(args);
    job->state = result_state; /* hand-off: render thread may now read job->response */
}

int fb_chat_start(FbChatJob *job, int port, const char *user_text) {
    if (job->state == FB_CHAT_RUNNING) return -1;

    append_turn(job, "user", user_text);

    ChatRequestArgs *args = (ChatRequestArgs *)malloc(sizeof(ChatRequestArgs));
    if (!args) return -1;
    args->job = job;
    args->port = port;
    args->turn_count = job->turn_count;
    memcpy(args->turns, job->turns, sizeof(FbChatTurn) * (size_t)job->turn_count);

    job->port = port;
    job->state = FB_CHAT_RUNNING;
    job->thread = fb_thread_start(chat_worker, args);
    if (!job->thread) {
        job->state = FB_CHAT_ERROR;
        snprintf(job->response, sizeof(job->response), "failed to start chat request");
        free(args);
        return -1;
    }
    return 0;
}

void fb_chat_tick(FbChatJob *job) {
    if (job->state == FB_CHAT_RUNNING) return;
    if (!job->thread) return; /* nothing new: never started, or already consumed */

    fb_thread_join(job->thread);
    job->thread = NULL;
    if (job->state == FB_CHAT_DONE) append_turn(job, "assistant", job->response);
}

void fb_chat_consume(FbChatJob *job) { job->state = FB_CHAT_IDLE; }

void fb_chat_shutdown(FbChatJob *job) {
    if (job->thread) {
        fb_thread_join(job->thread);
        job->thread = NULL;
    }
}
