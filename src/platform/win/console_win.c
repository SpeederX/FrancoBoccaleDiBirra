/*
 * Windows implementation of tui/screen.h. termbox2 has no Windows backend at
 * all (verified against its source), so this is a real, if minimal, Win32
 * console TUI layer: ReadConsoleInputW for input/resize, VT/SGR escape
 * sequences over WriteConsoleW for truecolor output. The escape sequences
 * themselves are always plain ASCII (safe to widen byte-for-byte); actual
 * glyph cells carry a full Unicode codepoint (FbCell.ch) encoded as one
 * UTF-16 code unit, or a surrogate pair for anything above the BMP.
 */

#include "tui/screen.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HANDLE g_hin = NULL;
static HANDLE g_hout = NULL;
static DWORD g_orig_in_mode = 0;
static DWORD g_orig_out_mode = 0;
static int g_initialized = 0;

static int g_width = 0;
static int g_height = 0;
static FbCell *g_cells = NULL; /* g_width * g_height, row-major */

static wchar_t *g_out_buf = NULL; /* count is in wchar_t units, not bytes */
static size_t g_out_buf_cap = 0;

static volatile LONG g_quit_requested = 0;

static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    switch (ctrl_type) {
    case CTRL_C_EVENT: /* normally suppressed by our input mode; handled here
                           only if it ever arrives this way */
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        InterlockedExchange(&g_quit_requested, 1);
        /* Windows gives ~5s before killing the process on CLOSE/LOGOFF/
           SHUTDOWN; the main loop may not get scheduled in time, so restore
           console modes synchronously here too, best-effort. */
        if (g_hin) SetConsoleMode(g_hin, g_orig_in_mode);
        if (g_hout) {
            SetConsoleMode(g_hout, g_orig_out_mode);
            DWORD written;
            WriteConsoleA(g_hout, "\x1b[?25h", 6, &written, NULL);
        }
        return TRUE;
    default:
        return FALSE;
    }
}

/* Keeps the actual OS console screen buffer in lockstep with our believed
   dimensions on every resize, not just at startup (see fb_screen_init's
   original comment on why they're matched at all -- no scrollback to fight
   with absolute cursor positioning). Growing is a single call, but
   SetConsoleScreenBufferSize documents that it *fails outright* if the
   requested size is smaller than the console's current window rect
   (srWindow) in either dimension -- exactly the case when the user shrinks
   the window. Left unhandled, that failure is silent: our g_width/g_cells
   shrink to the new size regardless (nothing here checks the return
   value), but the OS buffer stays at its old, larger size, so the window
   still shows stale leftover content past our new, smaller edge -- read as
   the old content "pushing" against whatever just got drawn into the
   actually-live area. The fix is the documented shrink-then-resize
   sequence: shrink the window rect first (so it's never larger than the
   buffer we're about to request), resize the buffer, then grow the window
   back out to exactly match -- restoring the no-scrollback invariant
   either way, growing or shrinking. */
static void set_console_buffer_size(HANDLE hout, int width, int height) {
    if (!hout) return;

    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(hout, &info)) {
        int window_w = info.srWindow.Right - info.srWindow.Left + 1;
        int window_h = info.srWindow.Bottom - info.srWindow.Top + 1;
        if (width < window_w || height < window_h) {
            SMALL_RECT shrink = info.srWindow;
            if (width < window_w) shrink.Right = (SHORT)(shrink.Left + width - 1);
            if (height < window_h) shrink.Bottom = (SHORT)(shrink.Top + height - 1);
            SetConsoleWindowInfo(hout, TRUE, &shrink);
        }
    }

    COORD buf_size;
    buf_size.X = (SHORT)width;
    buf_size.Y = (SHORT)height;
    SetConsoleScreenBufferSize(hout, buf_size);

    SMALL_RECT full = {0, 0, (SHORT)(width - 1), (SHORT)(height - 1)};
    SetConsoleWindowInfo(hout, TRUE, &full);
}

