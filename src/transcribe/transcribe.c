#include "transcribe/transcribe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "util/fileutil.h"
#include "util/paths.h"

/* Spawns one attempt against job's already-stashed cfg/wav_path/log_dir.
   Shared by fb_transcribe_start (attempt 1) and fb_transcribe_tick's
   auto-retry (attempt 2+). */
static int start_attempt(FbTranscribeJob *job) {
    char transcribe_dir[600];
    snprintf(transcribe_dir, sizeof(transcribe_dir), "%s/transcribe", job->log_dir);
    fb_ensure_dir(transcribe_dir);
    snprintf(job->log_path, sizeof(job->log_path), "%s/last.stderr.log", transcribe_dir);
    snprintf(job->stdout_path, sizeof(job->stdout_path), "%s/last.stdout.log", transcribe_dir);

    char tokens_arg[600], encoder_arg[600], decoder_arg[600], joiner_arg[600];
    snprintf(tokens_arg, sizeof(tokens_arg), "--tokens=%s", job->cfg.tokens_path);
    snprintf(encoder_arg, sizeof(encoder_arg), "--encoder=%s", job->cfg.encoder_path);
    snprintf(decoder_arg, sizeof(decoder_arg), "--decoder=%s", job->cfg.decoder_path);
    snprintf(joiner_arg, sizeof(joiner_arg), "--joiner=%s", job->cfg.joiner_path);

    const char *argv[] = {
        job->cfg.sherpa_exe,
        tokens_arg,
        encoder_arg,
        decoder_arg,
        joiner_arg,
        /* Confirmed empirically: --num-threads=4 has an intermittent race
           that drops the whole JSON result (clean exit, full stderr
           stats, zero-byte stdout) roughly half the time. --num-threads=1
           was 100% reliable across repeated identical runs but ~3x slower
           to decode; =2 was also 100% reliable in the same testing at a
           much smaller speed cost, so that's the balance struck here. */
        "--num-threads=2",
        "--decoding-method=greedy_search",
        job->wav_path,
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

int fb_transcribe_start(FbTranscribeJob *job, const FbTranscribeConfig *cfg,
                         const char *wav_path, const char *log_dir) {
    memset(job, 0, sizeof(*job));
    job->cfg = *cfg;
    snprintf(job->wav_path, sizeof(job->wav_path), "%s", wav_path);
    snprintf(job->log_dir, sizeof(job->log_dir), "%s", log_dir);
    job->attempt = 1;
    return start_attempt(job);
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

    if (found) {
        job->state = FB_TRANSCRIBE_DONE;
        return;
    }

    if (job->attempt < FB_TRANSCRIBE_MAX_ATTEMPTS) {
        job->attempt++;
        start_attempt(job); /* clean exit + empty stdout is a known sherpa-onnx quirk, not audio-content-specific */
        return;
    }

    job->state = FB_TRANSCRIBE_ERROR;
}
