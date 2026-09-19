#ifndef FB_APP_H
#define FB_APP_H

#include "backend/backend_mgr.h"
#include "calibr/calibr_log_tail.h"
#include "chat/chat_job.h"
#include "config/config.h"
#include "diary/diary.h"
#include "project/project.h"
#include "transcribe/transcribe.h"
#include "tui/screen.h"
#include "tui/widget_cmdline.h"
#include "tui/widget_file_picker.h"
#include "tui/widget_textview.h"
#include "util/version.h"

typedef enum {
    FB_VIEW_DASHBOARD = 0,
    FB_VIEW_LEADERBOARD,
    FB_VIEW_LOGS
} FbAppView;

/* Four input modes; PROJECT is currently an empty box (see project.h) --
   selectable and visually distinct (its own color/effect/mode tag) but
   with no actual feature behind it yet. Built out on its own worktree/
   branch (feature/project-mode) specifically so that work never touches
   diary/*, chat/*, or app/sweep.c, and concurrent diary/memory work never
   touches project/* -- the two can proceed in parallel without either
   side's changes landing on the same lines. */
typedef enum {
    FB_INPUT_MODE_COMMAND = 0, /* free text -> Qwen intent classify -> a command */
    FB_INPUT_MODE_SCRATCHPAD,  /* free text -> sent directly to the loaded model, bounded memory */
    FB_INPUT_MODE_DIARY,       /* free text -> loaded model, plus fed into diary/diary.h's memory pipeline */
    FB_INPUT_MODE_PROJECT      /* empty box -- see project/project.h */
} FbInputMode;

#define FB_WAVE_HISTORY 256

typedef struct {
    int running;
    int width, height;
    FbCmdline cmdline;
    char last_submitted[FB_CMDLINE_MAX];
    unsigned long long start_ms;
    float wave_samples[FB_WAVE_HISTORY];
    float sphere_intensity; /* 0 = idle, 1 = actively recording */
    int mic_available;
    int recording;
    char data_dir[512];
    FbBackendMgr backend_mgr;
    FbConfig config;
    char llama_server_path[FB_BACKEND_PATH_MAX];
    FbFilePicker file_picker;
    char file_picker_pending_name[FB_BACKEND_NAME_MAX];
    int file_picker_pending_load; /* F2 shortcut with nothing registered yet: add, then load right away */
    int transcribe_available;
    FbTranscribeConfig transcribe_cfg;
    FbTranscribeJob transcribe_job;
    int transcribe_confirm_pending; /* awaiting a y/N reply after all retry attempts failed */

    FbAppView view;
    char calibr_root[512]; /* empty if FB_CALIBR_ROOT isn't set */
    FbTextView leaderboard_view;
    FbTextView log_view;
    FbCalibrLogTail log_tail;

    int qwen_available;
    /* Each mode gets its own right-side panel -- they're genuinely separate
       conversations (separate chat_job/diary_chat_job turn histories under
       the hood), so showing them interleaved in one shared log would be
       actively misleading about what context the model actually has. */
    FbTextView history_view; /* command mode: "You: ..." / "Qwen: ..." */

    FbInputMode input_mode; /* toggled with Tab outside /load and /unload completion */
    FbChatJob chat_job;
    FbTextView scratchpad_history_view;
    /* Name of the model a chat request was sent to, cached at request time
       -- the "most recently loaded" target can change while a reply is
       still in flight, but the reply belongs to whichever model actually
       received it. */
    char chat_target_name[FB_BACKEND_NAME_MAX];

    FbDiary diary;
    FbChatJob diary_chat_job; /* separate from chat_job: diary's own live conversation */
    char diary_chat_target_name[FB_BACKEND_NAME_MAX];
    FbTextView diary_live_view; /* diary mode, browse_index == -1 */
    /* Rebuilt whenever Alt-Left/Alt-Right changes diary.browse_index;
       rendered instead of diary_live_view while browse_index >= 0. */
    FbTextView diary_browse_view;
    int diary_reopen_confirm_pending; /* awaiting a y/N reply to reopen the browsed archived page */
    int scratchpad_new_page_confirm_pending; /* awaiting a y/N/a reply to /new_page in scratchpad */

    FbProject project;        /* empty box -- see project/project.h */
    FbTextView project_view;  /* placeholder panel, set once at init */
} FbApp;

/* Shifts the wave history left and appends one new amplitude sample [0,1]. */
void fb_app_push_wave_sample(FbApp *app, float amplitude);

void fb_app_shutdown(FbApp *app);

void fb_app_init(FbApp *app, int width, int height);
/* Non-rendering per-frame state update (backend health polling, etc.);
   call once per frame before fb_app_render. */
void fb_app_tick(FbApp *app);
/* Returns 0 normally; non-zero means the app wants to quit. */
int fb_app_handle_event(FbApp *app, const FbEvent *ev);
void fb_app_render(FbApp *app);

#endif /* FB_APP_H */
