#include "app/sweep.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend/backend_mgr.h"
#include "config/config.h"
#include "diary/diary.h"
#include "diary/diary_job.h"
#include "platform/open_url.h"
#include "platform/platform.h"
#include "util/fileutil.h"
#include "util/paths.h"

/* Splits a free-form "extra args" string (whitespace-separated, no quote
   support) into a NULL-terminated argv-style array of pointers *into*
   buf, which this mutates in place (strtok-style). Same tokenizer as
   command_dispatch.c's (not shared -- both are small, file-local
   utilities, consistent with this codebase's existing style elsewhere). */
static int tokenize_extra_args(char *buf, const char **out_argv, int max_argv) {
    int argc = 0;
    char *tok = strtok(buf, " \t");
    while (tok && argc < max_argv - 1) {
        out_argv[argc++] = tok;
        tok = strtok(NULL, " \t");
    }
    out_argv[argc] = NULL;
    return argc;
}

#define FB_SWEEP_MAX_MODELS FB_CONFIG_MAX_MODELS
#define FB_SWEEP_MAX_SOURCES 256
#define FB_SWEEP_MAX_LIVE_PAGES 64
#define FB_SWEEP_SOURCE_MAX (FB_DIARY_LTM_MAX_CHARS > 20000 ? FB_DIARY_LTM_MAX_CHARS : 20000)
/* Default only -- overridable per run via --load-timeout=<seconds>. 60s
   (the original hardcoded value) turned out to be nowhere near enough for
   a large model that needs heavy CPU/mmap offload on a small GPU --
   confirmed against a real 21GB model on an 8GB card: llama-server was
   still legitimately loading (no crash, no error, just still working)
   when the old fixed timeout killed it. 180s is still just a default,
   not a real bound on how long a big model can take. */
#define FB_SWEEP_DEFAULT_LOAD_TIMEOUT_MS 180000

/* --experimental: an alternate instruction pair, dev-only, not wired into
   the live pipeline anywhere -- purely for --sweep-summarize to compare
   against the production instructions (diary_job.h) in its own separate
   timestamped output directory. v2, revised against a three-way external
   cross-review (Astra/GPT Sol/Qwen3.8 Max) of the first draft:
   1. The user's own statements are ground truth; the assistant's
      inferences/guesses count only if the user actually confirmed them --
      staying silent or changing the subject is NOT agreement, and a
      partial correction keeps only the corrected part, not the original
      claim (this replaced v1's "note the specific part agreed with",
      which named the idea but didn't make silence-isn't-agreement
      explicit -- Astra's dreading-Monday example and GPT Sol's line
      "silence should never count as agreement" are why this changed).
   2. Importance is judged by "would this change a future conversation",
      not by repetition -- v1's checkpoint instruction used repeat-count
      as its importance signal ("appears across multiple pages is more
      likely to be... important"), which Astra correctly flagged: a
      consequential decision may be stated once, a passing irritation may
      occupy several pages. Dropped entirely, replaced with an explicit
      enduring/temporary/page-specific classification instead.
   3. Checkpoint here synthesizes from each individual page's raw text
      (see build_experimental_ltm_text) rather than re-compressing
      long_term.txt's own flat, already-blended prior output, and is now
      explicitly told to keep disagreements and corrections *visible*
      rather than merge everything into one flattened voice -- Astra:
      "'Blending' must leave disagreements and changes of mind visible
      where they matter." */
#define FB_DIARY_SLEEP_INSTRUCTION_V2                                                           \
    "You are a diary's memory consolidation process. Read the page below (a raw record of a "   \
    "conversation between the user and an assistant) and extract what's worth remembering "      \
    "long-term. Rules: 1) Ground truth is what the USER said -- their own facts, statements, "   \
    "decisions, and stated feelings; extract these first. 2) Never treat the assistant's "       \
    "inferences, guesses, or interpretations as fact just because they were stated "             \
    "confidently -- only keep an assistant claim if the user actually confirmed it, and only "   \
    "the part they confirmed. The user staying silent, moving on, or changing the subject is "   \
    "NOT agreement; do not record the assistant's claim as true in that case. 3) If the user "   \
    "corrected or partially rejected something the assistant said, record the correction, not "  \
    "the original claim. 4) Judge importance by whether remembering this would materially "      \
    "change a future conversation, never by how many times it was repeated -- a fact stated "    \
    "once (a decision, an event, a plan) can matter more than a feeling repeated several times; " \
    "a repeated complaint is usually a temporary emotional state, not a durable fact, and "      \
    "should be marked as such rather than inflated into one because it recurred. Write only "    \
    "the resulting summary, no preamble, no headers."

