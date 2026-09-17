#include "tui/widget_file_picker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tui/palette.h"

static int ends_with_ci(const char *s, const char *suffix) {
    size_t s_len = strlen(s), suf_len = strlen(suffix);
    if (s_len < suf_len) return 0;
    const char *tail = s + (s_len - suf_len);
    for (size_t i = 0; i < suf_len; ++i) {
        char a = tail[i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}

#define FB_DRIVES_MARKER "<drives>" /* '<' '>' are illegal in real filenames on Windows */

static int compare_entries(const void *a, const void *b) {
    const FbDirEntry *ea = (const FbDirEntry *)a;
    const FbDirEntry *eb = (const FbDirEntry *)b;
    int a_special = strcmp(ea->name, FB_DRIVES_MARKER) == 0;
    int b_special = strcmp(eb->name, FB_DRIVES_MARKER) == 0;
    if (a_special != b_special) return a_special ? -1 : 1;
    int a_up = strcmp(ea->name, "..") == 0;
    int b_up = strcmp(eb->name, "..") == 0;
    if (a_up != b_up) return a_up ? -1 : 1;
    if (ea->is_dir != eb->is_dir) return ea->is_dir ? -1 : 1;
    return strcmp(ea->name, eb->name);
}

/* Strips the last path component. Leaves a Windows drive-root ("C:") as
   "C:/" rather than an ambiguous bare "C:" (which FindFirstFileA would
   otherwise interpret as "current directory on C:", not the root). Leaves
   a POSIX root "/" unchanged. */
static void path_parent(char *path) {
    size_t len = strlen(path);
    while (len > 0 && (path[len - 1] == '/' || path[len - 1] == '\\')) path[--len] = '\0';

    char *slash = strrchr(path, '/');
    char *bslash = strrchr(path, '\\');
    char *last = (slash && bslash) ? (slash > bslash ? slash : bslash) : (slash ? slash : bslash);
    if (!last) return; /* nothing left to strip */

    *last = '\0';
    size_t new_len = strlen(path);
    if (new_len == 0) {
        path[0] = '/';
        path[1] = '\0';
    } else if (new_len == 2 && path[1] == ':') {
        path[2] = '/';
        path[3] = '\0';
    }
}

/* cwd == "" is a sentinel meaning "show the drive list", not a real path. */
static int in_drives_mode(const FbFilePicker *p) { return p->cwd[0] == '\0'; }

static void refresh_listing(FbFilePicker *p) {
    /* Heap, not a stack array: FB_PICKER_MAX_ENTRIES FbDirEntry values run
       to ~135KB, and stacking fixed-size buffers like that has already
       overflowed a 1MB thread stack once this session (calibr_results.c's
       equivalent list, at a larger size) — closing out the same pattern
       here too rather than trusting this particular call depth is shallow
       enough. */
    FbDirEntry *raw = (FbDirEntry *)malloc(FB_PICKER_MAX_ENTRIES * sizeof(FbDirEntry));
    p->entry_count = 0;
    if (!raw) return;

    if (in_drives_mode(p)) {
        int n = fb_list_drives(raw, FB_PICKER_MAX_ENTRIES);
        for (int i = 0; i < n; ++i) p->entries[p->entry_count++] = raw[i];
    } else {
        int n = fb_list_dir(p->cwd, raw, FB_PICKER_MAX_ENTRIES);
        if (n < 0) n = 0;
        for (int i = 0; i < n; ++i) {
            if (raw[i].is_dir || ends_with_ci(raw[i].name, ".gguf")) {
                p->entries[p->entry_count++] = raw[i];
            }
        }

        /* Offer a way to switch drives from any directory, not just from a
           root — but only where the platform actually has drives (Windows);
           a 0-entry probe means "not applicable", not "no drives found". */
        FbDirEntry probe[1];
        if (fb_list_drives(probe, 1) > 0 && p->entry_count < FB_PICKER_MAX_ENTRIES) {
            FbDirEntry *marker = &p->entries[p->entry_count++];
            strncpy(marker->name, FB_DRIVES_MARKER, FB_DIRENTRY_NAME_MAX - 1);
            marker->name[FB_DIRENTRY_NAME_MAX - 1] = '\0';
            marker->is_dir = 1;
        }
    }
    free(raw);

    qsort(p->entries, (size_t)p->entry_count, sizeof(FbDirEntry), compare_entries);
    p->selected = 0;
}

void fb_file_picker_open(FbFilePicker *p, const char *start_dir) {
    memset(p, 0, sizeof(*p));
    strncpy(p->cwd, start_dir, sizeof(p->cwd) - 1);
    p->active = 1;
    refresh_listing(p);
}

void fb_file_picker_close(FbFilePicker *p) { p->active = 0; }

FbPickerResult fb_file_picker_handle_event(FbFilePicker *p, const FbEvent *ev, char *out_path,
                                            size_t out_cap) {
    if (ev->type != FB_EVENT_KEY) return FB_PICKER_NONE;

    switch (ev->key) {
    case FB_KEY_ESC:
        fb_file_picker_close(p);
        return FB_PICKER_CANCELLED;
    case FB_KEY_ARROW_UP:
        if (p->selected > 0) p->selected--;
        return FB_PICKER_NONE;
    case FB_KEY_ARROW_DOWN:
        if (p->selected + 1 < p->entry_count) p->selected++;
        return FB_PICKER_NONE;
    case FB_KEY_ENTER: {
        if (p->entry_count == 0) return FB_PICKER_NONE;
        FbDirEntry *e = &p->entries[p->selected];
        if (e->is_dir) {
            if (strcmp(e->name, FB_DRIVES_MARKER) == 0) {
                p->cwd[0] = '\0'; /* enter drives mode */
            } else if (in_drives_mode(p)) {
                snprintf(p->cwd, sizeof(p->cwd), "%s/", e->name); /* e.g. "C:" -> "C:/" */
            } else if (strcmp(e->name, "..") == 0) {
                path_parent(p->cwd);
            } else {
                char joined[600];
                snprintf(joined, sizeof(joined), "%s/%s", p->cwd, e->name);
                strncpy(p->cwd, joined, sizeof(p->cwd) - 1);
                p->cwd[sizeof(p->cwd) - 1] = '\0';
            }
            refresh_listing(p);
            return FB_PICKER_NONE;
        }
        snprintf(out_path, out_cap, "%s/%s", p->cwd, e->name);
        fb_file_picker_close(p);
        return FB_PICKER_SELECTED;
    }
    default:
        return FB_PICKER_NONE;
    }
}

void fb_file_picker_render(const FbFilePicker *p, FbRect area) {
    fb_fill_rect(area, FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);
    if (area.h < 2 || area.w < 4) return;

    char header[600];
    snprintf(header, sizeof(header), " %s  (dirs + *.gguf, enter=open/select, esc=cancel)",
             in_drives_mode(p) ? "(choose a drive)" : p->cwd);
    fb_draw_text(area.x, area.y, header, FB_COLOR_ACCENT_R, FB_COLOR_ACCENT_G, FB_COLOR_ACCENT_B,
                 FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);

    int list_h = area.h - 1;
    if (list_h <= 0) return;

    int scroll = 0;
    if (p->selected >= list_h) scroll = p->selected - list_h + 1;
    if (p->entry_count - scroll < list_h) scroll = p->entry_count - list_h;
    if (scroll < 0) scroll = 0;

    if (p->entry_count == 0) {
        fb_draw_text(area.x, area.y + 1, " (empty)", FB_COLOR_DIM_R, FB_COLOR_DIM_G,
                     FB_COLOR_DIM_B, FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);
        return;
    }

    for (int row = 0; row < list_h; ++row) {
        int idx = scroll + row;
        if (idx >= p->entry_count) break;
        const FbDirEntry *e = &p->entries[idx];
        int selected = (idx == p->selected);

        int is_marker = strcmp(e->name, FB_DRIVES_MARKER) == 0;
        char line[300];
        snprintf(line, sizeof(line), "%s %s%s", selected ? ">" : " ", e->name,
                 (e->is_dir && !is_marker) ? "/" : "");

        uint8_t r, g, b;
        if (selected) { r = FB_COLOR_HOT_R; g = FB_COLOR_HOT_G; b = FB_COLOR_HOT_B; }
        else if (is_marker) { r = FB_COLOR_DIM_R; g = FB_COLOR_DIM_G; b = FB_COLOR_DIM_B; }
        else if (e->is_dir) { r = FB_COLOR_ACCENT_R; g = FB_COLOR_ACCENT_G; b = FB_COLOR_ACCENT_B; }
        else { r = FB_COLOR_FG_R; g = FB_COLOR_FG_G; b = FB_COLOR_FG_B; }

        fb_draw_text(area.x, area.y + 1 + row, line, r, g, b, FB_COLOR_BG_R, FB_COLOR_BG_G,
                     FB_COLOR_BG_B);
    }
}
