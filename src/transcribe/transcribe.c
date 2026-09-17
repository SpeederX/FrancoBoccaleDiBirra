#include "transcribe/transcribe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "util/fileutil.h"
#include "util/paths.h"

int fb_transcribe_start(FbTranscribeJob *job, const FbTranscribeConfig *cfg,
                         const char *wav_path, const char *log_dir) {
    memset(job, 0, sizeof(*job));

    char transcribe_dir[600];
    snprintf(transcribe_dir, sizeof(transcribe_dir), "%s/transcribe", log_dir);
    fb_ensure_dir(transcribe_dir);
    snprintf(job->log_path, sizeof(job->log_path), "%s/last.stderr.log", transcribe_dir);
    snprintf(job->stdout_path, sizeof(job->stdout_path), "%s/last.stdout.log", transcribe_dir);

    char tokens_arg[600], encoder_arg[600], decoder_arg[600], joiner_arg[600];
    snprintf(tokens_arg, sizeof(tokens_arg), "--tokens=%s", cfg->tokens_path);
    snprintf(encoder_arg, sizeof(encoder_arg), "--encoder=%s", cfg->encoder_path);
    snprintf(decoder_arg, sizeof(decoder_arg), "--decoder=%s", cfg->decoder_path);
    snprintf(joiner_arg, sizeof(joiner_arg), "--joiner=%s", cfg->joiner_path);

    const char *argv[] = {
        cfg->sherpa_exe,
        tokens_arg,
        encoder_arg,
        decoder_arg,
        joiner_arg,
        "--num-threads=4",
        "--decoding-method=greedy_search",
        wav_path,
        NULL,
    };

    job->proc = fb_proc_spawn(argv, job->stdout_path, job->log_path);
    if (!job->proc) {
        job->state = FB_TRANSCRIBE_ERROR;
        return -1;
    }
    job->state = FB_TRANSCRIBE_RUNNING;
    return 0;
}

/* sherpa-onnx-offline's stdout is just the JSON result line(s); scan by
   line anyway rather than assuming a single line, since that's cheap and
   robust to incidental blank lines. */
static int extract_text(const char *stdout_text, char *out, size_t out_cap) {
    const char *line = stdout_text;
    while (line && *line) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);

        const char *p = line;
        size_t l = len;
        while (l > 0 && (*p == ' ' || *p == '\t' || *p == '\r')) { p++; l--; }

        if (l > 0 && p[0] == '{') {
            /* Parsed directly from the line's bounds (cJSON_ParseWithLength,
               not cJSON_Parse) rather than copied into a fixed-size buffer
               first: the token/timestamp/log-prob arrays scale with the
               recording's length, so a real line can run to several KB —
               confirmed the hard way at 3778 bytes for one ~30s clip. */
            cJSON *root = cJSON_ParseWithLength(p, l);
            if (root) {
                cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
                if (cJSON_IsString(text)) {
                    snprintf(out, out_cap, "%s", text->valuestring);
                    cJSON_Delete(root);
                    return 1;
                }
                cJSON_Delete(root);
            }
        }

        line = nl ? nl + 1 : NULL;
    }
    return 0;
}

void fb_transcribe_tick(FbTranscribeJob *job) {
    if (job->state != FB_TRANSCRIBE_RUNNING) return;
    if (!fb_proc_has_exited(job->proc)) return;

    fb_proc_free(job->proc);
    job->proc = NULL;

    char *stdout_text = fb_read_file(job->stdout_path, NULL);
    int found = stdout_text && extract_text(stdout_text, job->result_text, sizeof(job->result_text));
    free(stdout_text);

    job->state = found ? FB_TRANSCRIBE_DONE : FB_TRANSCRIBE_ERROR;
}
