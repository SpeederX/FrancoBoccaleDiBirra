#include "diary/diary.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "platform/dirlist.h"
#include "util/fileutil.h"
#include "util/paths.h"

/* FB_DIARY_SLEEP_INSTRUCTION / FB_DIARY_CHECKPOINT_INSTRUCTION come from
   diary_job.h (via diary.h) -- moved there so --sweep-summarize can send
   the exact same framing a real consolidation pass would. */

/*
 * memory_log.jsonl -- see diary.h's file comment. One JSON object per
 * line, ids assigned in append order (so id order == file order == time
 * order, no separate counter to keep in sync): a "sleep" entry records
 * which page it distilled ({"id":3,"kind":"sleep","page":2,"ts":...}); a
 * "checkpoint" entry records every entry id it folded together
 * ({"id":7,"kind":"checkpoint","sources":[3,4,5,6],"ts":...}). An entry
 * referenced in *any* checkpoint's sources is "folded" -- superseded, no
 * longer part of current long-term memory, but kept in the log so the
 * tree can still be expanded down to it. Whatever's left unfolded is
 * exactly what fb_diary_tick's append_to_long_term calls have produced
 * that a later checkpoint hasn't compressed away yet.
 */
#define FB_DIARY_LOG_MAX_ENTRIES 512
#define FB_DIARY_LOG_MAX_SOURCES 128

typedef struct {
    int id;
    int is_checkpoint; /* 0 = sleep */
    int page;          /* valid if !is_checkpoint */
    int sources[FB_DIARY_LOG_MAX_SOURCES];
    int source_count; /* valid if is_checkpoint */
    int folded;        /* referenced as some later checkpoint's source */
} FbDiaryLogEntry;

static void log_path(const char *root_dir, char *out, size_t out_cap) {
    snprintf(out, out_cap, "%s/memory_log.jsonl", root_dir);
}

/* Loads every entry in file order into entries (bounded by max_entries),
   then marks folded. Missing/empty log is not an error -- *out_count is
   just 0, meaning "nothing consolidated yet". */
