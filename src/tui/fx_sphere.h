#ifndef FB_FX_SPHERE_H
#define FB_FX_SPHERE_H

#include "tui/layout.h"

/*
 * A rotating, shaded ASCII sphere in the donut.c lineage: a unit sphere
 * sampled in (theta, phi), rotated over time, projected to the character
 * grid with a z-buffer, shaded by a fixed light direction into the ASCII
 * luminance ramp. Pure CPU math, no GPU calls.
 *
 * intensity in [0,1]: 0 is the idle state (subtle pulsation, cool color),
 * 1 is "actively recording" (stronger pulsation, hot color) — wired to the
 * mic hotkey starting in milestone 4/5, synthetic (time-only) until then.
 */
void fb_fx_sphere_render(FbRect area, unsigned long long elapsed_ms, float intensity);

#endif /* FB_FX_SPHERE_H */
