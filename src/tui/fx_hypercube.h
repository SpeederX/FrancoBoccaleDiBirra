#ifndef FB_FX_HYPERCUBE_H
#define FB_FX_HYPERCUBE_H

#include "tui/layout.h"

/*
 * A tesseract (4D hypercube) wireframe, rotating simultaneously in two
 * orthogonal planes (XW and YZ) and projected 4D -> 3D -> 2D -- the classic
 * "cube within a cube, edges passing through each other" look. Diary
 * mode's visual identity: reflective, layered, always turning. Pure CPU
 * math (vertex transform + Bresenham line draw), no GPU calls.
 */
void fb_fx_hypercube_render(FbRect area, unsigned long long elapsed_ms, uint8_t r, uint8_t g,
                             uint8_t b);

#endif /* FB_FX_HYPERCUBE_H */