#define FB_DIARY_CHECKPOINT_INSTRUCTION_V2                                                      \
    "You are synthesizing several diary pages (each section below is one page's raw "           \
    "conversation, oldest first) into ONE coherent overview of what's durably true and "         \
    "important. Rules: 1) Prioritize the user's own established facts, decisions, and stated "   \
    "feelings over the assistant's phrasing -- the assistant's contributions matter only for "   \
    "concrete answers given and for what the user actually confirmed, never for confident-"      \
    "sounding inferences the user never agreed to. 2) Do not use repetition as a proxy for "     \
    "importance -- something mentioned once can matter more than something repeated. Judge "     \
    "each point by whether it would materially change how a future conversation should go, and " \
    "classify it as enduring, temporary, or specific to that page. 3) When pages disagree, or a " \
    "later page corrects or supersedes an earlier one, keep the correction and note that it "    \
    "changed, rather than silently merging both into one flattened claim -- this overview "      \
    "should stay revisable, not erase how understanding shifted. Write only the resulting "      \
    "overview, no preamble, no headers, no per-page breakdown -- one blended narrative that "    \
    "still shows what changed."

/* --source=test:N: fixed synthetic scenarios, each written to probe one
   specific rule above with a known-correct outcome -- independent of
   whatever's actually in this install's diary data, so the instructions
   can be regression-tested going forward regardless of what's been
   archived. Raw "You:"/"<model>:" format, matching real pages exactly. */
static const char *const FB_SWEEP_TEST_SCENARIOS[] = {
    /* 1: a fact stated once should outweigh a complaint repeated four
       times -- tests rule 4 (sleep) / rule 2 (checkpoint): importance by
       consequence, not by repeat count. */
    "You: I just accepted a new job offer at Meridian Labs, starting in three weeks.\n"
    "\n"
    "model: That's huge news, congratulations! How are you feeling about the transition?\n"
    "\n"
    "You: Honestly pretty stressed. My commute is going to be way longer.\n"
    "\n"
    "model: Long commutes can really wear you down over time. Have you thought about ways to "
    "make it more bearable?\n"
    "\n"
    "You: My commute is going to be so long, I really don't know how I'll survive that drive "
    "every single day.\n"
    "\n"
    "model: That does sound rough -- some people use that time for podcasts or audiobooks, "
    "might help it feel less wasted.\n"
    "\n"
    "You: Yeah my commute is definitely the worst part of this whole thing.\n",

    /* 2: the assistant offers a confident but wrong interpretation; the
       user explicitly corrects it. Tests rule 2/3 (sleep): keep the
       correction, not the original claim. */
    "You: I'm dreading Monday.\n"
    "\n"
    "model: It sounds like you might be losing interest in your job overall.\n"
    "\n"
    "You: No, it's not that -- I still like the work. It's just the presentation I have to "
    "give.\n"
    "\n"
    "model: That makes sense, presentations can be nerve-wracking regardless of how much you "
    "enjoy your role.\n"
    "\n"
    "You: Exactly. I actually really enjoy the day-to-day work.\n",

    /* 3: the assistant offers a confident interpretation the user neither
       confirms nor denies -- just changes the subject. Tests rule 2
       (sleep): silence/topic-change is not agreement. */
    "You: I've been having trouble sleeping lately.\n"
    "\n"
    "model: That's often a sign of underlying anxiety or unresolved stress -- have you "
    "considered that might be what's going on?\n"
    "\n"
    "You: Maybe. Anyway, do you know a good stretch routine for lower back pain?\n"
    "\n"
    "model: Sure, here's a simple routine: stand up, reach for your toes, hold for 20 "
    "seconds, then arch your back gently the other way.\n",
};
#define FB_SWEEP_TEST_SCENARIO_COUNT \
    (int)(sizeof(FB_SWEEP_TEST_SCENARIOS) / sizeof(FB_SWEEP_TEST_SCENARIOS[0]))

typedef struct {
    char label[128];    /* "diary page 0", "current long-term memory" */
    char file_stub[64]; /* "page_0", "ltm" -- used to name that source's report file */
    char *text;         /* malloc'd */
    const char *instruction;
    char *tree; /* malloc'd; a placeholder unless this is the ltm source */
} FbSweepSource;

typedef struct {
    char name[FB_BACKEND_NAME_MAX];
    int ok;
    char result[FB_DIARY_JOB_RESULT_MAX];
    unsigned long long elapsed_ms;
} FbSweepResult;

static const char *arg_value(int argc, char **argv, const char *key) {
    size_t klen = strlen(key);
    for (int i = 0; i < argc; ++i) {
        if (strncmp(argv[i], key, klen) == 0 && argv[i][klen] == '=') return argv[i] + klen + 1;
    }
    return NULL;
}

