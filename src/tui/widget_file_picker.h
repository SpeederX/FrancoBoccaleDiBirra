#ifndef FB_WIDGET_FILE_PICKER_H
#define FB_WIDGET_FILE_PICKER_H

#include "platform/dirlist.h"
#include "tui/layout.h"
#include "tui/screen.h"

#define FB_PICKER_MAX_ENTRIES 512

typedef struct {
    int active;
    char cwd[600];
    FbDirEntry entries[FB_PICKER_MAX_ENTRIES];
    int entry_count;
    int selected; /* scroll position is derived from this at render time */
} FbFilePicker;

/* Lists directories plus *.gguf files only — this picker exists
   specifically to stop "/add"ing a directory or a non-model file by
   mistake, not to be a general file browser. */
void fb_file_picker_open(FbFilePicker *p, const char *start_dir);
void fb_file_picker_close(FbFilePicker *p);

typedef enum {
    FB_PICKER_NONE = 0,   /* event consumed (navigation), picker still open */
    FB_PICKER_SELECTED,   /* a .gguf file was chosen; out_path filled, picker closed */
    FB_PICKER_CANCELLED   /* esc; picker closed */
} FbPickerResult;

/* Only call while p->active. */
FbPickerResult fb_file_picker_handle_event(FbFilePicker *p, const FbEvent *ev,
                                            char *out_path, size_t out_cap);

void fb_file_picker_render(const FbFilePicker *p, FbRect area);

#endif /* FB_WIDGET_FILE_PICKER_H */
