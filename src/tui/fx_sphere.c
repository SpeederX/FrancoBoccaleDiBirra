#include "tui/fx_sphere.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tui/palette.h"
#include "tui/screen.h"

/* M_PI is a POSIX/BSD extension, not guaranteed by C11 <math.h> (MSVC only
   defines it behind _USE_MATH_DEFINES) — define our own instead. */
#define FB_PI 3.14159265358979323846

static float *g_zbuf = NULL;
static int g_zbuf_w = 0, g_zbuf_h = 0;

static void ensure_zbuf(int w, int h) {
    if (w == g_zbuf_w && h == g_zbuf_h && g_zbuf) return;
    free(g_zbuf);
    g_zbuf_w = w;
    g_zbuf_h = h;
    g_zbuf = (float *)malloc((size_t)w * (size_t)h * sizeof(float));
}

void fb_fx_sphere_render(FbRect area, unsigned long long elapsed_ms, float intensity) {
    if (area.w <= 0 || area.h <= 0) return;
    if (intensity < 0.0f) intensity = 0.0f;
    if (intensity > 1.0f) intensity = 1.0f;

    ensure_zbuf(area.w, area.h);
    if (!g_zbuf) return;
    for (int i = 0; i < area.w * area.h; ++i) g_zbuf[i] = 0.0f;

    double t = (double)elapsed_ms / 1000.0;
    float spin = (float)(t * 0.6);       /* rotation around Y: steady spin */
    float wobble = (float)(sin(t * 0.35) * 0.5); /* rotation around X: gentle wobble */
    float pulse = 1.0f + (0.05f + 0.10f * intensity) *
                              (float)sin(t * (2.0 + 4.0 * intensity));

    float cx = (float)area.w / 2.0f;
    float cy = (float)area.h / 2.0f;
    float radius_cells = (float)(area.h < area.w / 2 ? area.h : area.w / 2) * 0.85f;
    float k2 = 4.0f; /* viewer distance */
    float k1 = radius_cells * k2 * 0.6f;

    /* Light from upper-front-left, fixed. */
    float lx = -0.4f, ly = 0.6f, lz = -0.7f;
    float llen = sqrtf(lx * lx + ly * ly + lz * lz);
    lx /= llen; ly /= llen; lz /= llen;

    const char *ramp = FB_SHADE_RAMP;
    int ramp_len = (int)strlen(ramp);

    float cos_b = cosf(spin), sin_b = sinf(spin);
    float cos_a = cosf(wobble), sin_a = sinf(wobble);

    float theta_step = 0.07f;
    float phi_step = 0.02f;

    for (float theta = 0.0f; theta < (float)FB_PI; theta += theta_step) {
        float st = sinf(theta), ct = cosf(theta);
        for (float phi = 0.0f; phi < 2.0f * (float)FB_PI; phi += phi_step) {
            float px = st * cosf(phi);
            float py = st * sinf(phi);
            float pz = ct;

            /* rotate around Y (spin) */
            float x1 = px * cos_b + pz * sin_b;
            float y1 = py;
            float z1 = -px * sin_b + pz * cos_b;

            /* rotate around X (wobble) */
            float x2 = x1;
            float y2 = y1 * cos_a - z1 * sin_a;
            float z2 = y1 * sin_a + z1 * cos_a;

            x2 *= pulse; y2 *= pulse; z2 *= pulse;

            float ooz = 1.0f / (k2 + z2);
            /* aspect-correct: character cells are roughly twice as tall as
               wide, so the horizontal extent needs a larger scale to read
               as a circle rather than an egg. */
            int sx = (int)(cx + k1 * 2.0f * ooz * x2);
            int sy = (int)(cy - k1 * ooz * y2);

            if (sx < 0 || sx >= area.w || sy < 0 || sy >= area.h) continue;

            int idx = sy * area.w + sx;
            if (ooz <= g_zbuf[idx]) continue;
            g_zbuf[idx] = ooz;

            float lum = x2 * lx + y2 * ly + z2 * lz; /* normal == point, unit sphere at origin */
            if (lum < 0.0f) lum = 0.0f;

            int ramp_idx = (int)(lum * (float)(ramp_len - 1));
            if (ramp_idx >= ramp_len) ramp_idx = ramp_len - 1;

            float base_r = FB_COLOR_ACCENT_R + intensity * (FB_COLOR_HOT_R - FB_COLOR_ACCENT_R);
            float base_g = FB_COLOR_ACCENT_G + intensity * (FB_COLOR_HOT_G - FB_COLOR_ACCENT_G);
            float base_b = FB_COLOR_ACCENT_B + intensity * (FB_COLOR_HOT_B - FB_COLOR_ACCENT_B);
            float shade = 0.35f + 0.65f * lum;

            FbCell cell;
            cell.ch = (unsigned char)ramp[ramp_idx];
            cell.fg_r = (uint8_t)(base_r * shade);
            cell.fg_g = (uint8_t)(base_g * shade);
            cell.fg_b = (uint8_t)(base_b * shade);
            cell.bg_r = FB_COLOR_BG_R; cell.bg_g = FB_COLOR_BG_G; cell.bg_b = FB_COLOR_BG_B;
            cell.bold = 0;
            fb_screen_set_cell(area.x + sx, area.y + sy, &cell);
        }
    }
}
