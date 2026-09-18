#include "tui/layout.h"

#include <string.h>

#include "tui/palette.h"
#include "util/utf8.h"

void fb_layout_compute(int screen_w, int screen_h, FbLayout *out) {
    int header_h = 1;
    int statusbar_h = 1;
    int hints_h = 2; /* row 1: F-key legend + mode tag/nav controls; row 2: mode's live stats */
    /* 3 rows so a long transcribed line wraps and stays editable instead of
       only showing its tail (see widget_cmdline.c). */
    int cmdline_h = 3;
    int reserved = header_h + statusbar_h + hints_h + cmdline_h;
    int main_h = screen_h - reserved;
    if (main_h < 0) main_h = 0;

    out->header = (FbRect){0, 0, screen_w, header_h};
    out->main_area = (FbRect){0, header_h, screen_w, main_h};
    out->statusbar = (FbRect){0, header_h + main_h, screen_w, statusbar_h};
    out->hints = (FbRect){0, header_h + main_h + statusbar_h, screen_w, hints_h};
    out->cmdline = (FbRect){0, header_h + main_h + statusbar_h + hints_h, screen_w, cmdline_h};
}

/* text is UTF-8; each decoded codepoint advances one column, not one byte,
   so multi-byte sequences (accented letters, non-Latin scripts from
   transcription) occupy a single cell instead of being torn into raw bytes
   that render as garbage/blank cells. */
void fb_draw_text(int x, int y, const char *text, uint8_t fg_r, uint8_t fg_g,
                   uint8_t fg_b, uint8_t bg_r, uint8_t bg_g, uint8_t bg_b) {
    fb_draw_text_styled(x, y, text, fg_r, fg_g, fg_b, bg_r, bg_g, bg_b, 0);
}

void fb_draw_text_styled(int x, int y, const char *text, uint8_t fg_r, uint8_t fg_g,
                          uint8_t fg_b, uint8_t bg_r, uint8_t bg_g, uint8_t bg_b, int bold) {
    FbCell cell;
    cell.fg_r = fg_r; cell.fg_g = fg_g; cell.fg_b = fg_b;
    cell.bg_r = bg_r; cell.bg_g = bg_g; cell.bg_b = bg_b;
    cell.bold = (uint8_t)bold;
    int col = 0;
    for (size_t i = 0; text[i] != '\0'; ) {
        uint32_t cp;
        i += fb_utf8_decode(text + i, &cp);
        cell.ch = cp;
        fb_screen_set_cell(x + col, y, &cell);
        col++;
    }
}

void fb_draw_hline(int x, int y, int w, uint32_t ch, uint8_t r, uint8_t g, uint8_t b) {
    FbCell cell;
    cell.ch = ch;
    cell.fg_r = r; cell.fg_g = g; cell.fg_b = b;
    cell.bg_r = FB_COLOR_BG_R; cell.bg_g = FB_COLOR_BG_G; cell.bg_b = FB_COLOR_BG_B;
    cell.bold = 0;
    for (int i = 0; i < w; ++i) fb_screen_set_cell(x + i, y, &cell);
}

void fb_fill_rect(FbRect rect, uint8_t bg_r, uint8_t bg_g, uint8_t bg_b) {
    FbCell cell;
    cell.ch = ' ';
    cell.fg_r = bg_r; cell.fg_g = bg_g; cell.fg_b = bg_b;
    cell.bg_r = bg_r; cell.bg_g = bg_g; cell.bg_b = bg_b;
    cell.bold = 0;
    for (int y = rect.y; y < rect.y + rect.h; ++y)
        for (int x = rect.x; x < rect.x + rect.w; ++x)
            fb_screen_set_cell(x, y, &cell);
}
