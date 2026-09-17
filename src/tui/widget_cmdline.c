#include "tui/widget_cmdline.h"

#include <string.h>

#include "tui/palette.h"

void fb_cmdline_init(FbCmdline *cl) {
    cl->len = 0;
    cl->buf[0] = '\0';
}

void fb_cmdline_clear(FbCmdline *cl) {
    cl->len = 0;
    cl->buf[0] = '\0';
}

FbCmdlineResult fb_cmdline_handle_event(FbCmdline *cl, const FbEvent *ev) {
    if (ev->type != FB_EVENT_KEY) return FB_CMDLINE_NONE;

    switch (ev->key) {
    case FB_KEY_ENTER:
        return FB_CMDLINE_SUBMIT;
    case FB_KEY_BACKSPACE:
        if (cl->len > 0) {
            cl->len--;
            cl->buf[cl->len] = '\0';
        }
        return FB_CMDLINE_NONE;
    case FB_KEY_CHAR:
        if (cl->len + 1 < FB_CMDLINE_MAX && ev->ch >= 0x20 && ev->ch < 0x7f) {
            cl->buf[cl->len++] = (char)ev->ch;
            cl->buf[cl->len] = '\0';
        }
        return FB_CMDLINE_NONE;
    default:
        return FB_CMDLINE_NONE;
    }
}

void fb_cmdline_render(const FbCmdline *cl, FbRect area) {
    fb_fill_rect(area, FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);

    const char *prompt = "> ";
    size_t prompt_len = strlen(prompt);
    fb_draw_text(area.x, area.y, prompt, FB_COLOR_ACCENT_R, FB_COLOR_ACCENT_G,
                 FB_COLOR_ACCENT_B, FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);

    int text_w = area.w - (int)prompt_len;
    if (text_w <= 0) return;

    /* Show the tail of the buffer when it doesn't fit, so the cursor
       (implicitly at the end, since there's no mid-line editing yet)
       stays visible. */
    const char *visible = cl->buf;
    size_t visible_len = cl->len;
    if (visible_len > (size_t)text_w) {
        visible += visible_len - (size_t)text_w;
    }

    fb_draw_text(area.x + (int)prompt_len, area.y, visible, FB_COLOR_FG_R,
                 FB_COLOR_FG_G, FB_COLOR_FG_B, FB_COLOR_BG_R, FB_COLOR_BG_G,
                 FB_COLOR_BG_B);
}
