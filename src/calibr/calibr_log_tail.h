#ifndef FB_CALIBR_LOG_TAIL_H
#define FB_CALIBR_LOG_TAIL_H

#include "tui/widget_textview.h"

typedef struct {
    long byte_offset;
} FbCalibrLogTail;

void fb_calibr_log_tail_init(FbCalibrLogTail *tail);

/* Reads whatever's been appended to <calibr_root>/log/action-trace.jsonl
   since the last call, formats each new complete line into a compact
   display line, and appends it to tv. Detects truncation/rotation (file
   shrank since we last read it) and reopens from the start. A missing
   file is normal (not configured / calibr never run yet) and a no-op, not
   an error. Call once per frame. */
void fb_calibr_log_tail_poll(FbCalibrLogTail *tail, const char *calibr_root, FbTextView *tv);

#endif /* FB_CALIBR_LOG_TAIL_H */
