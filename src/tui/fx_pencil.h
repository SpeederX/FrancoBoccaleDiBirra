#ifndef FB_FX_PENCIL_H
#define FB_FX_PENCIL_H

#include "tui/layout.h"

/*
 * A pencil doodling: a single point does a bounded random walk over the
 * area, leaving a fading trail of scratch marks behind it -- the
 * scratchpad mode's visual identity. Pure CPU math, module-singleton state
 * (the trail itself), same pattern as mic_capture.c's one capture device.
 */
void fb_fx_pencil_render(FbRect area, unsigned long long elapsed_ms, uint8_t r, uint8_t g,
                          uint8_t b);

#endif /* FB_FX_PENCIL_H */
