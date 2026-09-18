/*
 * POSIX implementation of tui/screen.h: a thin wrapper over termbox2.
 * Only ever compiled on non-Windows targets (see CMakeLists.txt) — never
 * compiled alongside src/platform/win/console_win.c.
 *
 * Not yet runtime-tested (development happened on Windows this pass); it
 * must be exercised on real Linux/macOS terminals before being relied on.
 */

#define TB_IMPL
#define TB_OPT_ATTR_W 32 /* required for TB_OUTPUT_TRUECOLOR */
#include "termbox2.h"

#include "tui/screen.h"

static uintattr_t pack_color(uint8_t r, uint8_t g, uint8_t b, int bold) {
    uintattr_t color = ((uintattr_t)r << 16) | ((uintattr_t)g << 8) | (uintattr_t)b;
    /* termbox2: 0x000000 means "terminal default", not black. */
    if (color == 0) color = TB_HI_BLACK;
    if (bold) color |= TB_BOLD;
    return color;
}

int fb_screen_init(void) {
    if (tb_init() != 0) return -1;
    tb_set_output_mode(TB_OUTPUT_TRUECOLOR);
    return 0;
}

void fb_screen_shutdown(void) { tb_shutdown(); }

int fb_screen_width(void) { return tb_width(); }
int fb_screen_height(void) { return tb_height(); }

void fb_screen_clear(void) { tb_clear(); }

void fb_screen_set_cell(int x, int y, const FbCell *cell) {
    uintattr_t fg = pack_color(cell->fg_r, cell->fg_g, cell->fg_b, cell->bold);
    uintattr_t bg = pack_color(cell->bg_r, cell->bg_g, cell->bg_b, 0);
    tb_set_cell(x, y, cell->ch ? cell->ch : ' ', fg, bg);
}

void fb_screen_present(void) { tb_present(); }

static FbKeyCode map_key(const struct tb_event *ev, uint32_t *out_ch) {
    *out_ch = 0;
    if (ev->key == TB_KEY_CTRL_C) return FB_KEY_CTRL_C;
    if (ev->key == TB_KEY_CTRL_TILDE) return FB_KEY_CTRL_SPACE; /* Ctrl+Space convention */
    if (ev->key == TB_KEY_ENTER) return FB_KEY_ENTER;
    if (ev->key == TB_KEY_ESC) return FB_KEY_ESC;
    if (ev->key == TB_KEY_BACKSPACE || ev->key == TB_KEY_BACKSPACE2) return FB_KEY_BACKSPACE;
    if (ev->key == TB_KEY_TAB) return FB_KEY_TAB;
    if (ev->key == TB_KEY_ARROW_UP) return FB_KEY_ARROW_UP;
    if (ev->key == TB_KEY_ARROW_DOWN) return FB_KEY_ARROW_DOWN;
    if ((ev->mod & TB_MOD_ALT) && ev->key == TB_KEY_ARROW_LEFT) return FB_KEY_ALT_LEFT;
    if ((ev->mod & TB_MOD_ALT) && ev->key == TB_KEY_ARROW_RIGHT) return FB_KEY_ALT_RIGHT;
    if (ev->key == TB_KEY_ARROW_LEFT) return FB_KEY_ARROW_LEFT;
    if (ev->key == TB_KEY_ARROW_RIGHT) return FB_KEY_ARROW_RIGHT;
    if (ev->key == TB_KEY_F1) return FB_KEY_F1;
    if (ev->key == TB_KEY_F2) return FB_KEY_F2;
    if (ev->key == TB_KEY_F3) return FB_KEY_F3;
    if (ev->key == TB_KEY_F4) return FB_KEY_F4;
    if (ev->key == TB_KEY_F5) return FB_KEY_F5;
    if (ev->key == TB_KEY_F6) return FB_KEY_F6;
    if (ev->key == TB_KEY_F7) return FB_KEY_F7;
    if (ev->key == TB_KEY_F10) return FB_KEY_F10;
    if (ev->key == TB_KEY_PGUP) return FB_KEY_PAGE_UP;
    if (ev->key == TB_KEY_PGDN) return FB_KEY_PAGE_DOWN;
    if (ev->ch != 0) {
        *out_ch = ev->ch;
        return FB_KEY_CHAR;
    }
    return FB_KEY_NONE;
}

int fb_screen_poll_event(FbEvent *out_event, int timeout_ms) {
    struct tb_event ev;
    int rc = tb_peek_event(&ev, timeout_ms);
    if (rc == 0) return 0;
    if (rc < 0) return -1;

    if (ev.type == TB_EVENT_RESIZE) {
        out_event->type = FB_EVENT_RESIZE;
        out_event->width = ev.w;
        out_event->height = ev.h;
        return 1;
    }

    if (ev.type == TB_EVENT_KEY) {
        uint32_t ch = 0;
        FbKeyCode key = map_key(&ev, &ch);
        if (key == FB_KEY_NONE) return 0; /* unrecognized key this poll */
        out_event->type = FB_EVENT_KEY;
        out_event->key = key;
        out_event->ch = ch;
        return 1;
    }

    return 0; /* mouse events: not used in first mile */
}
