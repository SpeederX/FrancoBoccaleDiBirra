#ifndef FB_MEETING_JOB_H
#define FB_MEETING_JOB_H

#include "cJSON.h"

typedef struct {
    char parakeet_exe[512];
    char asr_model[512];
    char diar_model[512];
    char ffmpeg_exe[512];
    char parakeet_library[512];
    char tts_python[512];
    char tts_script[512];
    char tts_model[512];
} FbMeetingConfig;

typedef enum {
    FB_MEETING_JOB_TRANSCRIBE,
    FB_MEETING_JOB_PROCESS,
    FB_MEETING_JOB_SAVE,
    FB_MEETING_JOB_OPEN,
    FB_MEETING_JOB_LIVE,
    FB_MEETING_JOB_TRANSCRIBE_PREVIEW,
    FB_MEETING_JOB_IDENTIFY,
    FB_MEETING_JOB_READ
} FbMeetingOperation;

typedef enum {
    FB_MEETING_JOB_RUNNING,
    FB_MEETING_JOB_DONE,
    FB_MEETING_JOB_ERROR,
    FB_MEETING_JOB_CANCELLED
} FbMeetingJobState;

typedef struct {
    FbMeetingJobState state;
    char message[256];
    int completed_parts;
    int total_parts;
    unsigned long long revision;
} FbMeetingJobStatus;

typedef struct FbMeetingJob FbMeetingJob;

FbMeetingJob *fb_meeting_job_start(FbMeetingOperation operation, const FbMeetingConfig *config,
                                  const char *directory, const char *source, const char *notes,
                                  const cJSON *document, int llm_port, const char *llm_model);
void fb_meeting_job_poll(FbMeetingJob *job, FbMeetingJobStatus *status);
void fb_meeting_job_cancel(FbMeetingJob *job);
void fb_meeting_job_audio_boundary(FbMeetingJob *job, int finished);
cJSON *fb_meeting_job_snapshot(FbMeetingJob *job, unsigned long long *revision);
void fb_meeting_job_wait(FbMeetingJob *job);
cJSON *fb_meeting_job_take_document(FbMeetingJob *job);
char *fb_meeting_job_take_notes(FbMeetingJob *job);
void fb_meeting_job_free(FbMeetingJob *job);
int fb_meeting_save_document(const char *directory, const cJSON *document);

#endif