static void html_escape(const char *in, char *out, size_t out_cap) {
    size_t off = 0;
    for (size_t i = 0; in[i] != '\0' && off + 6 < out_cap; ++i) {
        char c = in[i];
        const char *rep = NULL;
        switch (c) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        default: break;
        }
        if (rep) {
            size_t rlen = strlen(rep);
            memcpy(out + off, rep, rlen);
            off += rlen;
        } else {
            out[off++] = c;
        }
    }
    out[off] = '\0';
}

static char *html_escape_alloc(const char *in) {
    char *out = (char *)malloc(strlen(in) * 6 + 16);
    if (out) html_escape(in, out, strlen(in) * 6 + 16);
    return out;
}

/* Fills one source from an already-read buffer -- shared by build_sources's
   "all" enumeration and its single-value path so both name/instruction/tree
   the same way for the same kind of source. Takes ownership of `text`
   (malloc'd, freed on failure paths by the caller if this returns 0). */
static void fill_source(FbSweepSource *s, char *text, const char *instruction, const char *label,
                         const char *file_stub, const char *data_dir, int is_ltm) {
    s->text = text;
    s->instruction = instruction;
    snprintf(s->label, sizeof(s->label), "%s", label);
    snprintf(s->file_stub, sizeof(s->file_stub), "%s", file_stub);

    s->tree = (char *)malloc(4096);
    if (!s->tree) return;
    if (is_ltm) {
        fb_diary_build_source_tree(data_dir, s->tree, 4096);
    } else {
        snprintf(s->tree, 4096, "(source tree only tracked for --source=ltm)");
    }
}

/* For --experimental's ltm source: joins the raw text of every page that
   currently, actually contributes to long-term memory (per the provenance
   log -- see fb_diary_get_live_pages) into one page-labeled buffer, for a
   genuine per-page-aware synthesis pass instead of re-compressing the
   already-flattened long_term.txt. Returns NULL (caller falls back to
   long_term.txt directly) if there's no log yet or nothing is live --
   expected for data that predates the provenance log, not an error. */
static char *build_experimental_ltm_text(const char *data_dir) {
    int pages[FB_SWEEP_MAX_LIVE_PAGES];
    int n = fb_diary_get_live_pages(data_dir, pages, FB_SWEEP_MAX_LIVE_PAGES);
    if (n == 0) return NULL;

    size_t cap = (size_t)n * (FB_DIARY_PAGE_MAX_CHARS + 64) + 64;
    char *joined = (char *)malloc(cap);
    if (!joined) return NULL;
    joined[0] = '\0';
    size_t off = 0;

    for (int i = 0; i < n && off < cap; ++i) {
        char path[600];
        snprintf(path, sizeof(path), "%s/diary/pages/%04d.txt", data_dir, pages[i]);
        size_t len = 0;
        char *text = fb_read_file(path, &len);
        if (!text) continue;

        int written = snprintf(joined + off, cap - off, "== page %d ==\n%s\n\n", pages[i], text);
        free(text);
        if (written > 0) off += (size_t)written;
    }

    if (off == 0) {
        free(joined);
        return NULL;
    }
    return joined;
}

/* Builds the ltm source, production or --experimental. Experimental tries
   the per-page-blended path first (build_experimental_ltm_text); if that
   comes back empty (no provenance log yet -- true for any long_term.txt
   that predates this feature), it degrades to the same flat long_term.txt
   read production uses, just with the V2 instruction, and says so in the
   label so that's visible in the report rather than silently misleading. */
/* Returns 1 and fills *s if there's anything to summarize, 0 (text freed,
   *s untouched) if not -- e.g. a fresh install with no long_term.txt at
   all yet. */
static int build_ltm_source(FbSweepSource *s, const char *data_dir, int experimental) {
    char *text = NULL;
    int page_blended = 0;
    if (experimental) {
        text = build_experimental_ltm_text(data_dir);
        page_blended = text != NULL;
    }
    size_t len = 0;
    if (!text) {
        char path[600];
        snprintf(path, sizeof(path), "%s/diary/long_term.txt", data_dir);
        text = fb_read_file(path, &len);
    } else {
        len = strlen(text);
    }
    if (!text || len == 0) {
        free(text);
        return 0;
    }

    const char *instruction =
        experimental ? FB_DIARY_CHECKPOINT_INSTRUCTION_V2 : FB_DIARY_CHECKPOINT_INSTRUCTION;
    char label[160];
    if (!experimental) {
        snprintf(label, sizeof(label), "current long-term memory");
    } else if (page_blended) {
        snprintf(label, sizeof(label), "current long-term memory (experimental, page-blended)");
    } else {
        snprintf(label, sizeof(label),
                 "current long-term memory (experimental, no page breakdown available)");
    }
    fill_source(s, text, instruction, label, "ltm", data_dir, 1);
    return 1;
}

