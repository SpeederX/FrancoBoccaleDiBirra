/*
 * Windows implementation of tui/screen.h. termbox2 has no Windows backend at
 * all (verified against its source), so this is a real, if minimal, Win32
 * console TUI layer: ReadConsoleInputW for input/resize, VT/SGR escape
 * sequences over WriteConsoleA for truecolor output. Restricted to ASCII
 * glyphs (0-127) for now so plain WriteConsoleA is codepage-safe; Unicode
 * block glyphs would need a WriteConsoleW path and a font/codepage
 * capability check first.
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

static char *g_out_buf = NULL;
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

static void resize_buffers(int width, int height) {
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    free(g_cells);
    g_width = width;
    g_height = height;
    g_cells = (FbCell *)calloc((size_t)width * (size_t)height, sizeof(FbCell));

    /* Worst case per cell: a full SGR fg+bg change (~40 bytes) plus one
       character; a per-row cursor-position escape (~12 bytes) is negligible
       against that. Rounded up generously; present() never writes past
       g_out_buf_cap because it only ever appends bytes it has room for. */
    size_t cap = (size_t)width * (size_t)height * 48 + (size_t)height * 16 + 64;
    free(g_out_buf);
    g_out_buf = (char *)malloc(cap);
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

    /* Match the scroll buffer to the visible window so there is no
       scrollback to fight with absolute cursor positioning. */
    COORD buf_size;
    buf_size.X = (SHORT)width;
    buf_size.Y = (SHORT)height;
    SetConsoleScreenBufferSize(g_hout, buf_size);

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

static size_t append(char *buf, size_t cap, size_t off, const char *s, size_t len) {
    if (off + len > cap) return off; /* drop rather than overrun; cap is sized generously */
    memcpy(buf + off, s, len);
    return off + len;
}

void fb_screen_present(void) {
    if (!g_cells || !g_out_buf) return;

    size_t off = 0;
    char tmp[64];

    for (int y = 0; y < g_height; ++y) {
        int n = snprintf(tmp, sizeof(tmp), "\x1b[%d;1H", y + 1);
        off = append(g_out_buf, g_out_buf_cap, off, tmp, (size_t)n);

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
                off = append(g_out_buf, g_out_buf_cap, off, tmp, (size_t)n);
                last_fg_r = c->fg_r; last_fg_g = c->fg_g; last_fg_b = c->fg_b;
                last_bg_r = c->bg_r; last_bg_g = c->bg_g; last_bg_b = c->bg_b;
                have_color = 1;
            }
            char ch = (c->ch >= 0x20 && c->ch < 0x7f) ? (char)c->ch : ' ';
            off = append(g_out_buf, g_out_buf_cap, off, &ch, 1);
        }
    }

    DWORD written;
    WriteConsoleA(g_hout, g_out_buf, (DWORD)off, &written, NULL);
}

static FbKeyCode map_key(const KEY_EVENT_RECORD *k, uint32_t *out_ch) {
    *out_ch = 0;
    int ctrl = (k->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;

    if (ctrl && (k->wVirtualKeyCode == VK_SPACE)) return FB_KEY_CTRL_SPACE;
    if (k->uChar.UnicodeChar == 0x03) return FB_KEY_CTRL_C;

    switch (k->wVirtualKeyCode) {
    case VK_RETURN: return FB_KEY_ENTER;
    case VK_ESCAPE: return FB_KEY_ESC;
    case VK_BACK: return FB_KEY_BACKSPACE;
    case VK_TAB: return FB_KEY_TAB;
    case VK_UP: return FB_KEY_ARROW_UP;
    case VK_DOWN: return FB_KEY_ARROW_DOWN;
    case VK_LEFT: return FB_KEY_ARROW_LEFT;
    case VK_RIGHT: return FB_KEY_ARROW_RIGHT;
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
