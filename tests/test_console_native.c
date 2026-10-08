#include <windows.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <crtdbg.h>
#include "tui/screen.h"

static const int sizes[][2] = {{120,36}, {76,22}, {50,18}, {120,36}, {90,22}, {90,30}, {30,12}, {120,36}};
typedef struct { LONG command; LONG rendered; } Fixture;

static DWORD WINAPI drain_output(void *argument) {
    HANDLE output = argument;
    char buffer[4096];
    DWORD count;
    while (ReadFile(output, buffer, sizeof(buffer), &count, NULL) && count > 0) {
    }
    return 0;
}

static int child(const char *map_name) {
    HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, map_name);
    assert(mapping);
    Fixture *fixture = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Fixture));
    assert(fixture);
    HANDLE console_output = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE console_input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    assert(console_output != INVALID_HANDLE_VALUE && console_input != INVALID_HANDLE_VALUE);
    assert(SetStdHandle(STD_OUTPUT_HANDLE, console_output));
    assert(SetStdHandle(STD_INPUT_HANDLE, console_input));
    assert(fb_screen_init() == 0);
    for (int i = 0; i < 8; ++i) {
        int width = sizes[i][0];
        int height = sizes[i][1];
        ULONGLONG deadline = GetTickCount64() + 5000;
        while (InterlockedCompareExchange(&fixture->command, 0, 0) != i + 1) {
            assert(GetTickCount64() < deadline);
            Sleep(1);
        }
        while (fb_screen_width() != width || fb_screen_height() != height) {
            FbEvent event;
            fb_screen_poll_event(&event, 0);
            assert(GetTickCount64() < deadline);
            Sleep(1);
        }
        fb_screen_clear();
        FbCell cell = {'X', 130, 205, 185, 10, 14, 22, 0};
        fb_screen_set_cell(width - 1, height - 1, &cell);
        fb_screen_present();
        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        wchar_t character = 0;
        DWORD count = 0;
        assert(ReadConsoleOutputCharacterW(output, &character, 1,
            (COORD){(SHORT)(width - 1), (SHORT)(height - 1)}, &count));
        assert(count == 1 && character == L'X');
        CONSOLE_SCREEN_BUFFER_INFO info;
        assert(GetConsoleScreenBufferInfo(output, &info));
        assert(info.srWindow.Right - info.srWindow.Left + 1 == width);
        assert(info.srWindow.Bottom - info.srWindow.Top + 1 == height);
        InterlockedExchange(&fixture->rendered, i + 1);
    }
    fb_screen_shutdown();
    CloseHandle(console_input);
    CloseHandle(console_output);
    UnmapViewOfFile(fixture);
    CloseHandle(mapping);
    return 0;
}

int main(int argc, char **argv) {
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    if (argc == 3 && strcmp(argv[1], "--child") == 0) {
        return child(argv[2]);
    }
    assert(argc == 2);
    char map_name[100];
    snprintf(map_name, sizeof(map_name), "Local\\FrancoConsoleTest-%lu", GetCurrentProcessId());
    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
        0, sizeof(Fixture), map_name);
    assert(mapping);
    Fixture *fixture = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Fixture));
    assert(fixture);
    HANDLE input_read, input_write, output_read, output_write;
    assert(CreatePipe(&input_read, &input_write, NULL, 0));
    assert(CreatePipe(&output_read, &output_write, NULL, 0));
    HPCON console;
    assert(SUCCEEDED(CreatePseudoConsole((COORD){120,36}, input_read, output_write, 0, &console)));
    CloseHandle(input_read);
    CloseHandle(output_write);
    HANDLE reader = CreateThread(NULL, 0, drain_output, output_read, 0, NULL);
    assert(reader);
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
    STARTUPINFOEXW startup = {0};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = malloc(attribute_size);
    assert(startup.lpAttributeList);
    assert(InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_size));
    assert(UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
        console, sizeof(console), NULL, NULL));
    wchar_t executable[1024];
    assert(GetModuleFileNameW(NULL, executable, 1024));
    wchar_t command[1200];
    swprintf(command, 1200, L"\"%ls\" --child %hs", executable, map_name);
    PROCESS_INFORMATION process = {0};
    assert(CreateProcessW(NULL, command, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT,
        NULL, NULL, &startup.StartupInfo, &process));
    for (int i = 0; i < 8; ++i) {
        assert(SUCCEEDED(ResizePseudoConsole(console, (COORD){(SHORT)sizes[i][0], (SHORT)sizes[i][1]})));
        InterlockedExchange(&fixture->command, i + 1);
        ULONGLONG deadline = GetTickCount64() + 5000;
        while (InterlockedCompareExchange(&fixture->rendered, 0, 0) != i + 1) {
            DWORD exit_code;
            assert(GetExitCodeProcess(process.hProcess, &exit_code) && exit_code == STILL_ACTIVE);
            assert(GetTickCount64() < deadline);
            Sleep(1);
        }
    }
    assert(WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0);
    DWORD exit_code;
    assert(GetExitCodeProcess(process.hProcess, &exit_code) && exit_code == 0);
    ClosePseudoConsole(console);
    CloseHandle(input_write);
    assert(WaitForSingleObject(reader, 5000) == WAIT_OBJECT_0);
    CloseHandle(reader);
    CloseHandle(output_read);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    free(startup.lpAttributeList);
    UnmapViewOfFile(fixture);
    CloseHandle(mapping);
    FILE *receipt = fopen(argv[1], "wb");
    assert(receipt);
    fputs("Native ConPTY: 8 resize layouts and final-cell rendering passed.\n", receipt);
    fclose(receipt);
    return 0;
}
