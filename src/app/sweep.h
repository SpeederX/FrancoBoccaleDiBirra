#ifndef FB_SWEEP_H
#define FB_SWEEP_H

/*
 * --sweep-summarize: a dev-only CLI mode, entered only when fbdb.exe is
 * invoked with that flag as argv[1] (see main.c) -- the normal TUI never
 * touches this file. It runs the *same* summarization instruction the live
 * diary pipeline uses (see diary_job.h) against a list of models, one at a
 * time (load, send, record, unload), and writes a single self-contained
 * local HTML report comparing them: one tab per model, the original text
 * next to that model's summary, plus -- for --source=ltm -- the
 * provenance tree of what actually fed into current long-term memory (see
 * diary.h's fb_diary_build_source_tree). Built for exactly the question
 * "is Qwen bad at this specifically, or would a bigger model do better" --
 * answerable by eye without touching the live pipeline at all.
 *
 * Usage:
 *   fbdb.exe --sweep-summarize --source=<page:N|ltm|all|test:N|test:all|file:PATH>
 *            [--models=<name1,name2,...>|all] [--out=DIR] [--no-open]
 *            [--experimental] [--load-timeout=<seconds>]
 *
 * --load-timeout (default 180s) is how long to wait for a model to finish
 * loading before giving up on it -- confirmed the hard way that 60s isn't
 * enough for a large model needing heavy CPU/mmap offload on a small GPU;
 * a report entry from a timeout says so explicitly ("still loading...not
 * crashed, just slower than the timeout") rather than reading the same as
 * an actual crash, and the process is stopped either way before moving to
 * the next model so it can't keep competing for VRAM in the background.
 *
 * --source=page:N and --source=ltm read straight from this install's own
 * diary data (<data_dir>/diary/...) using the real sleep/checkpoint
 * instructions; --source=all sweeps every archived page plus long-term
 * memory in one run; --source=test:N (or test:all) runs a fixed set of
 * synthetic scenarios (FB_SWEEP_TEST_SCENARIOS in sweep.c), each written
 * to probe one specific instruction rule with a known-correct outcome --
 * independent of whatever's actually archived, for regression-testing
 * instruction wording changes; --source=file:PATH summarizes arbitrary
 * text with a generic instruction. --models (--model also accepted, a
 * natural typo when naming just one) defaults to every model in
 * config.json; any unrecognized --flag prints a warning rather than
 * being silently ignored.
 * --out is a directory (defaults under <data_dir>/diary/sweeps/, always a
 * fresh timestamped one when --out is omitted -- so an --experimental run
 * and a normal run never collide, each in its own directory ready to
 * compare); a report lands there per source, plus an index.html when
 * there's more than one. Opens the result in the default browser when
 * done unless --no-open is given -- each model is loaded once and reused
 * across every source before being unloaded, not reloaded per source.
 *
 * --experimental swaps in an alternate sleep/checkpoint instruction pair
 * (see sweep.c) that isn't wired into the live pipeline anywhere -- purely
 * for comparing against production in its own report, never affecting
 * long_term.txt or memory_log.jsonl. For --source=ltm specifically, it
 * also tries to synthesize from each individual contributing page's raw
 * text (via the provenance log) instead of re-compressing long_term.txt's
 * own already-flattened output -- degrading to that flat read, clearly
 * labeled, if the log has nothing live yet (data predating the log).
 *
 * Never touches long_term.txt, archived_count, or memory_log.jsonl --
 * purely a side computation over a snapshot of whatever source text was
 * asked for, run against models loaded and unloaded just for this sweep.
 */

/* argv is everything *after* the "--sweep-summarize" token itself (main.c
   strips it). Returns the process exit code (0 on success). */
int fb_sweep_run(int argc, char **argv);

#endif /* FB_SWEEP_H */
