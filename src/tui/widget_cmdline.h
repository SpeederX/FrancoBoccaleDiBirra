#ifndef FB_WIDGET_CMDLINE_H
#define FB_WIDGET_CMDLINE_H

#include "tui/layout.h"
#include "tui/screen.h"

/* Must be at least FB_TRANSCRIBE_TEXT_MAX (transcribe.h) since a completed
   transcription is copied straight into this buffer for the user to
   review/edit before submitting. */
#define FB_CMDLINE_MAX 1024

typedef struct {
    char buf[FB_CMDLINE_MAX];
    size_t len;
    size_t cursor; /* insertion point, in [0, len] */
} FbCmdline;

typedef enum {
    FB_CMDLINE_NONE = 0,
    FB_CMDLINE_SUBMIT /* Enter pressed; caller reads FbCmdline.buf before clearing */
} FbCmdlineResult;

void fb_cmdline_init(FbCmdline *cl);

/* Only consumes FB_EVENT_KEY events; ignores everything else. */
FbCmdlineResult fb_cmdline_handle_event(FbCmdline *cl, const FbEvent *ev);

void fb_cmdline_clear(FbCmdline *cl);

/* mode_r/g/b tints the "> " prompt and the cursor block -- the visible cue
   for which input mode is active (brain vs. direct chat; see app.c). */
void fb_cmdline_render(const FbCmdline *cl, FbRect area, uint8_t mode_r, uint8_t mode_g,
                        uint8_t mode_b);

#endif /* FB_WIDGET_CMDLINE_H */
