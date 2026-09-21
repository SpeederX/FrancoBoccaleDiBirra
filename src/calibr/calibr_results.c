#include "calibr/calibr_results.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "platform/dirlist.h"
#include "util/fileutil.h"

#define FB_CALIBR_MAX_RECURSE_DEPTH 4
#define FB_CALIBR_MAX_RUN_FILES 1024

static void format_number(double v, char *out, size_t cap) {
    if (v == (double)(long long)v) {
        snprintf(out, cap, "%lld", (long long)v);
    } else {
        snprintf(out, cap, "%.3f", v);
    }
}

static void render_json_value(FbTextView *tv, const char *key, const cJSON *item, int indent) {
    if (indent > FB_CALIBR_MAX_RECURSE_DEPTH) return;

    char line[FB_TEXTVIEW_LINE_MAX];
    char prefix[32];
    int pad = indent * 2;
    if (pad > (int)sizeof(prefix) - 1) pad = (int)sizeof(prefix) - 1;
    memset(prefix, ' ', (size_t)pad);
    prefix[pad] = '\0';

    if (cJSON_IsObject(item)) {
        if (key) {
            snprintf(line, sizeof(line), "%s%s:", prefix, key);
            fb_textview_add_line(tv, line);
        }
        const cJSON *child;
        cJSON_ArrayForEach(child, item) {
            render_json_value(tv, child->string, child, key ? indent + 1 : indent);
        }
        return;
    }

    if (cJSON_IsArray(item)) {
        int n = cJSON_GetArraySize(item);
        snprintf(line, sizeof(line), "%s%s: [%d item%s]", prefix, key ? key : "", n,
                 n == 1 ? "" : "s");
        fb_textview_add_line(tv, line);
        return;
    }

    if (cJSON_IsString(item)) {
        snprintf(line, sizeof(line), "%s%s: %s", prefix, key ? key : "", item->valuestring);
    } else if (cJSON_IsNumber(item)) {
        char num[64];
        format_number(item->valuedouble, num, sizeof(num));
        snprintf(line, sizeof(line), "%s%s: %s", prefix, key ? key : "", num);
    } else if (cJSON_IsBool(item)) {
        snprintf(line, sizeof(line), "%s%s: %s", prefix, key ? key : "",
                 cJSON_IsTrue(item) ? "true" : "false");
    } else if (cJSON_IsNull(item)) {
        snprintf(line, sizeof(line), "%s%s: null", prefix, key ? key : "");
    } else {
        return;
    }
    fb_textview_add_line(tv, line);
}

void fb_calibr_load_leaderboard(FbTextView *tv, const char *calibr_root) {
    fb_textview_clear(tv);

    if (!calibr_root || calibr_root[0] == '\0') {
        fb_textview_add_line(tv, "calibr not configured -- set calibr_root in config.json to your calibr checkout");
        return;
    }

    char rec_path[600];
    snprintf(rec_path, sizeof(rec_path), "%s/results/current/recommendation.json", calibr_root);

    char *text = fb_read_file(rec_path, NULL);
    if (!text) {
        fb_textview_add_line(tv, "no recommendation.json yet -- run a calibr benchmark first");
    } else {
        cJSON *root = cJSON_Parse(text);
        free(text);
        if (!root) {
            fb_textview_add_line(tv, "recommendation.json exists but failed to parse");
        } else {
            render_json_value(tv, NULL, root, 0);
            cJSON_Delete(root);
        }
    }

    fb_textview_add_line(tv, "");

    char runs_dir[600];
    snprintf(runs_dir, sizeof(runs_dir), "%s/results/runs", calibr_root);

    /* Heap, not a stack array: FB_CALIBR_MAX_RUN_FILES entries of FbDirEntry
       run to ~270KB, and a large local like that stacked on top of an
       already-deep call chain is exactly the kind of thing that silently
       overflows a 1MB thread stack (confirmed the hard way with this same
       pattern in this file's own unit test). */
    FbDirEntry *entries = (FbDirEntry *)malloc(FB_CALIBR_MAX_RUN_FILES * sizeof(FbDirEntry));
    int n = entries ? fb_list_dir(runs_dir, entries, FB_CALIBR_MAX_RUN_FILES) : -1;
    if (n < 0) {
        fb_textview_add_line(tv, "raw results: results/runs/ not found yet");
    } else {
        int json_count = 0;
        for (int i = 0; i < n; ++i) {
            if (!entries[i].is_dir && strstr(entries[i].name, ".json")) json_count++;
        }
        char header[64];
        snprintf(header, sizeof(header), "raw results (%d files):", json_count);
        fb_textview_add_line(tv, header);
        for (int i = 0; i < n; ++i) {
            if (entries[i].is_dir || !strstr(entries[i].name, ".json")) continue;
            char line[FB_TEXTVIEW_LINE_MAX];
            snprintf(line, sizeof(line), "  %s", entries[i].name);
            fb_textview_add_line(tv, line);
        }
    }
    free(entries);
}
