#ifndef FB_DIARY_H
#define FB_DIARY_H

#include <stddef.h>

#include "diary/diary_job.h"

/*
 * Diary mode's memory pipeline: an open "page" (raw recent turns) closes on
 * whichever comes first -- idle timeout, raw size, or an explicit manual
 * close -- and closing hands the page to a background consolidation
 * ("sleep") pass that distills it into long-term memory. Long-term memory
 * itself gets periodically re-summarized against itself (a "checkpoint")
 * once it grows past a size threshold, keeping it from growing without
 * bound. Both consolidation steps run through diary_job.h against the
 * always-resident Qwen brain -- fixed choice, not the mode's live chat
 * model, so consolidation stays cheap regardless of what you're chatting
 * with. Closed pages are archived to disk (not discarded), one plain-text
 * file per page under <data_dir>/diary/pages/ (page content is already
 * just formatted turn text, no structure worth wrapping in JSON);
 * long-term memory is a single text file, <data_dir>/diary/long_term.txt.
 *
 * Every real consolidation (never a --sweep-summarize dry run) also
 * appends one line to <data_dir>/diary/memory_log.jsonl -- a plain
 * provenance log, purely additive and never read by the live pipeline
 * itself, that exists so fb_diary_build_source_tree (and --sweep-summarize)
 * can answer "what actually fed into the current long-term memory": which
 * pages, and which earlier checkpoint passes folded which earlier entries.
 * Repeatedly checkpointing a checkpoint's own output with no way back to
 * source material is exactly the kind of self-referential drift that
 * produces ungrounded, increasingly abstract output from a small model --
 * this log is what makes that visible instead of invisible.
 */

/* Hard buffer-size caps -- page_buf/long_term are fixed-size arrays sized
   to these, so they can never change at runtime. The *actual* trigger
   thresholds below are runtime fields on FbDiary, configurable (see
   config.h's diary_page_close_chars/diary_checkpoint_chars/
   diary_idle_close_minutes) but always clamped to fit inside these caps,
   so a bad config value can't overflow a buffer. */
#define FB_DIARY_PAGE_MAX_CHARS 8000
#define FB_DIARY_LTM_MAX_CHARS 16000

/* Defaults used when the matching config.json field is 0 ("unset"). */
#define FB_DIARY_DEFAULT_PAGE_CLOSE_CHARS 8000
#define FB_DIARY_DEFAULT_CHECKPOINT_CHARS 5000
#define FB_DIARY_DEFAULT_IDLE_CLOSE_MS (10ULL * 60ULL * 1000ULL) /* 10 minutes */

typedef enum {
    FB_DIARY_TASK_NONE = 0,
    FB_DIARY_TASK_SLEEP,      /* closed page -> long-term memory */
    FB_DIARY_TASK_CHECKPOINT  /* long-term memory -> a more compact version of itself */
} FbDiaryTask;

typedef struct {
    char root_dir[512]; /* <data_dir>/diary */
    int qwen_port;      /* 0 if the brain isn't available -- consolidation is then a no-op */

    /* Runtime trigger thresholds -- see fb_diary_init's overrides. Always
       clamped to fit within FB_DIARY_PAGE_MAX_CHARS/FB_DIARY_LTM_MAX_CHARS. */
    size_t page_close_chars;
    size_t ltm_checkpoint_chars;
    unsigned long long idle_close_ms;

    char page_buf[FB_DIARY_PAGE_MAX_CHARS + 512];
    size_t page_len;
    int page_has_content;
    unsigned long long last_activity_ms;

    char long_term[FB_DIARY_LTM_MAX_CHARS];
    size_t long_term_len;

    int archived_count; /* total closed pages, on disk; also next page's id */
    int browse_index;   /* -1 = live; 0..archived_count-1 = read-only viewing that archived page */

    /* -1 = page_buf is a fresh page, not yet on disk -- the normal case;
       0..archived_count-1 = page_buf is a reopened archived page being
       edited/extended in place -- the next close overwrites that page's
       own file instead of allocating a new one. Set by fb_diary_reopen_page,
       cleared back to -1 by fb_diary_close_page once it's saved. */
    int edit_target;

    FbDiaryJob job;
    FbDiaryTask job_task; /* what the in-flight (or just-finished) job is doing */
    char job_target_page[FB_DIARY_PAGE_MAX_CHARS + 512]; /* snapshot of the page being slept on */
    int job_target_page_index; /* which page index that snapshot is, for the provenance log */

    /* Save confirmation, for the UI: last_saved_ms is a persistent "how
       long ago" indicator (0 if nothing has ever been saved this run);
       just_saved is a one-shot flag the caller should check once per
       fb_diary_tick call and clear after showing a confirmation message --
       it's set the instant a save actually completes, not on a timer. */
    unsigned long long last_saved_ms;
    int just_saved;
} FbDiary;