/* Builds the list of sources to sweep. --source=all enumerates every
   archived page (stopping at the first missing index -- pages are always
   numbered contiguously from 0, see diary.c) plus long-term memory if it
   has any content; any other --source value builds exactly one. Returns
   the count (sources malloc'd, caller frees each .text/.tree then the
   array itself), or -1 with a message on stderr already printed. */
static int build_sources(const char *source_arg, const char *data_dir, int experimental,
                          FbSweepSource **out_sources) {
    if (!source_arg) {
        fprintf(stderr, "--source is required (page:N, ltm, all, test:N|all, or file:PATH)\n");
        return -1;
    }

    FbSweepSource *sources = (FbSweepSource *)calloc(FB_SWEEP_MAX_SOURCES, sizeof(FbSweepSource));
    if (!sources) {
        fprintf(stderr, "out of memory\n");
        return -1;
    }
    int count = 0;

    if (strcmp(source_arg, "all") == 0) {
        char root_dir[512];
        snprintf(root_dir, sizeof(root_dir), "%s/diary", data_dir);
        const char *page_instruction =
            experimental ? FB_DIARY_SLEEP_INSTRUCTION_V2 : FB_DIARY_SLEEP_INSTRUCTION;

        for (int index = 0; count < FB_SWEEP_MAX_SOURCES; ++index) {
            char path[600];
            snprintf(path, sizeof(path), "%s/pages/%04d.txt", root_dir, index);
            size_t len = 0;
            char *text = fb_read_file(path, &len);
            if (!text) break; /* pages are numbered contiguously -- first gap means "no more" */

            char label[128], stub[64];
            snprintf(label, sizeof(label), "diary page %d%s", index,
                     experimental ? " (experimental)" : "");
            snprintf(stub, sizeof(stub), "page_%d", index);
            fill_source(&sources[count++], text, page_instruction, label, stub, data_dir, 0);
        }

        if (count < FB_SWEEP_MAX_SOURCES && build_ltm_source(&sources[count], data_dir, experimental)) {
            count++;
        }

        if (count == 0) {
            fprintf(stderr, "nothing to sweep yet -- no archived diary pages and no long-term "
                            "memory found under %s\n",
                    root_dir);
            free(sources);
            return -1;
        }
        *out_sources = sources;
        return count;
    }

    if (strncmp(source_arg, "test:", 5) == 0) {
        const char *which = source_arg + 5;
        const char *page_instruction =
            experimental ? FB_DIARY_SLEEP_INSTRUCTION_V2 : FB_DIARY_SLEEP_INSTRUCTION;

        if (strcmp(which, "all") == 0) {
            for (int i = 0; i < FB_SWEEP_TEST_SCENARIO_COUNT && count < FB_SWEEP_MAX_SOURCES; ++i) {
                char *text = (char *)malloc(strlen(FB_SWEEP_TEST_SCENARIOS[i]) + 1);
                if (!text) break;
                strcpy(text, FB_SWEEP_TEST_SCENARIOS[i]);

                char label[128], stub[64];
                snprintf(label, sizeof(label), "test scenario %d%s", i + 1,
                         experimental ? " (experimental)" : "");
                snprintf(stub, sizeof(stub), "test_%d", i + 1);
                fill_source(&sources[count++], text, page_instruction, label, stub, data_dir, 0);
            }
            *out_sources = sources;
            return count;
        }

        int index = atoi(which);
        if (index < 1 || index > FB_SWEEP_TEST_SCENARIO_COUNT) {
            fprintf(stderr, "no such test scenario '%s' (have 1..%d, or 'all')\n", which,
                    FB_SWEEP_TEST_SCENARIO_COUNT);
            free(sources);
            return -1;
        }
        char *text = (char *)malloc(strlen(FB_SWEEP_TEST_SCENARIOS[index - 1]) + 1);
        if (!text) {
            fprintf(stderr, "out of memory\n");
            free(sources);
            return -1;
        }
        strcpy(text, FB_SWEEP_TEST_SCENARIOS[index - 1]);

        char label[128], stub[64];
        snprintf(label, sizeof(label), "test scenario %d%s", index,
                 experimental ? " (experimental)" : "");
        snprintf(stub, sizeof(stub), "test_%d", index);
        fill_source(&sources[0], text, page_instruction, label, stub, data_dir, 0);
        *out_sources = sources;
        return 1;
    }

    if (strncmp(source_arg, "page:", 5) == 0) {
        int index = atoi(source_arg + 5);
        char path[600];
        snprintf(path, sizeof(path), "%s/diary/pages/%04d.txt", data_dir, index);
        size_t len = 0;
        char *text = fb_read_file(path, &len);
        if (!text) {
            fprintf(stderr, "can't read archived page %d (%s)\n", index, path);
            free(sources);
            return -1;
        }
        char label[128], stub[64];
        snprintf(label, sizeof(label), "diary page %d%s", index,
                 experimental ? " (experimental)" : "");
        snprintf(stub, sizeof(stub), "page_%d", index);
        fill_source(&sources[0], text,
                    experimental ? FB_DIARY_SLEEP_INSTRUCTION_V2 : FB_DIARY_SLEEP_INSTRUCTION,
                    label, stub, data_dir, 0);
        *out_sources = sources;
        return 1;
    }

    if (strcmp(source_arg, "ltm") == 0) {
        if (!build_ltm_source(&sources[0], data_dir, experimental)) {
            char path[600];
            snprintf(path, sizeof(path), "%s/diary/long_term.txt", data_dir);
            fprintf(stderr, "no long_term.txt yet (%s) -- nothing consolidated\n", path);
            free(sources);
            return -1;
        }
        *out_sources = sources;
        return 1;
    }

    if (strncmp(source_arg, "file:", 5) == 0) {
        const char *path = source_arg + 5;
        size_t len = 0;
        char *text = fb_read_file(path, &len);
        if (!text) {
            fprintf(stderr, "can't read '%s'\n", path);
            free(sources);
            return -1;
        }
        /* Unlike page/ltm sources (already bounded by the live pipeline's
           own hard caps), an arbitrary file has no size limit at all --
           "it can summarize anything" shouldn't mean "or one accidental
           500MB log file wedges the request." */
        if (len > FB_SWEEP_SOURCE_MAX) {
            text[FB_SWEEP_SOURCE_MAX] = '\0';
            fprintf(stderr, "note: '%s' is larger than %d chars, truncated for the sweep\n", path,
                    FB_SWEEP_SOURCE_MAX);
        }
        fill_source(&sources[0], text, FB_DIARY_GENERIC_SUMMARY_INSTRUCTION, path, "file", data_dir,
                    0);
        *out_sources = sources;
        return 1;
    }

    fprintf(stderr, "unrecognized --source '%s' (expected page:N, ltm, all, test:N|all, or file:PATH)\n",
            source_arg);
    free(sources);
    return -1;
}

