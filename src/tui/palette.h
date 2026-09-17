#ifndef FB_PALETTE_H
#define FB_PALETTE_H

/* A small, deliberately restrained demoscene palette: near-black ground,
   one cool accent (cyan) for structure, one hot accent (magenta) for
   activity/recording state. Plain ASCII glyphs only for now (see
   console_win.c) — Unicode block shading is a later capability-gated
   addition. */

#define FB_COLOR_BG_R 0x0a
#define FB_COLOR_BG_G 0x0a
#define FB_COLOR_BG_B 0x0e

#define FB_COLOR_FG_R 0xd8
#define FB_COLOR_FG_G 0xe0
#define FB_COLOR_FG_B 0xe8

#define FB_COLOR_ACCENT_R 0x35
#define FB_COLOR_ACCENT_G 0xd8
#define FB_COLOR_ACCENT_B 0xe0

#define FB_COLOR_HOT_R 0xe8
#define FB_COLOR_HOT_G 0x3a
#define FB_COLOR_HOT_B 0x8a

#define FB_COLOR_DIM_R 0x50
#define FB_COLOR_DIM_G 0x56
#define FB_COLOR_DIM_B 0x60

/* ASCII luminance ramp, dark to bright, for shaded effects (sphere, etc). */
#define FB_SHADE_RAMP " .:-=+*#%@"

#endif /* FB_PALETTE_H */