static void load_log(const char *root_dir, FbDiaryLogEntry *entries, int max_entries,
                      int *out_count) {
    *out_count = 0;

    char path[600];
    log_path(root_dir, path, sizeof(path));
    size_t len = 0;
    char *text = fb_read_file(path, &len);
    if (!text) return;

    char *line = text;
    while (line && *line && *out_count < max_entries) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        if (line[0] != '\0') {
            cJSON *obj = cJSON_Parse(line);
            if (obj) {
                cJSON *id_j = cJSON_GetObjectItemCaseSensitive(obj, "id");
                cJSON *kind_j = cJSON_GetObjectItemCaseSensitive(obj, "kind");
                if (cJSON_IsNumber(id_j) && cJSON_IsString(kind_j)) {
                    FbDiaryLogEntry *e = &entries[*out_count];
                    memset(e, 0, sizeof(*e));
                    e->id = (int)id_j->valuedouble;
                    e->is_checkpoint = strcmp(kind_j->valuestring, "checkpoint") == 0;

                    if (e->is_checkpoint) {
                        cJSON *sources = cJSON_GetObjectItemCaseSensitive(obj, "sources");
                        if (cJSON_IsArray(sources)) {
                            int n = cJSON_GetArraySize(sources);
                            for (int i = 0; i < n && e->source_count < FB_DIARY_LOG_MAX_SOURCES;
                                 ++i) {
                                cJSON *item = cJSON_GetArrayItem(sources, i);
                                if (cJSON_IsNumber(item)) {
                                    e->sources[e->source_count++] = (int)item->valuedouble;
                                }
                            }
                        }
                    } else {
                        cJSON *page_j = cJSON_GetObjectItemCaseSensitive(obj, "page");
                        e->page = cJSON_IsNumber(page_j) ? (int)page_j->valuedouble : -1;
                    }
                    (*out_count)++;
                }
                cJSON_Delete(obj);
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    free(text);

    for (int i = 0; i < *out_count; ++i) {
        if (!entries[i].is_checkpoint) continue;
        for (int s = 0; s < entries[i].source_count; ++s) {
            for (int j = 0; j < *out_count; ++j) {
                if (entries[j].id == entries[i].sources[s]) {
                    entries[j].folded = 1;
                    break;
                }
            }
        }
    }
}

static void append_log_line(const char *root_dir, const char *line) {
    char path[600];
    log_path(root_dir, path, sizeof(path));
    FILE *f = fopen(path, "ab");
    if (!f) return;
    fprintf(f, "%s\n", line);
    fclose(f);
}

static void log_sleep_event(const char *root_dir, int page_index, unsigned long long now_ms) {
    FbDiaryLogEntry entries[FB_DIARY_LOG_MAX_ENTRIES];
    int count = 0;
    load_log(root_dir, entries, FB_DIARY_LOG_MAX_ENTRIES, &count);
    int next_id = count > 0 ? entries[count - 1].id + 1 : 0;

    char line[128];
    snprintf(line, sizeof(line), "{\"id\":%d,\"kind\":\"sleep\",\"page\":%d,\"ts\":%llu}", next_id,
             page_index, now_ms);
    append_log_line(root_dir, line);
}

static void log_checkpoint_event(const char *root_dir, unsigned long long now_ms) {
    FbDiaryLogEntry entries[FB_DIARY_LOG_MAX_ENTRIES];
    int count = 0;
    load_log(root_dir, entries, FB_DIARY_LOG_MAX_ENTRIES, &count);
    int next_id = count > 0 ? entries[count - 1].id + 1 : 0;

    char sources[FB_DIARY_LOG_MAX_ENTRIES * 6 + 8];
    size_t off = 0;
    int n = snprintf(sources, sizeof(sources), "[");
    off += (size_t)n;
    int first = 1;
    for (int i = 0; i < count; ++i) {
        if (entries[i].folded) continue; /* only currently-live entries are being folded now */
        n = snprintf(sources + off, sizeof(sources) - off, "%s%d", first ? "" : ",", entries[i].id);
        if (n > 0) off += (size_t)n;
        first = 0;
    }
    snprintf(sources + off, sizeof(sources) - off, "]");

    char line[FB_DIARY_LOG_MAX_ENTRIES * 6 + 64];
    snprintf(line, sizeof(line), "{\"id\":%d,\"kind\":\"checkpoint\",\"sources\":%s,\"ts\":%llu}",
             next_id, sources, now_ms);
    append_log_line(root_dir, line);
}

static const FbDiaryLogEntry *find_entry(const FbDiaryLogEntry *entries, int count, int id) {
    for (int i = 0; i < count; ++i) {
        if (entries[i].id == id) return &entries[i];
    }
    return NULL;
}

static void tree_append(char *out, size_t out_cap, size_t *off, const char *fmt, ...) {
    if (*off >= out_cap) return;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(out + *off, out_cap - *off, fmt, args);
    va_end(args);
    if (n > 0) *off += (size_t)n;
}

static void tree_print(const FbDiaryLogEntry *entries, int count, int id, const char *prefix,
                        int is_last, char *out, size_t out_cap, size_t *off) {
    const FbDiaryLogEntry *e = find_entry(entries, count, id);
    char child_prefix[256];
    snprintf(child_prefix, sizeof(child_prefix), "%s%s", prefix, is_last ? "    " : "|   ");

    if (!e) {
        tree_append(out, out_cap, off, "%s%s(missing log entry #%d)\n", prefix,
                    is_last ? "`-- " : "|-- ", id);
        return;
    }

    if (e->is_checkpoint) {
        tree_append(out, out_cap, off, "%s%scheckpoint #%d (folded %d entries)\n", prefix,
                    is_last ? "`-- " : "|-- ", e->id, e->source_count);
        for (int i = 0; i < e->source_count; ++i) {
            tree_print(entries, count, e->sources[i], child_prefix, i == e->source_count - 1, out,
                       out_cap, off);
        }
    } else {
        tree_append(out, out_cap, off, "%s%ssleep #%d <- page %d\n", prefix,
                    is_last ? "`-- " : "|-- ", e->id, e->page);
    }
}

int fb_diary_build_source_tree(const char *data_dir, char *out, size_t out_cap) {
    if (out_cap > 0) out[0] = '\0';

    char root_dir[512];
    snprintf(root_dir, sizeof(root_dir), "%s/diary", data_dir);

    FbDiaryLogEntry entries[FB_DIARY_LOG_MAX_ENTRIES];
    int count = 0;
    load_log(root_dir, entries, FB_DIARY_LOG_MAX_ENTRIES, &count);

    if (count == 0) {
        snprintf(out, out_cap, "(nothing consolidated yet -- memory_log.jsonl doesn't exist)\n");
        return 0;
    }

    size_t off = 0;
    tree_append(out, out_cap, &off, "long-term memory is currently built from:\n");
    int live_count = 0;
    for (int i = 0; i < count; ++i) {
        if (entries[i].folded) continue;
        live_count++;
    }

    int printed = 0;
    for (int i = 0; i < count; ++i) {
        if (entries[i].folded) continue;
        printed++;
        tree_print(entries, count, entries[i].id, "", printed == live_count, out, out_cap, &off);
    }
    return live_count;
}

static void collect_leaf_pages(const FbDiaryLogEntry *entries, int count, int id, int *out_pages,
                                int max_pages, int *out_count) {
    const FbDiaryLogEntry *e = find_entry(entries, count, id);
    if (!e) return;

    if (e->is_checkpoint) {
        for (int i = 0; i < e->source_count; ++i) {
            collect_leaf_pages(entries, count, e->sources[i], out_pages, max_pages, out_count);
        }
        return;
    }

    for (int i = 0; i < *out_count; ++i) {
        if (out_pages[i] == e->page) return; /* already collected via another branch */
    }
    if (*out_count < max_pages) out_pages[(*out_count)++] = e->page;
}

int fb_diary_get_live_pages(const char *data_dir, int *out_pages, int max_pages) {
    char root_dir[512];
    snprintf(root_dir, sizeof(root_dir), "%s/diary", data_dir);

    FbDiaryLogEntry entries[FB_DIARY_LOG_MAX_ENTRIES];
    int count = 0;
    load_log(root_dir, entries, FB_DIARY_LOG_MAX_ENTRIES, &count);

    int out_count = 0;
    for (int i = 0; i < count; ++i) {
        if (entries[i].folded) continue;
        collect_leaf_pages(entries, count, entries[i].id, out_pages, max_pages, &out_count);
    }
    return out_count;
}

static void page_path(const FbDiary *d, int index, char *out, size_t out_cap) {
    snprintf(out, out_cap, "%s/pages/%04d.txt", d->root_dir, index);
}

static void long_term_path(const FbDiary *d, char *out, size_t out_cap) {
    snprintf(out, out_cap, "%s/long_term.txt", d->root_dir);
}

static void save_long_term(const FbDiary *d) {
    char path[600];
    long_term_path(d, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(d->long_term, 1, d->long_term_len, f);
    fclose(f);
}

static int count_archived_pages(const FbDiary *d) {
    char pages_dir[600];
    snprintf(pages_dir, sizeof(pages_dir), "%s/pages", d->root_dir);

    FbDirEntry entries[2048];
    int n = fb_list_dir(pages_dir, entries, 2048);
    if (n < 0) return 0;

    int count = 0;
    for (int i = 0; i < n; ++i) {
        if (!entries[i].is_dir && strstr(entries[i].name, ".txt")) count++;
    }
    return count;
}

void fb_diary_init(FbDiary *d, const char *data_dir, int qwen_port, int page_close_chars_cfg,
                    int checkpoint_chars_cfg, int idle_close_minutes_cfg) {
    memset(d, 0, sizeof(*d));
    d->qwen_port = qwen_port;
    d->browse_index = -1;
    d->edit_target = -1;
    fb_diary_job_init(&d->job);

    d->page_close_chars = page_close_chars_cfg > 0 ? (size_t)page_close_chars_cfg
                                                    : FB_DIARY_DEFAULT_PAGE_CLOSE_CHARS;
    if (d->page_close_chars > FB_DIARY_PAGE_MAX_CHARS) d->page_close_chars = FB_DIARY_PAGE_MAX_CHARS;

    d->ltm_checkpoint_chars = checkpoint_chars_cfg > 0 ? (size_t)checkpoint_chars_cfg
                                                        : FB_DIARY_DEFAULT_CHECKPOINT_CHARS;
    if (d->ltm_checkpoint_chars > FB_DIARY_LTM_MAX_CHARS) {
        d->ltm_checkpoint_chars = FB_DIARY_LTM_MAX_CHARS;
    }

    d->idle_close_ms = idle_close_minutes_cfg > 0
                            ? (unsigned long long)idle_close_minutes_cfg * 60ULL * 1000ULL
                            : FB_DIARY_DEFAULT_IDLE_CLOSE_MS;

    snprintf(d->root_dir, sizeof(d->root_dir), "%s/diary", data_dir);
    fb_ensure_dir(d->root_dir);
    char pages_dir[600];
    snprintf(pages_dir, sizeof(pages_dir), "%s/pages", d->root_dir);
    fb_ensure_dir(pages_dir);

    char path[600];
    long_term_path(d, path, sizeof(path));
    size_t len = 0;
    char *text = fb_read_file(path, &len);
    if (text) {
        if (len >= sizeof(d->long_term)) len = sizeof(d->long_term) - 1;
        memcpy(d->long_term, text, len);
        d->long_term_len = len;
        free(text);
    }

    d->archived_count = count_archived_pages(d);
}

void fb_diary_append_turn(FbDiary *d, const char *role, const char *text,
                           unsigned long long now_ms) {
    d->last_activity_ms = now_ms;
    int n = snprintf(d->page_buf + d->page_len, sizeof(d->page_buf) - d->page_len, "%s%s: %s\n",
                      d->page_len > 0 ? "\n" : "", role, text);
    if (n > 0) {
        size_t written = (size_t)n;
        if (written > sizeof(d->page_buf) - 1 - d->page_len) {
            written = sizeof(d->page_buf) - 1 - d->page_len;
        }
        d->page_len += written;
    }
    d->page_has_content = 1;

    if (d->page_len >= d->page_close_chars) fb_diary_close_page(d);
}

void fb_diary_close_page(FbDiary *d) {
    if (!d->page_has_content || d->page_len == 0) return;
    if (d->job.state == FB_DIARY_JOB_RUNNING) return; /* retried on the next trigger */

    /* A reopened page (edit_target >= 0) overwrites its own existing file
       in place -- it's a continuation of a page that already exists, not a
       new one -- and doesn't bump archived_count. */
    int target_index = d->edit_target >= 0 ? d->edit_target : d->archived_count;
    char path[600];
    page_path(d, target_index, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(d->page_buf, 1, d->page_len, f);
        fclose(f);
    }
    if (d->edit_target < 0) d->archived_count++;
    d->edit_target = -1;

    d->job_target_page_index = target_index;
    snprintf(d->job_target_page, sizeof(d->job_target_page), "%s", d->page_buf);
    d->page_len = 0;
    d->page_buf[0] = '\0';
    d->page_has_content = 0;

    if (d->qwen_port != 0) {
        if (fb_diary_job_start(&d->job, d->qwen_port, FB_DIARY_SLEEP_INSTRUCTION,
                                d->job_target_page) == 0) {
            d->job_task = FB_DIARY_TASK_SLEEP;
        }
    }
}

static void append_to_long_term(FbDiary *d, const char *summary) {
    size_t summary_len = strlen(summary);
    size_t sep_len = d->long_term_len > 0 ? 1 : 0;

    /* Defensive: a checkpoint should keep this from ever actually
       happening, but if a summary alone would overflow, drop the oldest
       content rather than truncate the new (most relevant) summary. */
    if (d->long_term_len + sep_len + summary_len >= sizeof(d->long_term)) {
        size_t keep = sizeof(d->long_term) / 2;
        if (keep > d->long_term_len) keep = d->long_term_len;
        size_t drop = d->long_term_len - keep;
        memmove(d->long_term, d->long_term + drop, keep);
        d->long_term_len = keep;
        sep_len = d->long_term_len > 0 ? 1 : 0;
    }

    if (sep_len) d->long_term[d->long_term_len++] = '\n';
    size_t room = sizeof(d->long_term) - 1 - d->long_term_len;
    if (summary_len > room) summary_len = room;
    memcpy(d->long_term + d->long_term_len, summary, summary_len);
    d->long_term_len += summary_len;
    d->long_term[d->long_term_len] = '\0';
}

void fb_diary_tick(FbDiary *d, unsigned long long now_ms) {
    if (d->page_has_content && now_ms - d->last_activity_ms >= d->idle_close_ms) {
        fb_diary_close_page(d);
    }

    fb_diary_job_tick(&d->job);
    if (d->job.state == FB_DIARY_JOB_DONE) {
        if (d->job_task == FB_DIARY_TASK_SLEEP) {
            append_to_long_term(d, d->job.result);
            save_long_term(d);
            log_sleep_event(d->root_dir, d->job_target_page_index, now_ms);
            d->last_saved_ms = now_ms;
            d->just_saved = 1;
            fb_diary_job_consume(&d->job);
            d->job_task = FB_DIARY_TASK_NONE;

            if (d->long_term_len > d->ltm_checkpoint_chars && d->qwen_port != 0) {
                if (fb_diary_job_start(&d->job, d->qwen_port, FB_DIARY_CHECKPOINT_INSTRUCTION,
                                        d->long_term) == 0) {
                    d->job_task = FB_DIARY_TASK_CHECKPOINT;
                }
            }
        } else if (d->job_task == FB_DIARY_TASK_CHECKPOINT) {
            size_t len = strlen(d->job.result);
            if (len >= sizeof(d->long_term)) len = sizeof(d->long_term) - 1;
            memcpy(d->long_term, d->job.result, len);
            d->long_term_len = len;
            d->long_term[d->long_term_len] = '\0';
            save_long_term(d);
            log_checkpoint_event(d->root_dir, now_ms);
            d->last_saved_ms = now_ms;
            d->just_saved = 1;
            fb_diary_job_consume(&d->job);
            d->job_task = FB_DIARY_TASK_NONE;
        } else {
            fb_diary_job_consume(&d->job);
        }
    } else if (d->job.state == FB_DIARY_JOB_ERROR) {
        /* The raw page is already safely archived on disk either way;
           long-term memory just doesn't get this page's distillation. */
        fb_diary_job_consume(&d->job);
        d->job_task = FB_DIARY_TASK_NONE;
    }
}

void fb_diary_browse(FbDiary *d, int delta) {
    int idx = d->browse_index + delta;
    if (idx < -1) idx = -1;
    if (idx > d->archived_count - 1) idx = d->archived_count - 1;
    d->browse_index = idx;
}

int fb_diary_read_browsed_page(const FbDiary *d, char *out, size_t out_cap) {
    if (d->browse_index < 0 || d->browse_index >= d->archived_count) return 0;

    char path[600];
    page_path(d, d->browse_index, path, sizeof(path));
    size_t len = 0;
    char *text = fb_read_file(path, &len);
    if (!text) return 0;
    snprintf(out, out_cap, "%s", text);
    free(text);
    return 1;
}

int fb_diary_reopen_page(FbDiary *d, int index) {
    if (index < 0 || index >= d->archived_count) return 0;
    if (d->job.state == FB_DIARY_JOB_RUNNING) return 0;

    fb_diary_close_page(d); /* preserves whatever's currently open, untouched, as its own page */

    char path[600];
    page_path(d, index, path, sizeof(path));
    size_t len = 0;
    char *text = fb_read_file(path, &len);
    if (!text) return 0;
    if (len >= sizeof(d->page_buf)) len = sizeof(d->page_buf) - 1;
    memcpy(d->page_buf, text, len);
    d->page_buf[len] = '\0';
    d->page_len = len;
    free(text);

    d->page_has_content = 1;
    d->edit_target = index;
    d->browse_index = -1;
    return 1;
}

int fb_diary_busy(const FbDiary *d) { return d->job.state == FB_DIARY_JOB_RUNNING; }

void fb_diary_shutdown(FbDiary *d) { fb_diary_job_shutdown(&d->job); }
