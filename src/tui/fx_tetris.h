#ifndef FB_FX_TETRIS_H
#define FB_FX_TETRIS_H

#include "tui/layout.h"

/*
 * Blocks of random height drop one at a time into random columns and pile
 * up; when a column tops out, the whole board clears and starts again --
 * project mode's visual identity (build-up, like a project accumulating
 * work). Module-singleton state (the column heights), same pattern as
 * fx_pencil.c's trail.
 */
void fb_fx_tetris_render(FbRect area, unsigned long long elapsed_ms, uint8_t r, uint8_t g,
                          uint8_t b);

#endif /* FB_FX_TETRIS_H */
