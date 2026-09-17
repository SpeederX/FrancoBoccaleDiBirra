#ifndef FB_APP_H
#define FB_APP_H

#include "backend/backend_mgr.h"
#include "calibr/calibr_log_tail.h"
#include "config/config.h"
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
    int transcribe_available;
    FbTranscribeConfig transcribe_cfg;
    FbTranscribeJob transcribe_job;

    FbAppView view;
    char calibr_root[512]; /* empty if FB_CALIBR_ROOT isn't set */
    FbTextView leaderboard_view;
    FbTextView log_view;
    FbCalibrLogTail log_tail;

    int qwen_available;
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
