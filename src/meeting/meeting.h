#ifndef FB_MEETING_H
#define FB_MEETING_H

#include "audio/system_capture.h"
#include "audio/device_selection.h"
#include "config/config.h"
#include "meeting/meeting_job.h"
#include "tui/widget_cmdline.h"
#include "tui/widget_notepad.h"
#include "tui/widget_textview.h"

typedef struct {
    FbNotepad notes;
    FbTextView output;
    FbConfig *config;
    FbMeetingConfig tools;
    char root[600];
    char directory[600];
    char opening_directory[600];
    char status[256];
    cJSON *document;
    FbMeetingJob *job;
    FbMeetingJob *asr_job;
    FbMeetingJob *read_job;
    unsigned long long asr_revision;
    int asr_live;
    int paused;
    int finish_pending;
    int process_pending;
    int recording_finished;
    int identity_done;
    int identifying_total;
    size_t processing_revision;
    FbCaptureSource source;
    FbAudioDeviceJob *devices_job;
    FbCaptureSource selecting_device;
    int settings_dirty;
    FbMeetingOperation operation;
    FbMeetingJob *save_job;
    size_t saving_revision;
    size_t saved_revision;
    unsigned long long edited_ms;
    FbSystemCapture *capture;
    FbCaptureStatus capture_status;
    int command_focus;
    int output_kind;
    char *report;
    char *output_text;
    int output_width;
    int output_height;
    int output_follow;
    size_t output_offsets[FB_TEXTVIEW_MAX_LINES];
    int llm_port;
    char llm_model[128];
    int document_dirty;
    int saving_document;
    int save_failed;
} FbMeeting;

void fb_meeting_init(FbMeeting *meeting, FbConfig *config);
int fb_meeting_command(FbMeeting *meeting, const char *command);
int fb_meeting_handle_event(FbMeeting *meeting, const FbEvent *event);
void fb_meeting_tick(FbMeeting *meeting);
void fb_meeting_render(FbMeeting *meeting, FbLayout layout, const FbCmdline *command);
void fb_meeting_shutdown(FbMeeting *meeting);

#endif