static int resolve_models(const char *models_arg, FbConfig *cfg, char names[][FB_BACKEND_NAME_MAX],
                           int max_models) {
    int count = 0;
    if (!models_arg || strcmp(models_arg, "all") == 0) {
        for (int i = 0; i < cfg->model_count && count < max_models; ++i) {
            snprintf(names[count++], FB_BACKEND_NAME_MAX, "%s", cfg->models[i].name);
        }
        return count;
    }

    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", models_arg);
    char *tok = strtok(buf, ",");
    while (tok && count < max_models) {
        while (*tok == ' ' || *tok == '\t') tok++;
        size_t tok_len = strlen(tok);
        while (tok_len > 0 && (tok[tok_len - 1] == ' ' || tok[tok_len - 1] == '\t')) {
            tok[--tok_len] = '\0';
        }

        char trimmed[FB_BACKEND_NAME_MAX];
        snprintf(trimmed, sizeof(trimmed), "%s", tok);
        if (fb_config_find_model(cfg, trimmed)) {
            snprintf(names[count++], FB_BACKEND_NAME_MAX, "%s", trimmed);
        } else {
            fprintf(stderr, "skipping unknown model '%s' (not in config.json)\n", trimmed);
        }
        tok = strtok(NULL, ",");
    }
    return count;
}

typedef enum { FB_SWEEP_LOAD_UP, FB_SWEEP_LOAD_TIMEOUT, FB_SWEEP_LOAD_CRASHED } FbSweepLoadResult;

/* Distinguishes "still loading, just slow" from "actually crashed" -- a
   large model doing heavy CPU/mmap offload on a small GPU can take
   several real minutes with no crash and no error, which used to be
   reported identically to an actual llama-server failure. */
static FbSweepLoadResult wait_for_up_or_error(FbBackendMgr *mgr, const char *name,
                                               unsigned long long timeout_ms) {
    unsigned long long start = fb_now_ms();
    for (;;) {
        fb_backend_mgr_tick(mgr);
        FbBackendInstance *b = fb_backend_mgr_find(mgr, name);
        if (!b) return FB_SWEEP_LOAD_CRASHED;
        if (b->state == FB_BACKEND_UP) return FB_SWEEP_LOAD_UP;
        if (b->state == FB_BACKEND_ERROR) return FB_SWEEP_LOAD_CRASHED;
        if (fb_now_ms() - start > timeout_ms) return FB_SWEEP_LOAD_TIMEOUT;
        fb_sleep_ms(200);
        printf(".");
        fflush(stdout);
    }
}

