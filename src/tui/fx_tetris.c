#include "tui/fx_tetris.h"

#include <stdlib.h>

#include "tui/palette.h"
#include "tui/screen.h"

#define FB_TETRIS_MAX_COLS 60
#define FB_TETRIS_COL_W 4
#define FB_TETRIS_FALL_ROWS_PER_SEC 6.0f
#define FB_TETRIS_SPAWN_INTERVAL_MS 700

typedef struct {
    int col_height[FB_TETRIS_MAX_COLS];
    int num_cols;
    int area_w_seen, area_h_seen;

    int has_falling;
    int falling_col;
    int falling_height;
    unsigned long long fall_start_ms;

    unsigned long long next_spawn_ms;
    int seeded;
} FbTetrisState;

static FbTetrisState g_state;

static float frand(void) { return (float)rand() / (float)RAND_MAX; }

static void reset_columns(FbTetrisState *st, int area_w, int area_h) {
    st->num_cols = area_w / FB_TETRIS_COL_W;
    if (st->num_cols < 1) st->num_cols = 1;
    if (st->num_cols > FB_TETRIS_MAX_COLS) st->num_cols = FB_TETRIS_MAX_COLS;
    for (int i = 0; i < FB_TETRIS_MAX_COLS; ++i) st->col_height[i] = 0;
    st->area_w_seen = area_w;
    st->area_h_seen = area_h;
    st->has_falling = 0;
}

static void draw_block(FbRect area, int col, int col_w, int row_from_bottom, int rows,
                        uint8_t r, uint8_t g, uint8_t b, int bold) {
    for (int dy = 0; dy < rows; ++dy) {
        int sy = area.h - 1 - (row_from_bottom + dy);
        if (sy < 0 || sy >= area.h) continue;
        for (int dx = 0; dx < col_w - 1; ++dx) {
            int sx = col * col_w + dx;
            if (sx < 0 || sx >= area.w) continue;
            FbCell cell;
            cell.ch = '#';
            cell.fg_r = r; cell.fg_g = g; cell.fg_b = b;
            cell.bg_r = FB_COLOR_BG_R; cell.bg_g = FB_COLOR_BG_G; cell.bg_b = FB_COLOR_BG_B;
            cell.bold = (uint8_t)bold;
            fb_screen_set_cell(area.x + sx, area.y + sy, &cell);
        }
    }
}

void fb_fx_tetris_render(FbRect area, unsigned long long elapsed_ms, uint8_t r, uint8_t g,
                          uint8_t b) {
    if (area.w <= 0 || area.h <= 0) return;

    if (!g_state.seeded || g_state.area_w_seen != area.w || g_state.area_h_seen != area.h) {
        reset_columns(&g_state, area.w, area.h);
        g_state.next_spawn_ms = elapsed_ms + FB_TETRIS_SPAWN_INTERVAL_MS;
        g_state.seeded = 1;
    }

    if (!g_state.has_falling && elapsed_ms >= g_state.next_spawn_ms) {
        g_state.has_falling = 1;
        g_state.falling_col = (int)(frand() * (float)g_state.num_cols);
        if (g_state.falling_col >= g_state.num_cols) g_state.falling_col = g_state.num_cols - 1;
        g_state.falling_height = 1 + (int)(frand() * 3.0f);
        if (g_state.falling_height > 3) g_state.falling_height = 3;
        g_state.fall_start_ms = elapsed_ms;
    }

    if (g_state.has_falling) {
        int col = g_state.falling_col;
        int landing_row = area.h - g_state.col_height[col] - g_state.falling_height;
        if (landing_row < 0) landing_row = 0; /* column already full: land immediately */

        float fall_secs = (float)(elapsed_ms - g_state.fall_start_ms) / 1000.0f;
        int cur_row_from_top = (int)(fall_secs * FB_TETRIS_FALL_ROWS_PER_SEC);

        if (cur_row_from_top >= landing_row) {
            g_state.col_height[col] += g_state.falling_height;
            g_state.has_falling = 0;
            g_state.next_spawn_ms = elapsed_ms + FB_TETRIS_SPAWN_INTERVAL_MS;
            if (g_state.col_height[col] >= area.h) {
                /* Topped out: clear the board and start piling up again,
                   the same shape as a real board topping out and resetting. */
                reset_columns(&g_state, area.w, area.h);
                g_state.next_spawn_ms = elapsed_ms + FB_TETRIS_SPAWN_INTERVAL_MS;
            }
        } else {
            int row_from_bottom = area.h - g_state.falling_height - cur_row_from_top;
            draw_block(area, col, FB_TETRIS_COL_W, row_from_bottom, g_state.falling_height, r, g,
                       b, 1);
        }
    }

    for (int c = 0; c < g_state.num_cols; ++c) {
        int h = g_state.col_height[c];
        if (h <= 0) continue;
        float shade = 0.55f;
        uint8_t sr = (uint8_t)(FB_COLOR_DIM_R + shade * (r - FB_COLOR_DIM_R));
        uint8_t sg = (uint8_t)(FB_COLOR_DIM_G + shade * (g - FB_COLOR_DIM_G));
        uint8_t sb = (uint8_t)(FB_COLOR_DIM_B + shade * (b - FB_COLOR_DIM_B));
        draw_block(area, c, FB_TETRIS_COL_W, 0, h, sr, sg, sb, 0);
    }
}
