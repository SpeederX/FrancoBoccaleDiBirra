#ifndef FB_WIDGET_CMDLINE_H
#define FB_WIDGET_CMDLINE_H

#include "tui/layout.h"
#include "tui/screen.h"

#define FB_CMDLINE_MAX 256

typedef struct {
    char buf[FB_CMDLINE_MAX];
    size_t len;
} FbCmdline;

typedef enum {
    FB_CMDLINE_NONE = 0,
    FB_CMDLINE_SUBMIT /* Enter pressed; caller reads FbCmdline.buf before clearing */
} FbCmdlineResult;

void fb_cmdline_init(FbCmdline *cl);

/* Only consumes FB_EVENT_KEY events; ignores everything else. */
FbCmdlineResult fb_cmdline_handle_event(FbCmdline *cl, const FbEvent *ev);

void fb_cmdline_clear(FbCmdline *cl);

void fb_cmdline_render(const FbCmdline *cl, FbRect area);

#endif /* FB_WIDGET_CMDLINE_H */