static void resize_buffers(int width, int height) {
    if (width < 1) width = 1;
    if (height < 1) height = 1;

    set_console_buffer_size(g_hout, width, height);

    free(g_cells);
    g_width = width;
    g_height = height;
    g_cells = (FbCell *)calloc((size_t)width * (size_t)height, sizeof(FbCell));

    /* Worst case per cell: a full SGR fg+bg change (~40 ASCII chars, each
       one wchar_t unit) plus one glyph (up to a 2-unit surrogate pair); a
       per-row cursor-position escape (~12 units) is negligible against
       that. Rounded up generously; present() never writes past
       g_out_buf_cap because it only ever appends units it has room for. */
    size_t cap = (size_t)width * (size_t)height * 48 + (size_t)height * 16 + 64;
    free(g_out_buf);
    g_out_buf = (wchar_t *)malloc(cap * sizeof(wchar_t));
    g_out_buf_cap = g_out_buf ? cap : 0;
}

int fb_screen_init(void) {
    if (g_initialized) return 0;

    g_hin = GetStdHandle(STD_INPUT_HANDLE);
    g_hout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_hin == INVALID_HANDLE_VALUE || g_hout == INVALID_HANDLE_VALUE) return -1;

    if (!GetConsoleMode(g_hin, &g_orig_in_mode)) return -1;
    if (!GetConsoleMode(g_hout, &g_orig_out_mode)) return -1;

    /* Drop line/echo/processed input and QuickEdit (QuickEdit left on is a
       classic trap: a user click into the window suspends ReadConsoleInput
       until they right-click to release selection). Keep window-resize
       events flowing in. */
    DWORD in_mode = (g_orig_in_mode | ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT) &
                    ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                             ENABLE_PROCESSED_INPUT | ENABLE_QUICK_EDIT_MODE |
                             ENABLE_INSERT_MODE);
    if (!SetConsoleMode(g_hin, in_mode)) return -1;

    DWORD out_mode = g_orig_out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                      DISABLE_NEWLINE_AUTO_RETURN;
    if (!SetConsoleMode(g_hout, out_mode)) return -1;

    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(g_hout, &info)) return -1;
    int width = info.srWindow.Right - info.srWindow.Left + 1;
    int height = info.srWindow.Bottom - info.srWindow.Top + 1;

    /* resize_buffers matches the scroll buffer to this size too (via
       set_console_buffer_size), so there is no scrollback to fight with
       absolute cursor positioning. */
    resize_buffers(width, height);
    if (!g_cells || !g_out_buf) return -1;

    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    DWORD written;
    WriteConsoleA(g_hout, "\x1b[?25l\x1b[2J", 10, &written, NULL); /* hide cursor, clear */

    g_initialized = 1;
    return 0;
}

void fb_screen_shutdown(void) {
    if (!g_initialized) return;

    DWORD written;
    if (g_hout) {
        WriteConsoleA(g_hout, "\x1b[?25h\x1b[0m", 10, &written, NULL); /* show cursor, reset attrs */
        SetConsoleMode(g_hout, g_orig_out_mode);
    }
    if (g_hin) SetConsoleMode(g_hin, g_orig_in_mode);

    free(g_cells);
    g_cells = NULL;
    free(g_out_buf);
    g_out_buf = NULL;
    g_out_buf_cap = 0;

    g_initialized = 0;
}

int fb_screen_width(void) { return g_width; }
int fb_screen_height(void) { return g_height; }

void fb_screen_clear(void) {
    if (!g_cells) return;
    FbCell blank;
    memset(&blank, 0, sizeof(blank));
    blank.ch = ' ';
    for (int i = 0; i < g_width * g_height; ++i) g_cells[i] = blank;
}

