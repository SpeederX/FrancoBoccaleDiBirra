#include "tui/fx_waveform.h"

#include <string.h>

#include "tui/palette.h"
#include "tui/screen.h"

void fb_fx_waveform_render(FbRect area, const float *samples, int sample_count) {
    if (area.w <= 0 || area.h <= 0) return;

    const char *ramp = FB_SHADE_RAMP;
    int ramp_len = (int)strlen(ramp);

    for (int col = 0; col < area.w; ++col) {
        /* Map this column to the most recent `area.w` samples, newest on
           the right. */
        int sample_idx = sample_count - area.w + col;
        float amp = (sample_idx >= 0 && sample_idx < sample_count) ? samples[sample_idx] : 0.0f;
        if (amp < 0.0f) amp = 0.0f;
        if (amp > 1.0f) amp = 1.0f;

        float filled = amp * (float)area.h;
        int full_rows = (int)filled;
        float frac = filled - (float)full_rows;

        for (int row = 0; row < area.h; ++row) {
            int from_bottom = area.h - 1 - row;
            FbCell cell;
            cell.bg_r = FB_COLOR_BG_R; cell.bg_g = FB_COLOR_BG_G; cell.bg_b = FB_COLOR_BG_B;
            cell.bold = 0;

            if (from_bottom < full_rows) {
                cell.ch = (unsigned char)ramp[ramp_len - 1];
                cell.fg_r = FB_COLOR_ACCENT_R; cell.fg_g = FB_COLOR_ACCENT_G; cell.fg_b = FB_COLOR_ACCENT_B;
            } else if (from_bottom == full_rows && frac > 0.0f) {
                int idx = (int)(frac * (float)(ramp_len - 1));
                if (idx >= ramp_len) idx = ramp_len - 1;
                cell.ch = (unsigned char)ramp[idx];
                cell.fg_r = FB_COLOR_ACCENT_R; cell.fg_g = FB_COLOR_ACCENT_G; cell.fg_b = FB_COLOR_ACCENT_B;
            } else {
                cell.ch = ' ';
                cell.fg_r = FB_COLOR_BG_R; cell.fg_g = FB_COLOR_BG_G; cell.fg_b = FB_COLOR_BG_B;
            }

            fb_screen_set_cell(area.x + col, area.y + row, &cell);
        }
    }
}
