#include "platform/dirlist.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

int fb_list_dir(const char *dir, FbDirEntry *out, int max_entries) {
    char pattern[MAX_PATH];
    if (snprintf(pattern, sizeof(pattern), "%s\\*", dir) >= (int)sizeof(pattern)) return -1;

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;

    int count = 0;
    do {
        if (strcmp(fd.cFileName, ".") == 0) continue;
        if (count >= max_entries) break;

        strncpy(out[count].name, fd.cFileName, FB_DIRENTRY_NAME_MAX - 1);
        out[count].name[FB_DIRENTRY_NAME_MAX - 1] = '\0';
        out[count].is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        count++;
    } while (FindNextFileA(h, &fd));

    FindClose(h);
    return count;
}

int fb_path_is_dir(const char *path) {
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

int fb_list_drives(FbDirEntry *out, int max_entries) {
    char buf[256];
    DWORD n = GetLogicalDriveStringsA(sizeof(buf), buf);
    if (n == 0 || n > sizeof(buf)) return 0;

    int count = 0;
    const char *p = buf;
    while (*p != '\0' && count < max_entries) {
        size_t len = strlen(p);
        size_t copy_len = len;
        if (copy_len > 0 && p[copy_len - 1] == '\\') copy_len--; /* "C:\" -> "C:" */
        if (copy_len >= FB_DIRENTRY_NAME_MAX) copy_len = FB_DIRENTRY_NAME_MAX - 1;

        memcpy(out[count].name, p, copy_len);
        out[count].name[copy_len] = '\0';
        out[count].is_dir = 1;
        count++;

        p += len + 1;
    }
    return count;
}
