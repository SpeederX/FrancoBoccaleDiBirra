#ifndef FB_DOWNLOAD_JOB_H
#define FB_DOWNLOAD_JOB_H

#include "platform/proc.h"

typedef enum {
    FB_DOWNLOAD_IDLE = 0,
    FB_DOWNLOAD_RUNNING,
    FB_DOWNLOAD_DONE,
    FB_DOWNLOAD_ERROR
} FbDownloadState;

#define FB_DOWNLOAD_URL_MAX 1024
#define FB_DOWNLOAD_PATH_MAX 1024

typedef struct {
    FbDownloadState state;
    FbProcess *proc;
    char url[FB_DOWNLOAD_URL_MAX];
    char model_name[128];
    char final_path[FB_DOWNLOAD_PATH_MAX];
    char part_path[FB_DOWNLOAD_PATH_MAX];
    char stdout_path[FB_DOWNLOAD_PATH_MAX];
    char log_path[FB_DOWNLOAD_PATH_MAX];
    char error[256];
    unsigned long long start_bytes;
    int exit_code;
    int retried_without_resume;
} FbDownloadJob;

void fb_download_job_init(FbDownloadJob *job);

/* Starts a direct HTTP(S) GGUF download into <data_dir>/models. A partial
   file uses the .part suffix and is resumed on the next identical request.
   Hugging Face /blob/ links are normalized to /resolve/ links. Returns 0
   after either starting curl or finding an existing completed file; -1 on
   validation/spawn failure, with job->error filled. */
int fb_download_job_start(FbDownloadJob *job, const char *url, const char *data_dir);

/* Non-blocking process poll. DONE/ERROR remain sticky until consume. */
void fb_download_job_tick(FbDownloadJob *job);
void fb_download_job_consume(FbDownloadJob *job);

/* Stops an active transfer and retains its .part file for a later resume. */
void fb_download_job_cancel(FbDownloadJob *job);
void fb_download_job_shutdown(FbDownloadJob *job);

/* Current on-disk byte count, including bytes from a prior partial run. */
unsigned long long fb_download_job_bytes(const FbDownloadJob *job);

#endif /* FB_DOWNLOAD_JOB_H */
