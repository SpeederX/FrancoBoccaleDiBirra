#ifndef FB_MEETING_H
#define FB_MEETING_H

#include "audio/system_capture.h"
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
