#include "tui/widget_cmdline.h"

#include <string.h>

#include "tui/palette.h"
#include "util/utf8.h"

void fb_cmdline_init(FbCmdline *cl) {
    cl->len = 0;
    cl->cursor = 0;
    cl->buf[0] = '\0';
}

void fb_cmdline_clear(FbCmdline *cl) {
    cl->len = 0;
    cl->cursor = 0;
    cl->buf[0] = '\0';
}

FbCmdlineResult fb_cmdline_handle_event(FbCmdline *cl, const FbEvent *ev) {
    if (ev->type != FB_EVENT_KEY) return FB_CMDLINE_NONE;

    switch (ev->key) {
    case FB_KEY_ENTER:
        return FB_CMDLINE_SUBMIT;
    case FB_KEY_BACKSPACE:
        if (cl->cursor > 0) {
            /* Deletes the whole codepoint ending at the cursor (1-4 bytes),
               not just its last byte -- otherwise erasing an accented
               character leaves a dangling, unrenderable continuation byte
               behind. +1 on the move length also shifts the trailing '\0'. */
            size_t back = fb_utf8_prev_len(cl->buf, cl->cursor);
            memmove(cl->buf + cl->cursor - back, cl->buf + cl->cursor,
                    cl->len - cl->cursor + 1);
            cl->cursor -= back;
            cl->len -= back;
        }
        return FB_CMDLINE_NONE;
    case FB_KEY_ARROW_LEFT:
        if (cl->cursor > 0) cl->cursor -= fb_utf8_prev_len(cl->buf, cl->cursor);
        return FB_CMDLINE_NONE;
    case FB_KEY_ARROW_RIGHT:
        if (cl->cursor < cl->len) {
            uint32_t cp;
            cl->cursor += fb_utf8_decode(cl->buf + cl->cursor, &cp);
        }
        return FB_CMDLINE_NONE;
    case FB_KEY_CHAR:
        if (ev->ch >= 0x20 && ev->ch != 0x7f) {
            char enc[4];
            size_t n = fb_utf8_encode(ev->ch, enc);
            if (cl->len + n < FB_CMDLINE_MAX) {
                memmove(cl->buf + cl->cursor + n, cl->buf + cl->cursor,
                        cl->len - cl->cursor + 1);
                memcpy(cl->buf + cl->cursor, enc, n);
                cl->cursor += n;
                cl->len += n;
            }
        }
        return FB_CMDLINE_NONE;
    default:
        return FB_CMDLINE_NONE;
    }
}

void fb_cmdline_render(const FbCmdline *cl, FbRect area, uint8_t mode_r, uint8_t mode_g,
                        uint8_t mode_b) {
    fb_fill_rect(area, FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);
    if (area.h <= 0) return;

    const char *prompt = "> ";
    int prompt_len = (int)strlen(prompt);
    int text_w = area.w - prompt_len;
    if (text_w <= 0) return;

    size_t len = cl->len;
    size_t cursor = cl->cursor;
    size_t tw = (size_t)text_w;

    /* Row-wrap boundaries must land on codepoint boundaries, never mid-byte
       -- cl->buf is UTF-8, and a row cut through a multi-byte sequence
       would hand fb_draw_text a torn half-character on each side of the
       split. So this walks the buffer decoding codepoints (not bytes) to
       find: which row each visible codepoint belongs to (row_start_byte),
       and which row/column the cursor (a byte offset) lands in. */
    size_t row_start_byte[FB_CMDLINE_MAX + 1];
    size_t n_rows = 1;
    row_start_byte[0] = 0;
    size_t row_cp_start = 0;
    size_t cursor_row = 0, cursor_col = 0;
    int cursor_set = 0;

    size_t byte = 0, cp_idx = 0;
    while (byte < len) {
        if (cp_idx > row_cp_start && (cp_idx - row_cp_start) == tw) {
            row_start_byte[n_rows++] = byte;
            row_cp_start = cp_idx;
        }
        if (byte == cursor) {
            cursor_row = n_rows - 1;
            cursor_col = cp_idx - row_cp_start;
            cursor_set = 1;
        }
        uint32_t cp;
        byte += fb_utf8_decode(cl->buf + byte, &cp);
        cp_idx++;
    }
    if (!cursor_set) {
        /* cursor sits at/after the last codepoint (byte == len == cursor) */
        if (cp_idx > row_cp_start && (cp_idx - row_cp_start) == tw) {
            row_start_byte[n_rows++] = byte;
            row_cp_start = cp_idx;
        }
        cursor_row = n_rows - 1;
        cursor_col = cp_idx - row_cp_start;
    }
    row_start_byte[n_rows] = len;

    size_t visible_rows = (size_t)area.h;
    size_t scroll_row = 0;
    if (n_rows > visible_rows && cursor_row + 1 > visible_rows) {
        scroll_row = cursor_row + 1 - visible_rows;
    }

    for (size_t r = 0; r < visible_rows; ++r) {
        size_t buf_row = scroll_row + r;
        int y = area.y + (int)r;

        /* The "> " prompt always marks the widget's top visible row, even
           when scrolled past the buffer's first line, so it stays a stable
           landmark rather than scrolling away with old text. */
        const char *margin = (r == 0) ? prompt : "  ";
        fb_draw_text(area.x, y, margin, mode_r, mode_g, mode_b, FB_COLOR_BG_R, FB_COLOR_BG_G,
                     FB_COLOR_BG_B);

        if (buf_row < n_rows) {
            size_t start = row_start_byte[buf_row];
            size_t end = row_start_byte[buf_row + 1];

            char row_buf[FB_CMDLINE_MAX];
            size_t row_len = end - start;
            memcpy(row_buf, cl->buf + start, row_len);
            row_buf[row_len] = '\0';

            fb_draw_text(area.x + prompt_len, y, row_buf, FB_COLOR_FG_R,
                         FB_COLOR_FG_G, FB_COLOR_FG_B, FB_COLOR_BG_R,
                         FB_COLOR_BG_G, FB_COLOR_BG_B);
        }

        if (buf_row == cursor_row) {
            uint32_t under_cp = ' ';
            if (cursor < len) fb_utf8_decode(cl->buf + cursor, &under_cp);

            char enc[5];
            size_t n = fb_utf8_encode(under_cp, enc);
            enc[n] = '\0';

            /* Mode-colored block cursor: the character underneath renders
               in the dark background color, on a fill of the mode color. */
            fb_draw_text(area.x + prompt_len + (int)cursor_col, y, enc,
                         FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B, mode_r, mode_g, mode_b);
        }
    }
}
