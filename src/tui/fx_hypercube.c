#include "tui/fx_hypercube.h"

#include <math.h>
#include <stdlib.h>

#include "tui/palette.h"
#include "tui/screen.h"

#define FB_PI 3.14159265358979323846
#define FB_HYPERCUBE_VERTS 16

static void draw_line(FbRect area, int x0, int y0, int x1, int y1, uint8_t r, uint8_t g,
                       uint8_t b) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    FbCell cell;
    cell.ch = '#';
    cell.fg_r = r; cell.fg_g = g; cell.fg_b = b;
    cell.bg_r = FB_COLOR_BG_R; cell.bg_g = FB_COLOR_BG_G; cell.bg_b = FB_COLOR_BG_B;
    cell.bold = 0;

    for (;;) {
        if (x0 >= 0 && x0 < area.w && y0 >= 0 && y0 < area.h) {
            fb_screen_set_cell(area.x + x0, area.y + y0, &cell);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void fb_fx_hypercube_render(FbRect area, unsigned long long elapsed_ms, uint8_t r, uint8_t g,
                             uint8_t b) {
    if (area.w <= 0 || area.h <= 0) return;

    double t = (double)elapsed_ms / 1000.0;
    float ang_xw = (float)(t * 0.5);
    float ang_yz = (float)(t * 0.33);
    float cos_xw = cosf(ang_xw), sin_xw = sinf(ang_xw);
    float cos_yz = cosf(ang_yz), sin_yz = sinf(ang_yz);

    /* The 16 vertices of a tesseract: every combination of +-1 in 4
       dimensions. Two vertices form an edge iff their coordinates differ
       in exactly one component (Hamming distance 1 on the bit pattern
       below) -- 32 edges total, checked directly in the draw loop rather
       than precomputed, since 16*16 is nothing to iterate every frame. */
    float verts4[FB_HYPERCUBE_VERTS][4];
    for (int v = 0; v < FB_HYPERCUBE_VERTS; ++v) {
        for (int k = 0; k < 4; ++k) verts4[v][k] = (v & (1 << k)) ? 1.0f : -1.0f;
    }

    int sx[FB_HYPERCUBE_VERTS], sy[FB_HYPERCUBE_VERTS];
    float depth[FB_HYPERCUBE_VERTS];

    float cx = (float)area.w / 2.0f;
    float cy = (float)area.h / 2.0f;
    float scale = (float)(area.h < area.w / 2 ? area.h : area.w / 2) * 0.4f;
    float w_viewer = 2.2f;  /* 4D -> 3D perspective distance */
    float z_viewer = 3.0f;  /* 3D -> 2D perspective distance */

    for (int v = 0; v < FB_HYPERCUBE_VERTS; ++v) {
        float x = verts4[v][0], y = verts4[v][1], z = verts4[v][2], w = verts4[v][3];

        /* Rotate in the XW plane, then the YZ plane -- two independent
           planes at different speeds is what makes it read as tumbling in
           4D rather than just spinning a 3D cube. */
        float x1 = x * cos_xw - w * sin_xw;
        float w1 = x * sin_xw + w * cos_xw;
        float y1 = y * cos_yz - z * sin_yz;
        float z1 = y * sin_yz + z * cos_yz;

        float wf = w_viewer / (w_viewer - w1);
        float x3 = x1 * wf, y3 = y1 * wf, z3 = z1 * wf;

        float zf = z_viewer / (z_viewer - z3);
        float x2 = x3 * zf, y2 = y3 * zf;

        /* Aspect-correct, same reasoning as fx_sphere.c: character cells
           read roughly twice as tall as wide. */
        sx[v] = (int)(cx + scale * 2.0f * x2);
        sy[v] = (int)(cy - scale * y2);
        depth[v] = w1 + z3; /* rough combined depth cue for shading */
    }

    float min_d = depth[0], max_d = depth[0];
    for (int v = 1; v < FB_HYPERCUBE_VERTS; ++v) {
        if (depth[v] < min_d) min_d = depth[v];
        if (depth[v] > max_d) max_d = depth[v];
    }
    float range = (max_d - min_d) > 0.01f ? (max_d - min_d) : 1.0f;

    for (int i = 0; i < FB_HYPERCUBE_VERTS; ++i) {
        for (int j = i + 1; j < FB_HYPERCUBE_VERTS; ++j) {
            int diff = i ^ j;
            int bits = 0;
            for (int k = 0; k < 4; ++k) bits += (diff >> k) & 1;
            if (bits != 1) continue;

            float shade = 0.4f + 0.6f * (((depth[i] + depth[j]) / 2.0f - min_d) / range);
            uint8_t lr = (uint8_t)(r * shade);
            uint8_t lg = (uint8_t)(g * shade);
            uint8_t lb = (uint8_t)(b * shade);
            draw_line(area, sx[i], sy[i], sx[j], sy[j], lr, lg, lb);
        }
    }
}
