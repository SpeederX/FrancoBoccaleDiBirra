#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <crtdbg.h>
#include <windows.h>
#include <wchar.h>

static CONSOLE_SCREEN_BUFFER_INFO console_info;
static INPUT_RECORD input[8];
static int input_count;
static int position;
static int window_changes;
static int buffer_changes;
static int writes;
static int alternate_screen;
static wchar_t last_write[300000];

static HANDLE WINAPI fixture_handle(DWORD which) {
    return (HANDLE)(uintptr_t)which;
}

static BOOL WINAPI fixture_get_mode(HANDLE handle, LPDWORD mode) {
    (void)handle;
    *mode = 0;
    return TRUE;
}

static BOOL WINAPI fixture_set_mode(HANDLE handle, DWORD mode) {
    (void)handle;
    (void)mode;
    return TRUE;
}

static BOOL WINAPI fixture_info(HANDLE handle, PCONSOLE_SCREEN_BUFFER_INFO info) {
    (void)handle;
    *info = console_info;
    return TRUE;
}

static BOOL WINAPI fixture_window(HANDLE handle, BOOL absolute, const SMALL_RECT *rect) {
    (void)handle;
    (void)absolute;
    (void)rect;
    window_changes++;
    return TRUE;
}

static BOOL WINAPI fixture_buffer(HANDLE handle, COORD size) {
    (void)handle;
    (void)size;
    buffer_changes++;
    return TRUE;
}

static BOOL WINAPI fixture_handler(PHANDLER_ROUTINE handler, BOOL add) {
    (void)handler;
    (void)add;
    return TRUE;
}

static BOOL WINAPI fixture_write_a(HANDLE handle, const VOID *text, DWORD count, LPDWORD written, LPVOID reserved) {
    (void)handle;
    (void)reserved;
    const char *bytes = text;
    if (count >= 8 && memcmp(bytes, "\x1b[?1049h", 8) == 0) {
        alternate_screen = 1;
    }
    *written = count;
    return TRUE;
}

static BOOL WINAPI fixture_write_w(HANDLE handle, const VOID *text, DWORD count, LPDWORD written, LPVOID reserved) {
    (void)handle;
    (void)reserved;
    assert(count < sizeof(last_write) / sizeof(last_write[0]));
    memcpy(last_write, text, count * sizeof(wchar_t));
    last_write[count] = 0;
    writes++;
    *written = count;
    return TRUE;
}

static DWORD WINAPI fixture_wait(HANDLE handle, DWORD timeout) {
    (void)handle;
    (void)timeout;
    return position < input_count ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}

static BOOL WINAPI fixture_pending(HANDLE handle, LPDWORD count) {
    (void)handle;
    *count = (DWORD)(input_count - position);
    return TRUE;
}

static BOOL WINAPI fixture_read(HANDLE handle, PINPUT_RECORD record, DWORD count, LPDWORD read) {
    (void)handle;
    assert(count == 1 && position < input_count);
    *record = input[position++];
    *read = 1;
    return TRUE;
}

#define GetStdHandle fixture_handle
#define GetConsoleMode fixture_get_mode
#define SetConsoleMode fixture_set_mode
#define GetConsoleScreenBufferInfo fixture_info
#define SetConsoleWindowInfo fixture_window
#define SetConsoleScreenBufferSize fixture_buffer
#define SetConsoleCtrlHandler fixture_handler
#define WriteConsoleA fixture_write_a
#define WriteConsoleW fixture_write_w
#define WaitForSingleObject fixture_wait
#define GetNumberOfConsoleInputEvents fixture_pending
#define ReadConsoleInputW fixture_read
#include "../src/platform/win/console_win.c"

static void viewport(int width, int height) {
    console_info.srWindow = (SMALL_RECT){0, 0, (SHORT)(width - 1), (SHORT)(height - 1)};
    console_info.dwSize = (COORD){(SHORT)width, 900};
}

int main(void) {
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    SetErrorMode(SEM_NOGPFAULTERRORBOX);
    viewport(120, 36);
    assert(fb_screen_init() == 0);
    assert(window_changes == 0 && buffer_changes == 0);
    assert(alternate_screen);
    assert(fb_screen_width() == 120 && fb_screen_height() == 36);
    fb_screen_clear();
    fb_screen_present();
    assert(writes == 1);
    fb_screen_present();
    assert(writes == 1);
    FbCell cell = {'X', 255, 255, 255, 0, 0, 0, 0};
    fb_screen_set_cell(4, 5, &cell);
    fb_screen_present();
    assert(writes == 2);
    assert(wcsstr(last_write, L"[6;1H") && !wcsstr(last_write, L"[1;1H"));

    viewport(80, 22);
    input[0].EventType = WINDOW_BUFFER_SIZE_EVENT;
    input[0].Event.WindowBufferSizeEvent.dwSize = (COORD){180, 900};
    input[1].EventType = WINDOW_BUFFER_SIZE_EVENT;
    input[1].Event.WindowBufferSizeEvent.dwSize = (COORD){30, 900};
    input[2].EventType = KEY_EVENT;
    input[2].Event.KeyEvent.bKeyDown = TRUE;
    input[2].Event.KeyEvent.wVirtualKeyCode = VK_F8;
    input_count = 3;
    FbEvent event;
    assert(fb_screen_poll_event(&event, 0) == 1);
    assert(event.type == FB_EVENT_RESIZE && event.width == 80 && event.height == 22);
    assert(window_changes == 0 && buffer_changes == 0);
    assert(fb_screen_poll_event(&event, 0) == 1);
    assert(event.type == FB_EVENT_KEY && event.key == FB_KEY_F8);
    assert(fb_screen_width() == 80 && fb_screen_height() == 22);
    fb_screen_clear();
    fb_screen_present();
    assert(writes == 3);

    viewport(50, 18);
    fb_screen_present();
    assert(writes == 3);
    assert(fb_screen_poll_event(&event, 0) == 1);
    assert(event.type == FB_EVENT_RESIZE && event.width == 50 && event.height == 18);
    assert(fb_screen_poll_event(&event, 0) == 0);
    fb_screen_clear();
    fb_screen_present();
    assert(writes == 4);
    assert(window_changes == 0 && buffer_changes == 0);
    fb_screen_shutdown();
    return 0;
}
