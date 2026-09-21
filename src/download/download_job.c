#include "download/download_job.h"

#include <stdio.h>
#include <string.h>

#include "platform/dirlist.h"
#include "util/paths.h"

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int ends_with_gguf(const char *s) {
    size_t n = strlen(s);
    if (n < 5) return 0;
    const char *ext = s + n - 5;
    return ext[0] == '.' && (ext[1] == 'g' || ext[1] == 'G') &&
           (ext[2] == 'g' || ext[2] == 'G') && (ext[3] == 'u' || ext[3] == 'U') &&
           (ext[4] == 'f' || ext[4] == 'F');
}

static int is_http_url(const char *url) {
    return strncmp(url, "https://", 8) == 0 || strncmp(url, "http://", 7) == 0;
}

static void set_error(FbDownloadJob *job, const char *message) {
    snprintf(job->error, sizeof(job->error), "%s", message);
    job->state = FB_DOWNLOAD_ERROR;
}

static int normalize_url(const char *url, char *out, size_t out_cap) {
    const char *blob = strstr(url, "/blob/");
    if (strstr(url, "huggingface.co/") && blob) {
        size_t prefix = (size_t)(blob - url);
        int n = snprintf(out, out_cap, "%.*s/resolve/%s", (int)prefix, url, blob + 6);
        return n >= 0 && (size_t)n < out_cap;
    }
    int n = snprintf(out, out_cap, "%s", url);
    return n >= 0 && (size_t)n < out_cap;
}

static int filename_from_url(const char *url, char *out, size_t out_cap) {
    const char *end = url + strlen(url);
    const char *query = strpbrk(url, "?#");
    if (query) end = query;

    const char *start = end;
    while (start > url && start[-1] != '/') start--;
    if (start == end) return 0;

    size_t off = 0;
    for (const char *p = start; p < end && off + 1 < out_cap; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '%' && p + 2 < end) {
            int hi = hex_value(p[1]);
            int lo = hex_value(p[2]);
            if (hi >= 0 && lo >= 0) {
                c = (unsigned char)((hi << 4) | lo);
                p += 2;
            }
        }
        if (c < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
            c == '\\' || c == '|' || c == '?' || c == '*') {
            c = '_';
        }
        out[off++] = (char)c;
    }
    out[off] = '\0';
    return off > 0 && ends_with_gguf(out);
}

static void model_name_from_filename(const char *filename, char *out, size_t out_cap) {
    size_t len = strlen(filename);
    if (len >= 5) len -= 5; /* validated .gguf suffix */
    size_t off = 0;
    int previous_dash = 0;
    for (size_t i = 0; i < len && off + 1 < out_cap; ++i) {
        unsigned char c = (unsigned char)filename[i];
        int keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!keep) c = '-';
        if (c == '-' && previous_dash) continue;
        out[off++] = (char)c;
        previous_dash = c == '-';
    }
    while (off > 0 && (out[off - 1] == '-' || out[off - 1] == '.')) off--;
    out[off] = '\0';
    if (off == 0) snprintf(out, out_cap, "downloaded-model");
}

static int start_curl(FbDownloadJob *job) {
    const char *argv[] = {
        FB_CURL_DEFAULT_NAME,
        "--fail",
        "--location",
        "--retry", "3",
        "--retry-delay", "2",
        "--continue-at", "-",
        "--silent",
        "--show-error",
        "--output", job->part_path,
        job->url,
        NULL,
    };
    job->proc = fb_proc_spawn(argv, job->stdout_path, job->log_path);
    if (!job->proc) {
        set_error(job, "couldn't start curl (is curl available on PATH?)");
        return -1;
    }
    job->state = FB_DOWNLOAD_RUNNING;
    return 0;
}

void fb_download_job_init(FbDownloadJob *job) {
    memset(job, 0, sizeof(*job));
    job->state = FB_DOWNLOAD_IDLE;
}

