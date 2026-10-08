#include "tui/widget_notepad.h"

#include <stdlib.h>
#include <string.h>

#include "tui/palette.h"
#include "util/utf8.h"

void fb_notepad_init(FbNotepad *pad) {
    memset(pad, 0, sizeof(*pad));
}

void fb_notepad_free(FbNotepad *pad) {
    free(pad->text);
    fb_notepad_init(pad);
}

static int reserve(FbNotepad *pad, size_t needed) {
    if (needed <= pad->capacity) {
        return 0;
    }
    size_t capacity = pad->capacity ? pad->capacity * 2 : 4096;
    if (capacity < needed) {
        capacity = needed;
    }
    char *text = realloc(pad->text, capacity);
    if (!text) {
        return -1;
    }
    pad->text = text;
    pad->capacity = capacity;
    return 0;
}

int fb_notepad_set(FbNotepad *pad, const char *text) {
    size_t len = strlen(text);
    if (reserve(pad, len + 1) != 0) {
        return -1;
    }
    memcpy(pad->text, text, len + 1);
    pad->len = len;
    pad->cursor = len;
    pad->scroll = 0;
    pad->revision++;
    return 0;
}

static size_t line_start(const FbNotepad *pad, size_t position) {
    while (position > 0 && pad->text[position - 1] != '\n') {
        position--;
    }
    return position;
}

static size_t line_end(const FbNotepad *pad, size_t position) {
    while (position < pad->len && pad->text[position] != '\n') {
        position++;
    }
    return position;
}

static void move_vertical(FbNotepad *pad, int direction) {
    size_t start = line_start(pad, pad->cursor);
    size_t column = 0;
    for (size_t p = start; p < pad->cursor; ++column) {
        uint32_t cp;
        p += fb_utf8_decode(pad->text + p, &cp);
    }
    size_t target;
    if (direction < 0) {
        if (start == 0) {
            return;
        }
        target = line_start(pad, start - 1);
    } else {
        target = line_end(pad, start);
        if (target == pad->len) {
            return;
        }
        target++;
    }
    while (column > 0 && target < pad->len && pad->text[target] != '\n') {
        uint32_t cp;
        target += fb_utf8_decode(pad->text + target, &cp);
        column--;
    }
    pad->cursor = target;
}

int fb_notepad_handle_event(FbNotepad *pad, const FbEvent *event) {
    if (event->type != FB_EVENT_KEY) {
        return 0;
    }
    switch (event->key) {
    case FB_KEY_ARROW_LEFT:
        pad->cursor -= fb_utf8_prev_len(pad->text ? pad->text : "", pad->cursor);
        return 0;
    case FB_KEY_ARROW_RIGHT:
        if (pad->cursor < pad->len) {
            uint32_t cp;
            pad->cursor += fb_utf8_decode(pad->text + pad->cursor, &cp);
        }
        return 0;
    case FB_KEY_ARROW_UP:
        if (pad->len) {
            move_vertical(pad, -1);
        }
        return 0;
    case FB_KEY_ARROW_DOWN:
        if (pad->len) {
            move_vertical(pad, 1);
        }
        return 0;
    case FB_KEY_BACKSPACE:
        if (pad->cursor > 0) {
            size_t count = fb_utf8_prev_len(pad->text, pad->cursor);
            memmove(pad->text + pad->cursor - count, pad->text + pad->cursor,
                    pad->len - pad->cursor + 1);
            pad->cursor -= count;
            pad->len -= count;
            pad->revision++;
            return 1;
        }
        return 0;
    case FB_KEY_ENTER:
    case FB_KEY_CHAR: {
        char bytes[4];
        size_t count;
        if (event->key == FB_KEY_ENTER) {
            bytes[0] = '\n';
            count = 1;
        } else {
            if (event->ch < 32 || event->ch == 127) {
                return 0;
            }
            count = fb_utf8_encode(event->ch, bytes);
        }
        if (reserve(pad, pad->len + count + 1) != 0) {
            return -1;
        }
        if (pad->len == 0) {
            pad->text[0] = '\0';
        }
        memmove(pad->text + pad->cursor + count, pad->text + pad->cursor,
                pad->len - pad->cursor + 1);
        memcpy(pad->text + pad->cursor, bytes, count);
        pad->cursor += count;
        pad->len += count;
        pad->revision++;
        return 1;
    }
    default:
        return 0;
    }
}

static void cursor_position(const FbNotepad *pad, int width, int *row, int *column) {
    *row = 0;
    *column = 0;
    for (size_t p = 0; p < pad->cursor;) {
        uint32_t cp;
        p += fb_utf8_decode(pad->text + p, &cp);
        if (cp == '\n') {
            (*row)++;
            *column = 0;
        } else {
            (*column)++;
            if (*column >= width) {
                (*row)++;
                *column = 0;
            }
        }
    }
}

void fb_notepad_render(FbNotepad *pad, FbRect area, int focused) {
    fb_fill_rect(area, FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);
    if (area.w < 1 || area.h < 1) {
        return;
    }
    int cursor_row, cursor_col;
    cursor_position(pad, area.w, &cursor_row, &cursor_col);
    if (focused) {
        if (cursor_row < pad->scroll) {
            pad->scroll = cursor_row;
        }
        if (cursor_row >= pad->scroll + area.h) {
            pad->scroll = cursor_row - area.h + 1;
        }
    }
    FbCell cell = {' ', 215, 220, 228, FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B, 0};
    int row = 0;
    int column = 0;
    for (size_t p = 0; p < pad->len && row < pad->scroll + area.h;) {
        uint32_t cp;
        p += fb_utf8_decode(pad->text + p, &cp);
        if (cp == '\n') {
            row++;
            column = 0;
            continue;
        }
        if (row >= pad->scroll) {
            cell.ch = cp;
            fb_screen_set_cell(area.x + column, area.y + row - pad->scroll, &cell);
        }
        column++;
        if (column >= area.w) {
            row++;
            column = 0;
        }
    }
    if (focused) {
        cell.ch = ' ';
        if (pad->cursor < pad->len && pad->text[pad->cursor] != '\n') {
            fb_utf8_decode(pad->text + pad->cursor, &cell.ch);
        }
        cell.fg_r = FB_COLOR_BG_R;
        cell.fg_g = FB_COLOR_BG_G;
        cell.fg_b = FB_COLOR_BG_B;
        cell.bg_r = 130;
        cell.bg_g = 205;
        cell.bg_b = 185;
        fb_screen_set_cell(area.x + cursor_col, area.y + cursor_row - pad->scroll, &cell);
    }
}
