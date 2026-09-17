#ifndef FB_FX_WAVEFORM_H
#define FB_FX_WAVEFORM_H

#include "tui/layout.h"

/*
 * Renders a live amplitude bar across `area`. `samples` holds amplitude
 * values in [0,1], oldest first; only the most recent `area.w` samples are
 * used (older ones are dropped, newest on the right) — this is the same
 * shape the real mic ring buffer will hand in starting milestone 4, so this
 * widget doesn't change when the synthetic milestone-3 demo data is
 * replaced by real capture.
 */
void fb_fx_waveform_render(FbRect area, const float *samples, int sample_count);

#endif /* FB_FX_WAVEFORM_H */