int fb_download_job_start(FbDownloadJob *job, const char *url, const char *data_dir) {
    if (job->state == FB_DOWNLOAD_RUNNING) {
        snprintf(job->error, sizeof(job->error), "another download is already running");
        return -1;
    }
    fb_download_job_init(job);
    if (!url || !is_http_url(url)) {
        set_error(job, "expected a direct http:// or https:// URL");
        return -1;
    }
    if (!data_dir || data_dir[0] == '\0') {
        set_error(job, "the app data directory is unavailable");
        return -1;
    }
    if (!normalize_url(url, job->url, sizeof(job->url))) {
        set_error(job, "URL is too long");
        return -1;
    }

    char filename[260];
    if (!filename_from_url(job->url, filename, sizeof(filename))) {
        set_error(job, "URL must end in a .gguf filename (query strings are allowed)");
        return -1;
    }
    model_name_from_filename(filename, job->model_name, sizeof(job->model_name));

    char models_dir[FB_DOWNLOAD_PATH_MAX];
    char logs_root[FB_DOWNLOAD_PATH_MAX];
    char logs_dir[FB_DOWNLOAD_PATH_MAX];
    snprintf(models_dir, sizeof(models_dir), "%s/models", data_dir);
    snprintf(logs_root, sizeof(logs_root), "%s/logs", data_dir);
    snprintf(logs_dir, sizeof(logs_dir), "%s/logs/download", data_dir);
    if (fb_ensure_dir(models_dir) != 0 ||
        fb_ensure_dir(logs_root) != 0 ||
        fb_ensure_dir(logs_dir) != 0) {
        set_error(job, "couldn't create the models/download directories");
        return -1;
    }

    snprintf(job->final_path, sizeof(job->final_path), "%s/%s", models_dir, filename);
    snprintf(job->part_path, sizeof(job->part_path), "%s.part", job->final_path);
    snprintf(job->stdout_path, sizeof(job->stdout_path), "%s/last.stdout.log", logs_dir);
    snprintf(job->log_path, sizeof(job->log_path), "%s/last.stderr.log", logs_dir);

    unsigned long long existing_size = 0;
    if (fb_file_size(job->final_path, &existing_size)) {
        job->start_bytes = existing_size;
        job->state = FB_DOWNLOAD_DONE;
        return 0;
    }
    if (fb_file_size(job->part_path, &existing_size)) job->start_bytes = existing_size;
    return start_curl(job);
}

void fb_download_job_tick(FbDownloadJob *job) {
    if (job->state != FB_DOWNLOAD_RUNNING || !job->proc) return;

    int exit_code = 0;
    int exited = fb_proc_exit_code(job->proc, &exit_code);
    if (exited == 0) return;
    fb_proc_free(job->proc);
    job->proc = NULL;
    job->exit_code = exit_code;

    /* curl 33 means the server rejected resuming at the existing offset.
       Retry once from zero automatically rather than making the user find
       and delete an implementation-detail .part file. */
    if (exit_code == 33 && job->start_bytes > 0 && !job->retried_without_resume) {
        remove(job->part_path);
        job->start_bytes = 0;
        job->retried_without_resume = 1;
        start_curl(job);
        return;
    }

    if (exited < 0 || exit_code != 0) {
        snprintf(job->error, sizeof(job->error),
                 "curl failed (exit %d); partial file kept for resume; see %s",
                 exit_code, job->log_path);
        job->state = FB_DOWNLOAD_ERROR;
        return;
    }
    if (rename(job->part_path, job->final_path) != 0) {
        snprintf(job->error, sizeof(job->error), "download finished but couldn't rename %s",
                 job->part_path);
        job->state = FB_DOWNLOAD_ERROR;
        return;
    }
    job->state = FB_DOWNLOAD_DONE;
}

void fb_download_job_consume(FbDownloadJob *job) {
    job->state = FB_DOWNLOAD_IDLE;
    job->error[0] = '\0';
}

void fb_download_job_cancel(FbDownloadJob *job) {
    if (job->proc) {
        fb_proc_stop(job->proc);
        fb_proc_free(job->proc);
        job->proc = NULL;
    }
    job->state = FB_DOWNLOAD_IDLE;
}

void fb_download_job_shutdown(FbDownloadJob *job) {
    fb_download_job_cancel(job);
}

unsigned long long fb_download_job_bytes(const FbDownloadJob *job) {
    unsigned long long size = 0;
    if (fb_file_size(job->part_path, &size)) return size;
    if (fb_file_size(job->final_path, &size)) return size;
    return 0;
}
