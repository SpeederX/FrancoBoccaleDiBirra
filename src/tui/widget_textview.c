#include "tui/widget_textview.h"

#include <string.h>

#include "tui/palette.h"

void fb_textview_clear(FbTextView *tv) {
    tv->count = 0;
    tv->scroll = 0;
}

void fb_textview_add_line_styled(FbTextView *tv, const char *line, FbTextViewStyle style) {
    if (tv->count < FB_TEXTVIEW_MAX_LINES) {
        strncpy(tv->lines[tv->count], line, FB_TEXTVIEW_LINE_MAX - 1);
        tv->lines[tv->count][FB_TEXTVIEW_LINE_MAX - 1] = '\0';
        tv->style[tv->count] = (uint8_t)style;
        tv->count++;
    } else {
        for (int i = 1; i < FB_TEXTVIEW_MAX_LINES; ++i) {
            memcpy(tv->lines[i - 1], tv->lines[i], FB_TEXTVIEW_LINE_MAX);
            tv->style[i - 1] = tv->style[i];
        }
        strncpy(tv->lines[FB_TEXTVIEW_MAX_LINES - 1], line, FB_TEXTVIEW_LINE_MAX - 1);
        tv->lines[FB_TEXTVIEW_MAX_LINES - 1][FB_TEXTVIEW_LINE_MAX - 1] = '\0';
        tv->style[FB_TEXTVIEW_MAX_LINES - 1] = (uint8_t)style;
        if (tv->scroll > 0) tv->scroll--; /* keep the view anchored on the same lines */
    }
}

void fb_textview_add_line(FbTextView *tv, const char *line) {
    fb_textview_add_line_styled(tv, line, FB_TEXTVIEW_STYLE_NORMAL);
}

void fb_textview_handle_event(FbTextView *tv, const FbEvent *ev, int visible_rows) {
    if (ev->type != FB_EVENT_KEY) return;
    int max_scroll = tv->count - visible_rows;
    if (max_scroll < 0) max_scroll = 0;

    if (ev->key == FB_KEY_ARROW_UP) {
        if (tv->scroll > 0) tv->scroll--;
    } else if (ev->key == FB_KEY_ARROW_DOWN) {
        if (tv->scroll < max_scroll) tv->scroll++;
    }
}

void fb_textview_render(const FbTextView *tv, FbRect area) {
    fb_fill_rect(area, FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);

    for (int row = 0; row < area.h; ++row) {
        int idx = tv->scroll + row;
        if (idx >= tv->count) break;

        if (tv->style[idx] == FB_TEXTVIEW_STYLE_HEADER) {
            fb_draw_text_styled(area.x, area.y + row, tv->lines[idx], FB_COLOR_ACCENT_R,
                                 FB_COLOR_ACCENT_G, FB_COLOR_ACCENT_B, FB_COLOR_BG_R,
                                 FB_COLOR_BG_G, FB_COLOR_BG_B, 1);
        } else {
            fb_draw_text(area.x, area.y + row, tv->lines[idx], FB_COLOR_FG_R, FB_COLOR_FG_G,
                         FB_COLOR_FG_B, FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);
        }
    }
}
