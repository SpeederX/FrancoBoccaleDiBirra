#ifndef FB_WIDGET_TEXTVIEW_H
#define FB_WIDGET_TEXTVIEW_H

#include "tui/layout.h"
#include "tui/screen.h"

/*
 * A generic scrollable line buffer, shared by the calibr leaderboard view
 * (a full rebuild each time recommendation.json is (re)read) and the log
 * view (lines appended live as action-trace.jsonl grows). Kept
 * intentionally dumb — no owner needs anything fancier than "here are some
 * lines, let me scroll through them."
 */

#define FB_TEXTVIEW_LINE_MAX 220
#define FB_TEXTVIEW_MAX_LINES 2000

/* Whole-line style hint -- deliberately coarse (see app.c's markdown
   stripper): there is no per-character/inline styling here, just enough to
   make a "# Header" line stand out instead of showing its literal '#'. */
typedef enum {
    FB_TEXTVIEW_STYLE_NORMAL = 0,
    FB_TEXTVIEW_STYLE_HEADER
} FbTextViewStyle;

typedef struct {
    char lines[FB_TEXTVIEW_MAX_LINES][FB_TEXTVIEW_LINE_MAX];
    uint8_t style[FB_TEXTVIEW_MAX_LINES];
    int count;
    int scroll; /* index of the topmost visible line */
} FbTextView;

void fb_textview_clear(FbTextView *tv);

/* Appends one line, dropping the oldest (shifting) if already full. */
void fb_textview_add_line(FbTextView *tv, const char *line);

/* Same, but tagged with a style other than the default NORMAL. */
void fb_textview_add_line_styled(FbTextView *tv, const char *line, FbTextViewStyle style);

/* Arrow up/down scroll by one line; clamped to the buffer's extent given
   how many rows are currently visible. Only consumes arrow keys. */
void fb_textview_handle_event(FbTextView *tv, const FbEvent *ev, int visible_rows);

void fb_textview_render(const FbTextView *tv, FbRect area);

#endif /* FB_WIDGET_TEXTVIEW_H */
