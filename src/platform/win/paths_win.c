#include "util/paths.h"

#include <windows.h>
#include <stdio.h>

int fb_replace_file(const char *source, const char *destination) {
    return MoveFileExA(source, destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
               ? 0 : -1;
}

int fb_ensure_dir(const char *path) {
    if (!CreateDirectoryA(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return -1;
    }
    return 0;
}

int fb_get_home_dir(char *out, size_t cap) {
    DWORD n = GetEnvironmentVariableA("USERPROFILE", out, (DWORD)cap);
    return (n == 0 || n >= cap) ? -1 : 0;
}

int fb_get_data_dir(char *out, size_t cap) {
    char base[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof(base));
    if (n == 0 || n >= sizeof(base)) return -1;

    int written = snprintf(out, cap, "%s\\FrancoBoccaleDiBirra", base);
    if (written < 0 || (size_t)written >= cap) return -1;

    return fb_ensure_dir(out);
}
