#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio/wav_stream.h"
#include "meeting/meeting_document.h"
#include "meeting/meeting_job.h"
#include "platform/platform.h"
#include "util/paths.h"
#include "util/fileutil.h"

static void await_revision(FbMeetingJob *job, unsigned long long *revision, int utterances) {
    unsigned long long limit = fb_now_ms() + 5000;
    for (;;) {
        cJSON *doc = fb_meeting_job_snapshot(job, revision);
        if (doc) {
            int count = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(doc, "utterances"));
            cJSON_Delete(doc);
            if (count >= utterances) {
                return;
            }
        }
        assert(fb_now_ms() < limit);
        fb_sleep_ms(5);
    }
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(fb_ensure_dir("test-stream-state") == 0);
    FbWavStream wav;
    assert(fb_wav_stream_open(&wav, "test-stream-state/audio.wav", 16000) == 0);
    float *pcm = calloc(320000, sizeof(float));
    assert(pcm);
    FbMeetingConfig config = {0};
    snprintf(config.parakeet_library, sizeof(config.parakeet_library), "%s", argv[1]);
    snprintf(config.asr_model, sizeof(config.asr_model), "fixture");
    snprintf(config.diar_model, sizeof(config.diar_model), "fixture");
    FbMeetingJob *job = fb_meeting_job_start(FB_MEETING_JOB_LIVE, &config, "test-stream-state", NULL, NULL, NULL, 0, NULL);
    assert(job);
    unsigned long long revision = 0;
    assert(fb_wav_stream_append(&wav, pcm, 320000) == 0);
    await_revision(job, &revision, 1);
    assert(fb_wav_stream_close(&wav) == 0);
    fb_meeting_job_audio_boundary(job, 0);
    FbMeetingJobStatus status;
    fb_meeting_job_poll(job, &status);
    assert(status.state == FB_MEETING_JOB_RUNNING);
    assert(fb_wav_stream_resume(&wav, "test-stream-state/audio.wav") == 0);
    assert(fb_wav_stream_append(&wav, pcm, 320000) == 0);
    await_revision(job, &revision, 2);
    assert(fb_wav_stream_append(&wav, pcm, 80000) == 0);
    assert(fb_wav_stream_close(&wav) == 0);
    fb_meeting_job_audio_boundary(job, 1);
    fb_meeting_job_wait(job);
    fb_meeting_job_poll(job, &status);
    assert(status.state == FB_MEETING_JOB_DONE);
    cJSON *doc = fb_meeting_job_take_document(job);
    assert(fb_meeting_document_valid(doc));
    cJSON *utterances = cJSON_GetObjectItemCaseSensitive(doc, "utterances");
    assert(cJSON_GetArraySize(utterances) == 3);
    assert(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(utterances, 2), "speaker")->valueint == 1);
    assert(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(utterances, 2), "end")->valuedouble == 45);
    fb_meeting_job_free(job);
    assert(fb_meeting_document_name(doc, 1, "Alice", status.message, sizeof(status.message)) == 0);
    job = fb_meeting_job_start(FB_MEETING_JOB_TRANSCRIBE_PREVIEW, &config, "test-stream-state", NULL, NULL, doc, 0, NULL);
    assert(job);
    fb_meeting_job_wait(job);
    fb_meeting_job_poll(job, &status);
    assert(status.state == FB_MEETING_JOB_DONE);
    cJSON *retried = fb_meeting_job_take_document(job);
    char *text = fb_meeting_document_text(retried, 0);
    assert(text && strstr(text, "Alice:"));
    free(text);
    char *backup = fb_read_file("test-stream-state/transcript-previous.json", NULL);
    assert(backup && strstr(backup, "Alice"));
    free(backup);
    cJSON_Delete(retried);
    cJSON_Delete(doc);
    fb_meeting_job_free(job);
    snprintf(config.asr_model, sizeof(config.asr_model), "badload");
    job = fb_meeting_job_start(FB_MEETING_JOB_TRANSCRIBE_PREVIEW, &config, "test-stream-state", NULL, NULL, NULL, 0, NULL);
    assert(job);
    fb_meeting_job_wait(job);
    fb_meeting_job_poll(job, &status);
    assert(status.state == FB_MEETING_JOB_ERROR && strstr(status.message, "load failure"));
    fb_meeting_job_free(job);
    free(pcm);
    return 0;
}