static void write_report(const char *out_path, const FbSweepSource *source,
                          const FbSweepResult *results, int result_count) {
    FILE *f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "couldn't write report to '%s'\n", out_path);
        return;
    }

    char *esc_source = html_escape_alloc(source->text);
    char *esc_tree = html_escape_alloc(source->tree);

    fprintf(f,
            "<!doctype html><html><head><meta charset=\"utf-8\">"
            "<title>summarization sweep -- %s</title><style>"
            "body{font-family:ui-monospace,Consolas,monospace;background:#161616;color:#ddd;"
            "margin:0;padding:20px}"
            "h1{font-size:15px;color:#9a6ae8;margin:0 0 4px}"
            "a{color:#9a6ae8}"
            ".sub{color:#888;font-size:12px;margin-bottom:16px}"
            ".panes{display:flex;gap:16px}"
            ".pane{flex:1;min-width:0}"
            "pre{white-space:pre-wrap;word-wrap:break-word;background:#1f1f1f;border:1px solid "
            "#333;border-radius:4px;padding:10px;font-size:12px;max-height:70vh;overflow:auto}"
            ".tabs{display:flex;gap:4px;margin-bottom:8px;flex-wrap:wrap}"
            ".tab{background:#1f1f1f;border:1px solid #333;color:#ccc;padding:5px 10px;"
            "border-radius:4px 4px 0 0;cursor:pointer;font-size:12px}"
            ".tab.active{background:#2a2a2a;color:#fff;border-color:#9a6ae8}"
            ".tab.err{color:#e86a6a}"
            ".meta{color:#888;font-size:11px;margin:4px 0 8px}"
            ".model{display:none}"
            ".model.active{display:block}"
            "</style></head><body>"
            "<h1>summarization sweep</h1>"
            "<div class=\"sub\"><a href=\"index.html\">&laquo; all sources</a> -- source: %s -- "
            "%d model%s -- dev tool, not production output</div>",
            source->label, source->label, result_count, result_count == 1 ? "" : "s");

    fprintf(f, "<details><summary style=\"cursor:pointer;color:#888;font-size:12px\">"
               "provenance tree</summary><pre>%s</pre></details>",
            esc_tree ? esc_tree : "");

    fprintf(f, "<div class=\"tabs\">");
    for (int i = 0; i < result_count; ++i) {
        fprintf(f, "<div class=\"tab%s%s\" onclick=\"show(%d)\">%s</div>",
                i == 0 ? " active" : "", results[i].ok ? "" : " err", i, results[i].name);
    }
    fprintf(f, "</div>");

    fprintf(f, "<div class=\"panes\"><div class=\"pane\"><div class=\"meta\">original</div>"
               "<pre>%s</pre></div><div class=\"pane\" id=\"results\">",
            esc_source ? esc_source : "");

    for (int i = 0; i < result_count; ++i) {
        char *esc_result = html_escape_alloc(results[i].result);
        fprintf(f,
                "<div class=\"model%s\" id=\"m%d\"><div class=\"meta\">%s -- %s -- %llums</div>"
                "<pre>%s</pre></div>",
                i == 0 ? " active" : "", i, results[i].name, results[i].ok ? "ok" : "FAILED",
                results[i].elapsed_ms, esc_result ? esc_result : "");
        free(esc_result);
    }

    fprintf(f, "</div></div>"
               "<script>function show(i){"
               "document.querySelectorAll('.tab').forEach((e,j)=>e.classList.toggle('active',j===i));"
               "document.querySelectorAll('.model').forEach((e,j)=>e.classList.toggle('active',j===i));"
               "}</script></body></html>");

    free(esc_source);
    free(esc_tree);
    fclose(f);
}

static void write_index(const char *out_dir, const FbSweepSource *sources, int source_count,
                         int model_count) {
    char path[700];
    snprintf(path, sizeof(path), "%s/index.html", out_dir);
    FILE *f = fopen(path, "wb");
    if (!f) return;

    fprintf(f,
            "<!doctype html><html><head><meta charset=\"utf-8\"><title>summarization sweep -- "
            "all sources</title><style>"
            "body{font-family:ui-monospace,Consolas,monospace;background:#161616;color:#ddd;"
            "padding:20px}h1{font-size:15px;color:#9a6ae8}a{color:#ddd;text-decoration:none;"
            "display:block;padding:6px 0;border-bottom:1px solid #292929}a:hover{color:#9a6ae8}"
            "</style></head><body><h1>summarization sweep -- %d source%s x %d model%s</h1>",
            source_count, source_count == 1 ? "" : "s", model_count, model_count == 1 ? "" : "s");

    for (int i = 0; i < source_count; ++i) {
        char *esc_label = html_escape_alloc(sources[i].label);
        fprintf(f, "<a href=\"%s.html\">%s</a>", sources[i].file_stub, esc_label ? esc_label : "");
        free(esc_label);
    }
    fprintf(f, "</body></html>");
    fclose(f);
}

