#ifndef FB_TRANSCRIBE_H
#define FB_TRANSCRIBE_H

#include "config/config.h"
#include "platform/proc.h"

/*
 * Transcription runs sherpa-onnx-offline as a spawned, non-blocking job
 * (same tick-based pattern as backend/backend_mgr.h) rather than a
 * blocking call: each invocation pays a real ~6s model-load cost measured
 * against Orukeet's int8 weights, which would otherwise stall the UI
 * thread's ~30fps render loop for the length of one voice command.
 *
 * Orukeet's generic-transducer decoding path (confirmed via
 * sherpa-onnx-offline --help) has no language-selection flag at all —
 * language hints only exist for other model types (Whisper/Canary/
 * SenseVoice/...). Orukeet auto-detects the spoken language from the audio
 * itself, so there is nothing for this module to pass through for that.
 */

typedef enum {
    FB_TRANSCRIBE_IDLE = 0,
    FB_TRANSCRIBE_RUNNING,
    FB_TRANSCRIBE_DONE,
    FB_TRANSCRIBE_ERROR
} FbTranscribeState;

#define FB_TRANSCRIBE_TEXT_MAX 1024

typedef struct {
    FbTranscribeState state;
    FbProcess *proc;
    char log_path[600]; /* stderr: sherpa-onnx's diagnostics, shown to the user on ERROR */
    char stdout_path[600]; /* stdout: the actual JSON result, parsed internally on DONE */
    char result_text[FB_TRANSCRIBE_TEXT_MAX];
} FbTranscribeJob;

/* Starts transcribing wav_path in the background. log_dir's "transcribe"
   subdirectory is created if needed and holds sherpa-onnx's stdout (its
   JSON result) and stderr (its diagnostics) as two separate files — see
   platform/proc.h for why they must never share one file. Returns 0 if
   the job started, -1 on immediate spawn failure (job->state is left
   IDLE/ERROR accordingly either way, so the caller need not branch on the
   return value beyond deciding whether to start polling). */
int fb_transcribe_start(FbTranscribeJob *job, const FbTranscribeConfig *cfg,
                         const char *wav_path, const char *log_dir);

/* Call once per frame while job->state == FB_TRANSCRIBE_RUNNING; advances
   to DONE (result_text filled) or ERROR (check log_path) once the spawned
   process exits. No-op otherwise. */
void fb_transcribe_tick(FbTranscribeJob *job);

#endif /* FB_TRANSCRIBE_H */
