#ifndef FB_CALIBR_RESULTS_H
#define FB_CALIBR_RESULTS_H

#include "tui/widget_textview.h"

/*
 * Reads <calibr_root>/results/current/recommendation.json and renders it
 * generically into tv (cleared first) as indented key:value lines, then
 * appends a "raw results (N files)" section listing every file under
 * results/runs/. Deliberately generic/tolerant rather than keyed to a
 * specific set of fields or category names: calibr's schema is actively
 * evolving upstream (a live feat/category-registry branch exists), and
 * this console has no real calibr output to develop against yet on this
 * machine, so a fixed struct would be guessing at a moving target.
 * Always leaves tv with at least one explanatory line, even when nothing
 * is configured/found/parseable — never an empty or garbage view.
 */
void fb_calibr_load_leaderboard(FbTextView *tv, const char *calibr_root);

#endif /* FB_CALIBR_RESULTS_H */
