#ifndef FB_LAYOUT_H
#define FB_LAYOUT_H

#include "tui/screen.h"

typedef struct {
    int x, y, w, h;
} FbRect;

typedef struct {
    FbRect header;
    FbRect main_area;
    FbRect statusbar;
    FbRect hints; /* F1-F5 shortcut legend, directly above cmdline */
    FbRect cmdline;
} FbLayout;

/* Splits a screen_w x screen_h grid into fixed-height header/statusbar/
   hints/cmdline strips and a flexible main area between them. Degrades
   gracefully (zero-height regions) on very small terminals rather than
   producing negative sizes. */
void fb_layout_compute(int screen_w, int screen_h, FbLayout *out);

/* Drawing primitives shared by every widget. All clip silently to the
   screen bounds via fb_screen_set_cell. */
void fb_draw_text(int x, int y, const char *text, uint8_t fg_r, uint8_t fg_g,
                   uint8_t fg_b, uint8_t bg_r, uint8_t bg_g, uint8_t bg_b);
/* Same, with an explicit bold flag (0 or 1). */
void fb_draw_text_styled(int x, int y, const char *text, uint8_t fg_r, uint8_t fg_g,
                          uint8_t fg_b, uint8_t bg_r, uint8_t bg_g, uint8_t bg_b, int bold);
void fb_draw_hline(int x, int y, int w, uint32_t ch, uint8_t r, uint8_t g, uint8_t b);
void fb_fill_rect(FbRect rect, uint8_t bg_r, uint8_t bg_g, uint8_t bg_b);

#endif /* FB_LAYOUT_H */