void fb_screen_set_cell(int x, int y, const FbCell *cell) {
    if (!g_cells || x < 0 || y < 0 || x >= g_width || y >= g_height) return;
    g_cells[y * g_width + x] = *cell;
}

/* Widens an ASCII (always < 0x80) escape-sequence byte string one-for-one
   into wchar_t units -- safe because these strings are always our own
   snprintf'd escape codes, never arbitrary text. */
static size_t append_ascii(wchar_t *buf, size_t cap, size_t off, const char *s, size_t len) {
    if (off + len > cap) return off; /* drop rather than overrun; cap is sized generously */
    for (size_t i = 0; i < len; ++i) buf[off + i] = (wchar_t)(unsigned char)s[i];
    return off + len;
}

static size_t append_wchar(wchar_t *buf, size_t cap, size_t off, wchar_t ch) {
    if (off + 1 > cap) return off;
    buf[off] = ch;
    return off + 1;
}

void fb_screen_present(void) {
    if (!g_cells || !g_out_buf) return;

    size_t off = 0;
    char tmp[64];

    /* "Synchronized output" (DEC private mode 2026): tells a supporting
       terminal (Windows Terminal included) to hold rendering until the
       matching end marker rather than painting whatever's in its buffer
       mid-write. Every frame here is a full-screen repaint sent as one
       WriteConsoleW call, which a terminal can still sample partway
       through -- exactly what reads as flicker/tearing on a full redraw at
       ~30fps. A terminal that doesn't recognize the sequence just ignores
       it as an unknown private mode, so this is free on anything else. */
    off = append_ascii(g_out_buf, g_out_buf_cap, off, "\x1b[?2026h", 8);

    for (int y = 0; y < g_height; ++y) {
        int n = snprintf(tmp, sizeof(tmp), "\x1b[%d;1H", y + 1);
        off = append_ascii(g_out_buf, g_out_buf_cap, off, tmp, (size_t)n);

        int have_color = 0;
        uint8_t last_fg_r = 0, last_fg_g = 0, last_fg_b = 0;
        uint8_t last_bg_r = 0, last_bg_g = 0, last_bg_b = 0;

        for (int x = 0; x < g_width; ++x) {
            const FbCell *c = &g_cells[y * g_width + x];
            if (!have_color || c->fg_r != last_fg_r || c->fg_g != last_fg_g ||
                c->fg_b != last_fg_b || c->bg_r != last_bg_r ||
                c->bg_g != last_bg_g || c->bg_b != last_bg_b) {
                n = snprintf(tmp, sizeof(tmp), "\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm",
                             c->fg_r, c->fg_g, c->fg_b, c->bg_r, c->bg_g, c->bg_b);
                off = append_ascii(g_out_buf, g_out_buf_cap, off, tmp, (size_t)n);
                last_fg_r = c->fg_r; last_fg_g = c->fg_g; last_fg_b = c->fg_b;
                last_bg_r = c->bg_r; last_bg_g = c->bg_g; last_bg_b = c->bg_b;
                have_color = 1;
            }

            uint32_t cp = c->ch;
            if (cp < 0x20 || (cp >= 0xD800 && cp <= 0xDFFF)) cp = ' '; /* control char / lone surrogate value */
            if (cp <= 0xFFFF) {
                off = append_wchar(g_out_buf, g_out_buf_cap, off, (wchar_t)cp);
            } else if (cp <= 0x10FFFF) {
                uint32_t v = cp - 0x10000;
                off = append_wchar(g_out_buf, g_out_buf_cap, off, (wchar_t)(0xD800 + (v >> 10)));
                off = append_wchar(g_out_buf, g_out_buf_cap, off, (wchar_t)(0xDC00 + (v & 0x3FF)));
            } else {
                off = append_wchar(g_out_buf, g_out_buf_cap, off, L' ');
            }
        }
    }

    off = append_ascii(g_out_buf, g_out_buf_cap, off, "\x1b[?2026l", 8);

    DWORD written;
    WriteConsoleW(g_hout, g_out_buf, (DWORD)off, &written, NULL);
}