/* Recognized flags, checked below so a typo (--model vs --models, once
   confirmed the hard way -- it silently fell through to "sweep every
   registered model" instead of erroring) gets a visible warning instead
   of silently doing the wrong thing. */
static const char *const FB_SWEEP_KNOWN_FLAGS[] = {
    "--source", "--models", "--model", "--out", "--no-open", "--experimental", "--load-timeout",
};

static void warn_on_unknown_flags(int argc, char **argv) {
    for (int i = 0; i < argc; ++i) {
        if (strncmp(argv[i], "--", 2) != 0) continue;

        char name[64];
        const char *eq = strchr(argv[i], '=');
        size_t len = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);
        if (len >= sizeof(name)) len = sizeof(name) - 1;
        memcpy(name, argv[i], len);
        name[len] = '\0';

        int known = 0;
        for (size_t k = 0; k < sizeof(FB_SWEEP_KNOWN_FLAGS) / sizeof(FB_SWEEP_KNOWN_FLAGS[0]);
             ++k) {
            if (strcmp(name, FB_SWEEP_KNOWN_FLAGS[k]) == 0) { known = 1; break; }
        }
        if (!known) {
            fprintf(stderr, "warning: unrecognized flag '%s' -- ignored (did you mean --models?)\n",
                    name);
        }
    }
}

