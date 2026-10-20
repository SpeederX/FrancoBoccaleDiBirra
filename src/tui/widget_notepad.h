#ifndef FB_WIDGET_NOTEPAD_H
#define FB_WIDGET_NOTEPAD_H

#include <stddef.h>

#include "tui/layout.h"

typedef struct {
    char *text;
    size_t len;
    size_t capacity;
    size_t cursor;
    size_t revision;
    int scroll;
} FbNotepad;

void fb_notepad_init(FbNotepad *pad);
void fb_notepad_free(FbNotepad *pad);
int fb_notepad_set(FbNotepad *pad, const char *text);
int fb_notepad_handle_event(FbNotepad *pad, const FbEvent *event);
void fb_notepad_render(FbNotepad *pad, FbRect area, int focused);

#endif
