#include "test_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "calibr/calibr_log_tail.h"
#include "calibr/calibr_results.h"
#include "util/paths.h"

/* Mirrors tests/fixtures/recommendation.sample.json (kept as a standalone,
   human-readable reference); embedded directly here so the test doesn't
   depend on relative-path resolution from wherever ctest happens to run
   the binary from. */
static const char *kSampleRecommendation =
    "{"
    "\"status\": \"winner_selected\","
    "\"reason\": \"unique-frontier-point\","
    "\"category\": \"best-balanced-config\","
    "\"winner\": {"
    "  \"id\": \"qwen2.5-7b-instruct.q4_k_m.ctx32768\","
    "  \"model\": \"qwen2.5-7b-instruct\","
    "  \"eval_tps\": 42.7,"
    "  \"vram_peak_mib\": 5820,"
    "  \"shared_peak_mib\": 0"
    "},"
    "\"candidates\": ["
    "  { \"id\": \"a\", \"eval_tps\": 42.7 },"
    "  { \"id\": \"b\", \"eval_tps\": 31.2 }"
    "],"
    "\"constraints\": { \"eval_floor_profile\": \"chat\", \"context_floor\": null }"
    "}";

static int textview_contains(const FbTextView *tv, const char *needle) {
    for (int i = 0; i < tv->count; ++i) {
        if (strstr(tv->lines[i], needle)) return 1;
    }
    return 0;
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    FB_CHECK(f != NULL);
    fputs(content, f);
    fclose(f);
}

int main(void) {
    char root[512];
    snprintf(root, sizeof(root), "%s", "test_calibr_root_tmp");

    char current_dir[600], runs_dir[600], log_dir[600];
    snprintf(current_dir, sizeof(current_dir), "%s/results/current", root);
    snprintf(runs_dir, sizeof(runs_dir), "%s/results/runs", root);
    snprintf(log_dir, sizeof(log_dir), "%s/log", root);

    fb_ensure_dir(root);
    char results_dir[600];
    snprintf(results_dir, sizeof(results_dir), "%s/results", root);
    fb_ensure_dir(results_dir);
    fb_ensure_dir(current_dir);
    fb_ensure_dir(runs_dir);
    fb_ensure_dir(log_dir);

    char rec_path[700];
    snprintf(rec_path, sizeof(rec_path), "%s/recommendation.json", current_dir);
    write_file(rec_path, kSampleRecommendation);

    char run1[700], run2[700];
    snprintf(run1, sizeof(run1), "%s/run-a.json", runs_dir);
    snprintf(run2, sizeof(run2), "%s/run-b.json", runs_dir);
    write_file(run1, "{}");
    write_file(run2, "{}");

    /* Heap-allocated, not stack locals: FbTextView embeds a 2000x220-byte
       line buffer (~440KB) -- three of them as plain `main()` locals would
       comfortably blow a default 1MB thread stack (confirmed the hard way:
       this segfaulted before this fix). */
    FbTextView *leaderboard = (FbTextView *)malloc(sizeof(FbTextView));
    FbTextView *unconfigured = (FbTextView *)malloc(sizeof(FbTextView));
    FbTextView *logs = (FbTextView *)malloc(sizeof(FbTextView));
    FB_CHECK(leaderboard && unconfigured && logs);

    /* --- leaderboard: generic renderer, tolerant of any shape --- */
    fb_calibr_load_leaderboard(leaderboard, root);
    FB_CHECK(leaderboard->count > 0);
    FB_CHECK(textview_contains(leaderboard, "winner_selected"));
    FB_CHECK(textview_contains(leaderboard, "best-balanced-config"));
    FB_CHECK(textview_contains(leaderboard, "42.7"));
    FB_CHECK(textview_contains(leaderboard, "qwen2.5-7b-instruct"));
    FB_CHECK(textview_contains(leaderboard, "raw results (2 files)"));
    FB_CHECK(textview_contains(leaderboard, "run-a.json"));
    FB_CHECK(textview_contains(leaderboard, "run-b.json"));

    /* Missing calibr root: still produces an explanatory line, never empty/garbage. */
    fb_calibr_load_leaderboard(unconfigured, "");
    FB_CHECK(unconfigured->count == 1);
    FB_CHECK(textview_contains(unconfigured, "calibr_root"));

    /* --- log tail: incremental polling, no duplicate re-reads --- */
    char log_path[700];
    snprintf(log_path, sizeof(log_path), "%s/action-trace.jsonl", log_dir);
    write_file(log_path,
               "{\"ts\":\"T1\",\"source\":\"cli\",\"flow\":\"guided\",\"action\":\"discover\","
               "\"status\":\"started\",\"message\":\"scanning\"}\n");

    FbCalibrLogTail tail;
    fb_calibr_log_tail_init(&tail);
    fb_textview_clear(logs);

    fb_calibr_log_tail_poll(&tail, root, logs);
    FB_CHECK(logs->count == 1);
    FB_CHECK(textview_contains(logs, "discover"));
    FB_CHECK(textview_contains(logs, "scanning"));

    fb_calibr_log_tail_poll(&tail, root, logs);
    FB_CHECK(logs->count == 1); /* nothing new appended: no duplicate line */

    FILE *f = fopen(log_path, "ab");
    FB_CHECK(f != NULL);
    fputs("{\"ts\":\"T2\",\"source\":\"engine\",\"flow\":\"guided\",\"action\":\"bench\","
          "\"status\":\"completed\",\"message\":\"done\"}\n",
          f);
    fclose(f);

    fb_calibr_log_tail_poll(&tail, root, logs);
    FB_CHECK(logs->count == 2);
    FB_CHECK(textview_contains(logs, "bench"));
    FB_CHECK(textview_contains(logs, "done"));

    free(leaderboard);
    free(unconfigured);
    free(logs);

    printf("test_calibr_results: OK\n");
    return 0;
}