int fb_sweep_run(int argc, char **argv) {
    warn_on_unknown_flags(argc, argv);

    const char *source_arg = arg_value(argc, argv, "--source");
    /* --model is a very natural typo/expectation when specifying just one
       -- accepted as a plain alias for --models rather than silently
       falling through to "sweep everything" like it used to. */
    const char *models_arg = arg_value(argc, argv, "--models");
    if (!models_arg) models_arg = arg_value(argc, argv, "--model");
    const char *out_arg = arg_value(argc, argv, "--out");
    const char *load_timeout_arg = arg_value(argc, argv, "--load-timeout");
    unsigned long long load_timeout_ms = load_timeout_arg
                                              ? (unsigned long long)atoi(load_timeout_arg) * 1000ULL
                                              : FB_SWEEP_DEFAULT_LOAD_TIMEOUT_MS;
    int experimental = 0;
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--experimental") == 0) experimental = 1;
    }

    char data_dir[512];
    if (fb_get_data_dir(data_dir, sizeof(data_dir)) != 0) {
        fprintf(stderr, "couldn't resolve the app data directory\n");
        return 1;
    }

    FbConfig cfg;
    fb_config_load(&cfg, data_dir);

    FbSweepSource *sources = NULL;
    int source_count = build_sources(source_arg, data_dir, experimental, &sources);
    if (source_count <= 0) return 1;
    if (experimental) {
        printf("--experimental: using alternate sleep/checkpoint instructions (see sweep.c) -- "
               "compare this report's directory against a non-experimental run's\n");
    }

    char model_names[FB_SWEEP_MAX_MODELS][FB_BACKEND_NAME_MAX];
    int model_count = resolve_models(models_arg, &cfg, model_names, FB_SWEEP_MAX_MODELS);
    if (model_count == 0) {
        fprintf(stderr, "no models to sweep -- check config.json / --models\n");
        free(sources);
        return 1;
    }

    char llama_server_path[FB_BACKEND_PATH_MAX];
    fb_resolve_llama_server(&cfg, llama_server_path, sizeof(llama_server_path));

    /* results[s * model_count + m] */
    FbSweepResult *results =
        (FbSweepResult *)calloc((size_t)source_count * (size_t)model_count, sizeof(FbSweepResult));
    if (!results) {
        fprintf(stderr, "out of memory\n");
        free(sources);
        return 1;
    }

    FbBackendMgr mgr;
    fb_backend_mgr_init(&mgr);

    printf("sweeping %d model(s) x %d source(s) (load timeout %llus)\n", model_count, source_count,
           load_timeout_ms / 1000);
    for (int m = 0; m < model_count; ++m) {
        printf("  [%d/%d] %s: loading", m + 1, model_count, model_names[m]);
        fflush(stdout);

        /* A model registered for more than one backend (see FbConfigModel's
           multi-binding shape) has no way to disambiguate in this
           non-interactive loop -- always sweeps its first-registered
           binding. */
        FbConfigModel *cm = fb_config_find_model(&cfg, model_names[m]);
        FbModelBinding *bind = (cm && cm->binding_count > 0) ? &cm->bindings[0] : NULL;

        char backend_exe[FB_BACKEND_PATH_MAX];
        char extra_buf[256];
        const char *extra_argv[32];
        if (bind) {
            switch (bind->kind) {
            case FB_BACKEND_SILIANG: fb_resolve_siliang_exe(&cfg, backend_exe, sizeof(backend_exe)); break;
            case FB_BACKEND_VLLM: fb_resolve_vllm_exe(&cfg, backend_exe, sizeof(backend_exe)); break;
            case FB_BACKEND_SGLANG: fb_resolve_sglang_python(&cfg, backend_exe, sizeof(backend_exe)); break;
            case FB_BACKEND_LLAMA_CPP:
            default: snprintf(backend_exe, sizeof(backend_exe), "%s", llama_server_path); break;
            }
            snprintf(extra_buf, sizeof(extra_buf), "%s", bind->extra_args);
            tokenize_extra_args(extra_buf, extra_argv, 32);
        }
        int spawned = bind && fb_backend_mgr_load(&mgr, model_names[m], bind->path, bind->kind,
                                                   backend_exe, data_dir, 0,
                                                   extra_argv[0] ? extra_argv : NULL) == 0;
        FbSweepLoadResult load_result = FB_SWEEP_LOAD_CRASHED;
        if (spawned) load_result = wait_for_up_or_error(&mgr, model_names[m], load_timeout_ms);
        int loaded = load_result == FB_SWEEP_LOAD_UP;

        for (int s = 0; s < source_count; ++s) {
            FbSweepResult *r = &results[s * model_count + m];
            snprintf(r->name, sizeof(r->name), "%s", model_names[m]);
            if (!loaded) {
                if (load_result == FB_SWEEP_LOAD_TIMEOUT) {
                    snprintf(r->result, sizeof(r->result),
                             "still loading after %llus -- not crashed, just slower than the "
                             "timeout (see <data_dir>/logs/%s.log; try a larger "
                             "--load-timeout=<seconds>)",
                             load_timeout_ms / 1000, model_names[m]);
                } else {
                    snprintf(r->result, sizeof(r->result),
                             "failed to start or crashed (see <data_dir>/logs/%s.log)",
                             model_names[m]);
                }
                continue;
            }
            FbBackendInstance *b = fb_backend_mgr_find(&mgr, model_names[m]);
            unsigned long long start = fb_now_ms();
            int rc = fb_diary_job_run_sync(b->port, sources[s].instruction, sources[s].text,
                                            r->result, sizeof(r->result));
            r->elapsed_ms = fb_now_ms() - start;
            r->ok = rc == 0;
        }

        printf(loaded ? " done\n" : " FAILED\n");
        /* Stop it regardless of why it didn't come up -- a timed-out model
           is usually still actively loading in the background, and
           leaving it running into the next model's turn means two large
           models competing for the same VRAM, which can cascade into
           failing models that would otherwise have loaded fine. Only
           skip this if spawning itself never succeeded (nothing to stop). */
        if (spawned) fb_backend_mgr_unload(&mgr, model_names[m]);
    }

    fb_backend_mgr_shutdown(&mgr);

    char out_dir[600];
    if (out_arg) {
        snprintf(out_dir, sizeof(out_dir), "%s", out_arg);
    } else {
        snprintf(out_dir, sizeof(out_dir), "%s/diary/sweeps/%llu", data_dir, fb_now_ms());
    }
    fb_ensure_dir(out_dir);

    char first_report_path[700];
    for (int s = 0; s < source_count; ++s) {
        char out_path[700];
        snprintf(out_path, sizeof(out_path), "%s/%s.html", out_dir, sources[s].file_stub);
        write_report(out_path, &sources[s], &results[s * model_count], model_count);
        if (s == 0) snprintf(first_report_path, sizeof(first_report_path), "%s", out_path);
    }

    char index_path[700];
    int has_index = source_count > 1;
    if (has_index) {
        write_index(out_dir, sources, source_count, model_count);
        snprintf(index_path, sizeof(index_path), "%s/index.html", out_dir);
    }

    printf("report%s written to %s\n", source_count > 1 ? "s" : "", out_dir);

    /* --no-open for a headless/CI invocation; opens by default otherwise --
       the whole point of a dev-only comparison tool is looking at it. */
    int no_open = 0;
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--no-open") == 0) no_open = 1;
    }
    if (!no_open) {
        printf("opening in browser...\n");
        fb_open_in_browser(has_index ? index_path : first_report_path);
    }

    for (int s = 0; s < source_count; ++s) {
        free(sources[s].text);
        free(sources[s].tree);
    }
    free(sources);
    free(results);
    return 0;
}
