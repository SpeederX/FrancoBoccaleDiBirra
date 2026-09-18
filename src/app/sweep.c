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

#define FB_SWEEP_MAX_MODELS FB_CONFIG_MAX_MODELS
#define FB_SWEEP_MAX_SOURCES 256
#define FB_SWEEP_MAX_LIVE_PAGES 64
#define FB_SWEEP_SOURCE_MAX (FB_DIARY_LTM_MAX_CHARS > 20000 ? FB_DIARY_LTM_MAX_CHARS : 20000)
#define FB_SWEEP_LOAD_TIMEOUT_MS 60000

/* --experimental: an alternate instruction pair, dev-only, not wired into
   the live pipeline anywhere -- purely for --sweep-summarize to compare
   against the production instructions (diary_job.h) in its own separate
   timestamped output directory. Differences from production, per actual
   user feedback on what the production instructions get wrong:
   1. The user's own statements are ground truth; the assistant's replies
      only matter for concrete answers given and for whether/how much the
      user actually agreed with them -- not for their tone or flourish.
   2. Checkpoint here synthesizes from each individual page's raw text
      (see build_experimental_ltm_text) rather than re-compressing
      long_term.txt's own flat, already-blended prior output -- a fact
      repeated across multiple pages is a stronger signal than one
      mentioned once, which a flat compress-in-place can't distinguish. */
#define FB_DIARY_SLEEP_INSTRUCTION_V2                                                          \
    "You are a diary's memory consolidation process. Read the page below (a raw record of a "  \
    "conversation between the user and an assistant) and extract what's worth remembering "     \
    "long-term, following these rules: 1) Ground truth is what the USER said -- their own "     \
    "facts, statements, decisions, and stated feelings; extract these first. 2) The "           \
    "assistant's replies matter only for (a) any concrete answer, information, or plan it "     \
    "actually gave, and (b) whether and how much the user agreed with, accepted, corrected, "   \
    "or rejected it afterward -- note the specific part agreed with, not just \"agreed\" or "    \
    "\"disagreed\". 3) Do not treat the assistant's own rhetorical flourish, roleplay, tone, "  \
    "or exaggeration as fact, even if phrased confidently -- if a reply doesn't correspond to " \
    "something the user actually confirmed or acted on, leave it out rather than restating it. " \
    "Write only the resulting summary, no preamble, no headers."

#define FB_DIARY_CHECKPOINT_INSTRUCTION_V2                                                     \
    "You are synthesizing several diary pages (each section below is one page's raw "          \
    "conversation, oldest first) into ONE coherent overview of what's durably true and "        \
    "important -- not a per-page list. Rules: 1) Merge related or repeated points into a "      \
    "single statement; something that appears across multiple pages is more likely to be a "    \
    "stable, important fact than something mentioned once -- weight it accordingly. 2) "         \
    "Prioritize the user's own established facts, decisions, and stated feelings over the "     \
    "assistant's phrasing (assistant replies matter only for concrete answers given and "       \
    "whether/how much the user agreed with them). 3) Drop anything that reads as one-off, "     \
    "stale, or superseded by a later page. Write only the resulting overview, no preamble, no " \
    "headers, no per-page breakdown -- one blended narrative."

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
        fprintf(stderr, "--source is required (page:N, ltm, all, or file:PATH)\n");
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

    fprintf(stderr, "unrecognized --source '%s' (expected page:N, ltm, all, or file:PATH)\n",
            source_arg);
    free(sources);
    return -1;
}

static int resolve_models(const char *models_arg, const FbConfig *cfg,
                           char names[][FB_BACKEND_NAME_MAX], int max_models) {
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
        char path[FB_BACKEND_PATH_MAX];
        if (fb_config_find_model(cfg, trimmed, path, sizeof(path))) {
            snprintf(names[count++], FB_BACKEND_NAME_MAX, "%s", trimmed);
        } else {
            fprintf(stderr, "skipping unknown model '%s' (not in config.json)\n", trimmed);
        }
        tok = strtok(NULL, ",");
    }
    return count;
}

static int wait_for_up_or_error(FbBackendMgr *mgr, const char *name) {
    unsigned long long start = fb_now_ms();
    for (;;) {
        fb_backend_mgr_tick(mgr);
        FbBackendInstance *b = fb_backend_mgr_find(mgr, name);
        if (!b) return -1;
        if (b->state == FB_BACKEND_UP) return 0;
        if (b->state == FB_BACKEND_ERROR) return -1;
        if (fb_now_ms() - start > FB_SWEEP_LOAD_TIMEOUT_MS) return -1;
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

int fb_sweep_run(int argc, char **argv) {
    const char *source_arg = arg_value(argc, argv, "--source");
    const char *models_arg = arg_value(argc, argv, "--models");
    const char *out_arg = arg_value(argc, argv, "--out");
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

    printf("sweeping %d model(s) x %d source(s)\n", model_count, source_count);
    for (int m = 0; m < model_count; ++m) {
        printf("  [%d/%d] %s: loading", m + 1, model_count, model_names[m]);
        fflush(stdout);

        char model_path[FB_BACKEND_PATH_MAX];
        int loaded =
            fb_config_find_model(&cfg, model_names[m], model_path, sizeof(model_path)) &&
            fb_backend_mgr_load(&mgr, model_names[m], model_path, llama_server_path, data_dir, 0,
                                 NULL) == 0;
        if (loaded) loaded = wait_for_up_or_error(&mgr, model_names[m]) == 0;

        for (int s = 0; s < source_count; ++s) {
            FbSweepResult *r = &results[s * model_count + m];
            snprintf(r->name, sizeof(r->name), "%s", model_names[m]);
            if (!loaded) {
                snprintf(r->result, sizeof(r->result),
                         "model never became ready (see <data_dir>/logs/%s.log)", model_names[m]);
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
        if (loaded) fb_backend_mgr_unload(&mgr, model_names[m]);
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
