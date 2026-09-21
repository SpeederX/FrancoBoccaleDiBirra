/* Not yet runtime-tested (development happened on Windows this pass); it
   must be exercised on a real Linux/macOS machine before being relied on. */

#include "platform/dirlist.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int fb_list_dir(const char *dir, FbDirEntry *out, int max_entries) {
    DIR *d = opendir(dir);
    if (!d) return -1;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0) continue;
        if (count >= max_entries) break;

        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        struct stat st;
        int is_dir = (stat(full, &st) == 0) && S_ISDIR(st.st_mode);

        strncpy(out[count].name, ent->d_name, FB_DIRENTRY_NAME_MAX - 1);
        out[count].name[FB_DIRENTRY_NAME_MAX - 1] = '\0';
        out[count].is_dir = is_dir;
        count++;
    }

    closedir(d);
    return count;
}

int fb_path_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int fb_file_size(const char *path, unsigned long long *out_size) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    if (out_size) *out_size = (unsigned long long)st.st_size;
    return 1;
}

int fb_list_drives(FbDirEntry *out, int max_entries) {
    (void)out;
    (void)max_entries;
    return 0; /* no drive-letter concept; "/" is already reachable via ".." */
}
