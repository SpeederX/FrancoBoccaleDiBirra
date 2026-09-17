#include "calibr/calibr_log_tail.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

void fb_calibr_log_tail_init(FbCalibrLogTail *tail) {
    tail->byte_offset = 0;
}

static void format_and_add(FbTextView *tv, const char *json_line, size_t len) {
    cJSON *root = cJSON_ParseWithLength(json_line, len);
    if (!root) return;

    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(root, "ts");
    const cJSON *source = cJSON_GetObjectItemCaseSensitive(root, "source");
    const cJSON *flow = cJSON_GetObjectItemCaseSensitive(root, "flow");
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
    int has_message = cJSON_IsString(message);

    char line[FB_TEXTVIEW_LINE_MAX];
    snprintf(line, sizeof(line), "%s %s/%s %s -> %s%s%s",
             cJSON_IsString(ts) ? ts->valuestring : "?",
             cJSON_IsString(source) ? source->valuestring : "?",
             cJSON_IsString(flow) ? flow->valuestring : "?",
             cJSON_IsString(action) ? action->valuestring : "?",
             cJSON_IsString(status) ? status->valuestring : "?", has_message ? ": " : "",
             has_message ? message->valuestring : "");
    fb_textview_add_line(tv, line);
    cJSON_Delete(root);
}

void fb_calibr_log_tail_poll(FbCalibrLogTail *tail, const char *calibr_root, FbTextView *tv) {
    if (!calibr_root || calibr_root[0] == '\0') return;

    char path[600];
    snprintf(path, sizeof(path), "%s/log/action-trace.jsonl", calibr_root);

    FILE *f = fopen(path, "rb");
    if (!f) return; /* not configured / not created yet: not an error */

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < tail->byte_offset) {
        tail->byte_offset = 0; /* truncated/rotated: start over */
    }

    if (size > tail->byte_offset) {
        fseek(f, tail->byte_offset, SEEK_SET);
        long to_read = size - tail->byte_offset;
        char *buf = (char *)malloc((size_t)to_read + 1);
        if (buf) {
            size_t read_n = fread(buf, 1, (size_t)to_read, f);
            buf[read_n] = '\0';

            const char *line = buf;
            const char *last_complete_end = buf;
            while (line && *line) {
                const char *nl = strchr(line, '\n');
                if (!nl) break; /* partial trailing line: wait for more data next poll */
                size_t len = (size_t)(nl - line);
                if (len > 0) format_and_add(tv, line, len);
                line = nl + 1;
                last_complete_end = line;
            }
            tail->byte_offset += (long)(last_complete_end - buf);
            free(buf);
        }
    }

    fclose(f);
}
