#include "diary/diary_job.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "net/http_client.h"

/* Same reasoning as chat/chat_job.c: a real summarization pass can take a
   while, so this is generous but still bounded. */
#define FB_DIARY_JOB_TIMEOUT_MS 120000

typedef struct {
    FbDiaryJob *job;
    int port;
    char instruction[512];
    char *content; /* malloc'd copy, freed by the worker */
} DiaryJobArgs;

int fb_diary_job_run_sync(int port, const char *instruction, const char *content, char *out_result,
                           size_t out_cap) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", "diary");
    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", instruction);
    cJSON_AddItemToArray(messages, sys_msg);
    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", content);
    cJSON_AddItemToArray(messages, user_msg);
    cJSON_AddNumberToObject(root, "temperature", 0.3);
    cJSON_AddNumberToObject(root, "max_tokens", 1024);
    /* The brain is always launched with --reasoning off (app.c), which
       already disables this server-wide, so this is redundant belt-and-
       suspenders rather than a fix for an observed failure here -- see
       chat/chat_job.c's identical addition, where it *was* an observed,
       reproduced failure (empty content, whole token budget burned on
       reasoning) for whatever model chat/diary's live conversation
       targets, which has no such launch-time flag. Also matters for
       --sweep-summarize: it can target *any* loaded model, not just the
       brain, so this can't be assumed away there either. */
    cJSON *tmpl_kwargs = cJSON_AddObjectToObject(root, "chat_template_kwargs");
    cJSON_AddBoolToObject(tmpl_kwargs, "enable_thinking", 0);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    int ok = 0;
    if (body) {
        FbHttpResponse resp;
        int rc = fb_http_request(port, "POST", "/v1/chat/completions", "application/json", body,
                                  strlen(body), FB_DIARY_JOB_TIMEOUT_MS, &resp);
        cJSON_free(body);

        if (rc == 0) {
            cJSON *outer = cJSON_Parse(resp.body);
            fb_http_response_free(&resp);
            if (outer) {
                cJSON *choices = cJSON_GetObjectItemCaseSensitive(outer, "choices");
                cJSON *choice0 = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
                cJSON *message =
                    choice0 ? cJSON_GetObjectItemCaseSensitive(choice0, "message") : NULL;
                cJSON *content_j =
                    message ? cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;
                if (cJSON_IsString(content_j) && content_j->valuestring[0] != '\0') {
                    snprintf(out_result, out_cap, "%s", content_j->valuestring);
                    ok = 1;
                }
                cJSON_Delete(outer);
            }
        }
    }

    if (!ok) snprintf(out_result, out_cap, "consolidation request failed or timed out");
    return ok ? 0 : -1;
}

static void diary_worker(void *arg) {
    DiaryJobArgs *args = (DiaryJobArgs *)arg;
    FbDiaryJob *job = args->job;

    int rc = fb_diary_job_run_sync(args->port, args->instruction, args->content, job->result,
                                    sizeof(job->result));
    free(args->content);
    free(args);
    job->state = rc == 0 ? FB_DIARY_JOB_DONE : FB_DIARY_JOB_ERROR; /* hand-off */
}

void fb_diary_job_init(FbDiaryJob *job) {
    memset(job, 0, sizeof(*job));
    job->state = FB_DIARY_JOB_IDLE;
}

int fb_diary_job_start(FbDiaryJob *job, int port, const char *instruction, const char *content) {
    if (job->state == FB_DIARY_JOB_RUNNING) return -1;

    DiaryJobArgs *args = (DiaryJobArgs *)malloc(sizeof(DiaryJobArgs));
    if (!args) return -1;
    args->job = job;
    args->port = port;
    snprintf(args->instruction, sizeof(args->instruction), "%s", instruction);

    size_t content_len = strlen(content);
    args->content = (char *)malloc(content_len + 1);
    if (!args->content) {
        free(args);
        return -1;
    }
    memcpy(args->content, content, content_len + 1);

    job->state = FB_DIARY_JOB_RUNNING;
    job->thread = fb_thread_start(diary_worker, args);
    if (!job->thread) {
        job->state = FB_DIARY_JOB_ERROR;
        snprintf(job->result, sizeof(job->result), "failed to start consolidation request");
        free(args->content);
        free(args);
        return -1;
    }
    return 0;
}

void fb_diary_job_tick(FbDiaryJob *job) {
    if (job->state == FB_DIARY_JOB_RUNNING) return;
    if (!job->thread) return;
    fb_thread_join(job->thread);
    job->thread = NULL;
}

void fb_diary_job_consume(FbDiaryJob *job) { job->state = FB_DIARY_JOB_IDLE; }

void fb_diary_job_shutdown(FbDiaryJob *job) {
    if (job->thread) {
        fb_thread_join(job->thread);
        job->thread = NULL;
    }
}
