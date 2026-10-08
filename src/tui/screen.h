#ifndef FB_SCREEN_H
#define FB_SCREEN_H

/*
 * Terminal grid interface. Exactly one implementation is compiled in by
 * CMake (never both, never picked by #ifdef in this file or in callers):
 *   - src/platform/win/console_win.c   on Windows (termbox2 has no Win32
 *                                       backend at all, so this is a
 *                                       hand-written minimal replacement)
 *   - src/tui/backend_termbox2.c        on POSIX (thin termbox2 wrapper)
 *
 * Every widget/effect renders through these functions only. Nothing above
 * this boundary may call a termbox or Win32 API directly, and nothing above
 * it should need to branch on platform.
 */

#include <stdint.h>

typedef struct {
    uint32_t ch; /* ASCII/Unicode codepoint to draw in this cell */
    uint8_t fg_r, fg_g, fg_b;
    uint8_t bg_r, bg_g, bg_b;
    uint8_t bold;
} FbCell;

typedef enum {
    FB_EVENT_NONE = 0,
    FB_EVENT_KEY,
    FB_EVENT_RESIZE,
    FB_EVENT_QUIT /* external termination request (window close, ctrl handler) */
} FbEventType;

typedef enum {
    FB_KEY_NONE = 0,
    FB_KEY_CHAR, /* printable character, value in FbEvent.ch */
    FB_KEY_ENTER,
    FB_KEY_ESC,
    FB_KEY_BACKSPACE,
    FB_KEY_TAB,
    FB_KEY_ARROW_UP,
    FB_KEY_ARROW_DOWN,
    FB_KEY_ARROW_LEFT,
    FB_KEY_ARROW_RIGHT,
    FB_KEY_CTRL_C,
    FB_KEY_CTRL_SPACE, /* mic record toggle hotkey */
    /* Quick-action shortcuts, mirroring /help /load /unload /add /remove --
       see fb_app_handle_event. */
    FB_KEY_F1,
    FB_KEY_F2,
    FB_KEY_F3,
    FB_KEY_F4,
    FB_KEY_F5,
    FB_KEY_F6, /* /list */
    FB_KEY_F7, /* /new_page (diary mode only) */
    FB_KEY_F8,
    FB_KEY_F9,
    FB_KEY_F10,
    FB_KEY_PAGE_UP,
    FB_KEY_PAGE_DOWN,
    FB_KEY_ALT_LEFT,  /* diary mode: browse to the previous archived page */
    FB_KEY_ALT_RIGHT  /* diary mode: browse to the next archived page */
} FbKeyCode;

typedef struct {
    FbEventType type;
    FbKeyCode key;
    uint32_t ch;    /* valid when key == FB_KEY_CHAR */
    int width, height; /* valid when type == FB_EVENT_RESIZE */
} FbEvent;

/* Returns 0 on success, non-zero on failure. */
int fb_screen_init(void);

/* Restores original terminal state. Safe to call more than once. */
void fb_screen_shutdown(void);

int fb_screen_width(void);
int fb_screen_height(void);

/* Clears the back buffer (does not touch the terminal until fb_screen_present). */
void fb_screen_clear(void);

/* Out-of-range (x, y) is silently ignored. */
void fb_screen_set_cell(int x, int y, const FbCell *cell);

/* Flushes the back buffer to the terminal. */
void fb_screen_present(void);

/*
 * Waits up to timeout_ms for one event. Returns 1 if out_event was filled,
 * 0 on timeout, -1 on error. A resize event also updates what
 * fb_screen_width()/fb_screen_height() report, before returning.
 */
int fb_screen_poll_event(FbEvent *out_event, int timeout_ms);

#endif /* FB_SCREEN_H */