/* Loads long_term.txt and counts existing archived pages if data_dir/diary
   already exists (a previous run); otherwise starts empty. qwen_port == 0
   is valid (brain not configured) -- pages still accumulate and archive,
   they just never get consolidated until a brain becomes available.
   page_close_chars_cfg/checkpoint_chars_cfg/idle_close_minutes_cfg are the
   matching config.json overrides, or 0 to use the built-in defaults;
   whatever the effective value is, it's clamped to fit inside
   FB_DIARY_PAGE_MAX_CHARS/FB_DIARY_LTM_MAX_CHARS. */
void fb_diary_init(FbDiary *d, const char *data_dir, int qwen_port, int page_close_chars_cfg,
                    int checkpoint_chars_cfg, int idle_close_minutes_cfg);

/* Appends one turn to the current open page ("You: ..." / "<model>: ...",
   already-formatted text) and refreshes the idle clock to now_ms. May close
   the page immediately (starting consolidation) if this append crosses
   d->page_close_chars. */
void fb_diary_append_turn(FbDiary *d, const char *role, const char *text,
                           unsigned long long now_ms);

/* Closes the current page right now if it has any content, starting
   consolidation immediately rather than waiting for idle/size. No-op if
   the page is empty or a consolidation/checkpoint job is already running
   (the close is simply skipped that call; the page keeps accumulating and
   will be retried on the next trigger). */
void fb_diary_close_page(FbDiary *d);

/* Call once per frame. Closes the page on idle timeout, and advances/
   completes any in-flight sleep or checkpoint job -- on a completed sleep,
   folds the result into long-term memory (and triggers a checkpoint if
   that crosses d->ltm_checkpoint_chars); on a completed checkpoint,
   replaces long-term memory with the compacted result. Persists whatever
   changed to disk, setting just_saved and last_saved_ms when it does --
   check just_saved after calling this and clear it once you've shown a
   confirmation, or it'll report the same save again next frame. */
void fb_diary_tick(FbDiary *d, unsigned long long now_ms);

/* Alt+Left (delta<0) / Alt+Right (delta>0): moves the browse cursor among
   archived pages. Clamped to [-1, archived_count-1]; -1 is "live" (not
   browsing). */
void fb_diary_browse(FbDiary *d, int delta);

/* Loads the browsed archived page's raw text into out (bounded by out_cap).
   Returns 1 if d->browse_index names a real page, 0 if browse_index is -1
   (live) or the file is missing/unreadable. */
int fb_diary_read_browsed_page(const FbDiary *d, char *out, size_t out_cap);

/* Makes archived page `index` the live, editable page: closes whatever's
   currently open first (safely archiving it to its own file, untouched --
   it stays fully intact and separate, just no longer "open"), then loads
   `index`'s existing text back into page_buf so new turns append directly
   to it. The next close overwrites that same page's file in place rather
   than allocating a new one. Also clears d->browse_index (this is the live
   page now, not a read-only view). Returns 1 on success, 0 if `index` isn't
   a real archived page or a consolidation job is currently running (retry
   once fb_diary_busy() is false). */
int fb_diary_reopen_page(FbDiary *d, int index);

/* True while a sleep or checkpoint job is running -- for a status spinner. */
int fb_diary_busy(const FbDiary *d);

void fb_diary_shutdown(FbDiary *d);

/* Renders an ASCII tree into out (bounded by out_cap) showing which pages
   and prior consolidation passes fed into the *current* long-term memory,
   read straight from <data_dir>/diary/memory_log.jsonl -- no live FbDiary
   needed (the --sweep-summarize CLI tool has none, it only has a
   data_dir). Safe to call even if nothing has ever been consolidated (the
   log doesn't exist yet): out gets a plain "nothing consolidated yet"
   line, not an error. Returns the number of top-level (currently live)
   entries found, 0 either way (empty log or genuinely no live entries). */
int fb_diary_build_source_tree(const char *data_dir, char *out, size_t out_cap);

/* Recursively expands the log's current live entries down to their leaf
   pages -- every archived page that ultimately contributes to current
   long-term memory, however many checkpoint layers deep, deduplicated.
   For --sweep-summarize --experimental's ltm source: reconstructing the
   actual page-level breakdown a proper hierarchical (per-page, then
   blended) synthesis needs, rather than the flat already-blended
   long_term.txt text. Returns the count found (out_pages filled, bounded
   by max_pages), 0 if the log doesn't exist yet or nothing is live (not
   an error -- the caller falls back to long_term.txt directly). */
int fb_diary_get_live_pages(const char *data_dir, int *out_pages, int max_pages);

#endif /* FB_DIARY_H */
