#include "tui/fx_pencil.h"

#include <math.h>
#include <stdlib.h>

#include "tui/palette.h"
#include "tui/screen.h"

#define FB_PI 3.14159265358979323846

/* A ring buffer of recent pen positions (fractional 0..1 within the render
   area, so it stays valid across resizes) -- old marks drop off as new
   ones are added rather than the canvas filling in solid, which is what
   actually reads as "scratching," not just "scribbling once." */
#define FB_PENCIL_TRAIL_MAX 500
#define FB_PENCIL_STEP_MS 45

typedef struct {
    float x[FB_PENCIL_TRAIL_MAX];
    float y[FB_PENCIL_TRAIL_MAX];
    int count;
    int head; /* next slot to write once the ring is full */
    float pen_x, pen_y;
    float heading; /* radians */
    unsigned long long last_step_ms;
    int seeded;
} FbPencilState;

static FbPencilState g_state;

static float frand(void) { return (float)rand() / (float)RAND_MAX; }

static void pencil_step(FbPencilState *st) {
    /* A hand doodling wanders more than it commits to a straight line:
       small heading jitter every step, with an occasional sharp turn so
       the trail doesn't look like a single lazy spiral. */
    st->heading += (frand() - 0.5f) * 0.9f;
    if (frand() < 0.03f) st->heading += (frand() - 0.5f) * (float)FB_PI;

    float step_len = 0.02f + frand() * 0.02f;
    float nx = st->pen_x + cosf(st->heading) * step_len;
    float ny = st->pen_y + sinf(st->heading) * step_len * 2.0f; /* cells are ~2x taller than wide */

    /* Bounce off the edges instead of clamping, so the pen keeps moving
       rather than pinning itself to a wall. */
    if (nx < 0.0f) { nx = 0.0f; st->heading = (float)FB_PI - st->heading; }
    if (nx > 1.0f) { nx = 1.0f; st->heading = (float)FB_PI - st->heading; }
    if (ny < 0.0f) { ny = 0.0f; st->heading = -st->heading; }
    if (ny > 1.0f) { ny = 1.0f; st->heading = -st->heading; }

    st->pen_x = nx;
    st->pen_y = ny;

    int idx = (st->count < FB_PENCIL_TRAIL_MAX) ? st->count++ : st->head;
    if (st->count >= FB_PENCIL_TRAIL_MAX) st->head = (st->head + 1) % FB_PENCIL_TRAIL_MAX;
    st->x[idx] = nx;
    st->y[idx] = ny;
}

void fb_fx_pencil_render(FbRect area, unsigned long long elapsed_ms, uint8_t r, uint8_t g,
                          uint8_t b) {
    if (area.w <= 0 || area.h <= 0) return;

    if (!g_state.seeded) {
        g_state.pen_x = 0.5f;
        g_state.pen_y = 0.5f;
        g_state.heading = frand() * 2.0f * (float)FB_PI;
        g_state.last_step_ms = elapsed_ms;
        g_state.seeded = 1;
    }

    while (elapsed_ms - g_state.last_step_ms >= FB_PENCIL_STEP_MS) {
        pencil_step(&g_state);
        g_state.last_step_ms += FB_PENCIL_STEP_MS;
    }

    const char marks[] = {'.', '\'', '-', ',', '`'};
    for (int i = 0; i < g_state.count; ++i) {
        int sx = (int)(g_state.x[i] * (float)(area.w - 1));
        int sy = (int)(g_state.y[i] * (float)(area.h - 1));

        /* Fresher marks (closer to the write head) render brighter, like
           ink that hasn't faded yet. */
        int age = g_state.count - 1 - i;
        if (g_state.count >= FB_PENCIL_TRAIL_MAX) {
            age = (i - g_state.head + FB_PENCIL_TRAIL_MAX) % FB_PENCIL_TRAIL_MAX;
            age = FB_PENCIL_TRAIL_MAX - 1 - age;
        }
        float fade = 1.0f - (float)age / (float)FB_PENCIL_TRAIL_MAX;

        FbCell cell;
        cell.ch = (unsigned char)marks[i % (int)(sizeof(marks) / sizeof(marks[0]))];
        cell.fg_r = (uint8_t)(FB_COLOR_DIM_R + fade * (r - FB_COLOR_DIM_R));
        cell.fg_g = (uint8_t)(FB_COLOR_DIM_G + fade * (g - FB_COLOR_DIM_G));
        cell.fg_b = (uint8_t)(FB_COLOR_DIM_B + fade * (b - FB_COLOR_DIM_B));
        cell.bg_r = FB_COLOR_BG_R; cell.bg_g = FB_COLOR_BG_G; cell.bg_b = FB_COLOR_BG_B;
        cell.bold = 0;
        fb_screen_set_cell(area.x + sx, area.y + sy, &cell);
    }

    /* The pen tip itself, always fully bright, on top of the trail. */
    FbCell tip;
    tip.ch = '*';
    tip.fg_r = r; tip.fg_g = g; tip.fg_b = b;
    tip.bg_r = FB_COLOR_BG_R; tip.bg_g = FB_COLOR_BG_G; tip.bg_b = FB_COLOR_BG_B;
    tip.bold = 1;
    int tip_x = (int)(g_state.pen_x * (float)(area.w - 1));
    int tip_y = (int)(g_state.pen_y * (float)(area.h - 1));
    fb_screen_set_cell(area.x + tip_x, area.y + tip_y, &tip);
}
