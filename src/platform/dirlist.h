#ifndef FB_DIRLIST_H
#define FB_DIRLIST_H

/*
 * Cross-platform directory listing, chosen by CMake source-list selection
 * (platform/win/dirlist_win.c vs platform/posix/dirlist_posix.c) — same
 * pattern as tui/screen.h and platform/proc.h.
 */

#define FB_DIRENTRY_NAME_MAX 260

typedef struct {
    char name[FB_DIRENTRY_NAME_MAX];
    int is_dir;
} FbDirEntry;

/* Lists dir's immediate entries (excluding ".") into out[], up to
   max_entries. Returns the count (0 or more), or -1 if dir doesn't exist,
   isn't a directory, or can't be read. */
int fb_list_dir(const char *dir, FbDirEntry *out, int max_entries);

/* Lists available drives ("C:", "D:", ...) on Windows. Returns 0 on
   platforms with no drive-letter concept (POSIX has a single root reached
   via normal ".." navigation, so this is a legitimate empty result there,
   not an error) — callers should treat 0 as "no drive switcher to offer",
   not fall back to anything. Never returns -1. */
int fb_list_drives(FbDirEntry *out, int max_entries);

/* True if path exists and is a directory (false for a file, or for a path
   that doesn't exist at all). */
int fb_path_is_dir(const char *path);

#endif /* FB_DIRLIST_H */