static FbKeyCode map_key(const KEY_EVENT_RECORD *k, uint32_t *out_ch) {
    *out_ch = 0;
    int ctrl = (k->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
    int alt = (k->dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;

    if (ctrl && (k->wVirtualKeyCode == VK_SPACE)) return FB_KEY_CTRL_SPACE;
    if (k->uChar.UnicodeChar == 0x03) return FB_KEY_CTRL_C;
    if (alt && k->wVirtualKeyCode == VK_LEFT) return FB_KEY_ALT_LEFT;
    if (alt && k->wVirtualKeyCode == VK_RIGHT) return FB_KEY_ALT_RIGHT;

    switch (k->wVirtualKeyCode) {
    case VK_RETURN: return FB_KEY_ENTER;
    case VK_ESCAPE: return FB_KEY_ESC;
    case VK_BACK: return FB_KEY_BACKSPACE;
    case VK_TAB: return FB_KEY_TAB;
    case VK_UP: return FB_KEY_ARROW_UP;
    case VK_DOWN: return FB_KEY_ARROW_DOWN;
    case VK_LEFT: return FB_KEY_ARROW_LEFT;
    case VK_RIGHT: return FB_KEY_ARROW_RIGHT;
    case VK_F1: return FB_KEY_F1;
    case VK_F2: return FB_KEY_F2;
    case VK_F3: return FB_KEY_F3;
    case VK_F4: return FB_KEY_F4;
    case VK_F5: return FB_KEY_F5;
    case VK_F6: return FB_KEY_F6;
    case VK_F7: return FB_KEY_F7;
    case VK_F8: return FB_KEY_F8;
    case VK_F9: return FB_KEY_F9;
    case VK_F10: return FB_KEY_F10;
    case VK_PRIOR: return FB_KEY_PAGE_UP;
    case VK_NEXT: return FB_KEY_PAGE_DOWN;
    default: break;
    }

    if (k->uChar.UnicodeChar >= 0x20) {
        *out_ch = (uint32_t)k->uChar.UnicodeChar;
        return FB_KEY_CHAR;
    }
    return FB_KEY_NONE;
}

int fb_screen_poll_event(FbEvent *out_event, int timeout_ms) {
    memset(out_event, 0, sizeof(*out_event));

    if (InterlockedCompareExchange(&g_quit_requested, 0, 0)) {
        out_event->type = FB_EVENT_QUIT;
        return 1;
    }

    DWORD wait = WaitForSingleObject(g_hin, (DWORD)timeout_ms);
    if (wait == WAIT_TIMEOUT) return 0;
    if (wait != WAIT_OBJECT_0) return -1;

    for (;;) {
        DWORD pending = 0;
        if (!GetNumberOfConsoleInputEvents(g_hin, &pending) || pending == 0) return 0;

        INPUT_RECORD rec;
        DWORD read_count = 0;
        if (!ReadConsoleInputW(g_hin, &rec, 1, &read_count) || read_count == 0) return 0;

        if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) {
            uint32_t ch = 0;
            FbKeyCode key = map_key(&rec.Event.KeyEvent, &ch);
            if (key == FB_KEY_NONE) continue; /* bare modifier keypress etc. */
            out_event->type = FB_EVENT_KEY;
            out_event->key = key;
            out_event->ch = ch;
            return 1;
        }

        if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            int w = rec.Event.WindowBufferSizeEvent.dwSize.X;
            int h = rec.Event.WindowBufferSizeEvent.dwSize.Y;
            if (w > 0 && h > 0 && (w != g_width || h != g_height)) {
                resize_buffers(w, h);
                out_event->type = FB_EVENT_RESIZE;
                out_event->width = w;
                out_event->height = h;
                return 1;
            }
            continue;
        }

        /* mouse / focus / menu events: not used in first mile */
    }
}
