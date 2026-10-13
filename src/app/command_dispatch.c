#include "app/command_dispatch.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "backend/backend_mgr.h"
#include "calibr/calibr_results.h"
#include "chat/chat_job.h"
#include "config/config.h"
#include "diary/diary.h"
#include "intent/intent_brain.h"
#include "knowledge/knowledge.h"
#include "platform/dirlist.h"
#include "tui/widget_textview.h"
#include "util/paths.h"

/* The always-resident Qwen brain (and now the embedding brain, see
   knowledge.h) lives in backend_mgr right alongside any user-loaded
   model, so a plain backend_mgr.count == 1 check for "just one [user]
   model loaded" is wrong whenever either is running too -- confirmed the
   hard way for the Qwen brain: with it up, count is 2 even with a single
   real model loaded, so /unload's auto-select silently never fired. This
   counts (and finds) only entries that are neither brain. */
static int count_non_brain_backends(const FbBackendMgr *mgr, const char **out_sole_name) {
    int count = 0;
    for (int i = 0; i < mgr->count; ++i) {
        if (strcmp(mgr->items[i].name, FB_QWEN_BRAIN_NAME) != 0 &&
            strcmp(mgr->items[i].name, FB_EMBEDDING_BRAIN_NAME) != 0) {
            if (out_sole_name) *out_sole_name = mgr->items[i].name;
            count++;
        }
    }
    return count;
}

/* /add's optional trailing backend argument. Returns 1 for "add for every
   compatible backend" (empty, or literally "all" -- the default: see
   compatible_kinds_for_path for what "compatible" means), 0 for one
   specific named kind (filled into *out_kind), -1 for an unrecognized
   name the caller should report rather than silently guess past. */
static int parse_backend_kind_arg(const char *s, FbBackendKind *out_kind) {
    if (s[0] == '\0' || strcmp(s, "all") == 0) return 1;
    if (strcmp(s, "llama") == 0 || strcmp(s, "llama_cpp") == 0) {
        *out_kind = FB_BACKEND_LLAMA_CPP;
        return 0;
    }
    if (strcmp(s, "siliang") == 0) {
        *out_kind = FB_BACKEND_SILIANG;
        return 0;
    }
    if (strcmp(s, "vllm") == 0) {
        *out_kind = FB_BACKEND_VLLM;
        return 0;
    }
    if (strcmp(s, "sglang") == 0) {
        *out_kind = FB_BACKEND_SGLANG;
        return 0;
    }
    return -1;
}

/* Only llama.cpp and Siliang (its fork) consume a single .gguf file --
   vLLM and SGLang take a local model directory or a bare Hugging Face
   repo id, neither of which is something to validate against the local
   filesystem the way a .gguf path is. */
static int backend_kind_wants_gguf_file(FbBackendKind kind) {
    return kind == FB_BACKEND_LLAMA_CPP || kind == FB_BACKEND_SILIANG;
}

static const char *backend_kind_label(FbBackendKind kind) {
    switch (kind) {
    case FB_BACKEND_SILIANG: return "siliang";
    case FB_BACKEND_VLLM: return "vllm";
    case FB_BACKEND_SGLANG: return "sglang";
    case FB_BACKEND_LLAMA_CPP:
    default: return "llama_cpp";
    }
}

/* Which kinds are plausible for a /add path when the user didn't name one
   specifically ("all", or the argument omitted entirely -- the default).
   llama.cpp and Siliang both consume a single .gguf file; vLLM and SGLang
   both consume a local HF-format directory or a bare Hugging Face repo
   id -- the two groups' inputs are not interchangeable in general, so
   "all" means "every kind compatible with what this path actually looks
   like", not literally all four unconditionally (a .gguf file registered
   as a vLLM/SGLang binding would just never load with either). Returns
   the count (2, always, for now -- always exactly one of the two groups). */
static int compatible_kinds_for_path(const char *path, FbBackendKind *out_kinds) {
    if (!fb_path_is_dir(path)) {
        FILE *f = fopen(path, "rb");
        if (f) {
            fclose(f);
            out_kinds[0] = FB_BACKEND_LLAMA_CPP;
            out_kinds[1] = FB_BACKEND_SILIANG;
            return 2;
        }
    }
    out_kinds[0] = FB_BACKEND_VLLM;
    out_kinds[1] = FB_BACKEND_SGLANG;
    return 2;
}

/* Splits a free-form "extra args" string (whitespace-separated, no quote
   support -- a flag value containing a space isn't representable) into a
   NULL-terminated argv-style array of pointers *into* buf, which this
   mutates in place (strtok-style). Returns the token count. */
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

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static size_t token_len(const char *s) {
    size_t n = 0;
    while (s[n] != '\0' && s[n] != ' ' && s[n] != '\t') n++;
    return n;
}

static void copy_trimmed(char *out, size_t cap, const char *s, size_t len) {
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r')) len--;
    if (len >= cap) len = cap - 1;
    memcpy(out, s, len);
    out[len] = '\0';
}

FbCommand fb_command_parse(const char *line) {
    FbCommand cmd;
    memset(&cmd, 0, sizeof(cmd));

    const char *p = skip_ws(line);
    if (*p != '/') {
        cmd.kind = FB_CMD_UNKNOWN;
        return cmd;
    }
    p++;

    size_t verb_len = token_len(p);
    const char *rest = skip_ws(p + verb_len);

    char verb[32];
    copy_trimmed(verb, sizeof(verb), p, verb_len);

    if (strcmp(verb, "load") == 0) cmd.kind = FB_CMD_LOAD;
    else if (strcmp(verb, "unload") == 0) cmd.kind = FB_CMD_UNLOAD;
    else if (strcmp(verb, "add") == 0) cmd.kind = FB_CMD_ADD;
    else if (strcmp(verb, "remove") == 0) cmd.kind = FB_CMD_REMOVE;
    else if (strcmp(verb, "lang") == 0) cmd.kind = FB_CMD_LANG;
    else if (strcmp(verb, "download") == 0) cmd.kind = FB_CMD_DOWNLOAD;
    else if (strcmp(verb, "list") == 0 || strcmp(verb, "models") == 0) cmd.kind = FB_CMD_LIST;
    else if (strcmp(verb, "new_page") == 0) cmd.kind = FB_CMD_NEW_PAGE;
    else if (strcmp(verb, "ingest") == 0) cmd.kind = FB_CMD_INGEST;
    else if (strcmp(verb, "recall") == 0) cmd.kind = FB_CMD_RECALL;
    else if (strcmp(verb, "leaderboard") == 0) cmd.kind = FB_CMD_LEADERBOARD;
    else if (strcmp(verb, "logs") == 0) cmd.kind = FB_CMD_LOGS;
    else if (strcmp(verb, "dashboard") == 0) cmd.kind = FB_CMD_DASHBOARD;
    else if (strcmp(verb, "help") == 0) cmd.kind = FB_CMD_HELP;
    else if (strcmp(verb, "quit") == 0) cmd.kind = FB_CMD_QUIT;
    else {
        cmd.kind = FB_CMD_UNKNOWN;
        return cmd;
    }

    if (cmd.kind == FB_CMD_ADD) {
        size_t name_len = token_len(rest);
        copy_trimmed(cmd.arg1, sizeof(cmd.arg1), rest, name_len);
        const char *path_part = skip_ws(rest + name_len);
        const char *kind_part;
        if (path_part[0] == '"') {
            const char *closing = strchr(path_part + 1, '"');
            if (!closing) {
                return cmd;
            }
            copy_trimmed(cmd.arg2, sizeof(cmd.arg2), path_part + 1, (size_t)(closing - path_part - 1));
            kind_part = skip_ws(closing + 1);
        } else {
            size_t path_len = token_len(path_part);
            kind_part = skip_ws(path_part + path_len);
            char kind[32];
            copy_trimmed(kind, sizeof(kind), kind_part, token_len(kind_part));
            FbBackendKind parsed_kind;
            if (parse_backend_kind_arg(kind, &parsed_kind) < 0) {
                copy_trimmed(cmd.arg2, sizeof(cmd.arg2), path_part, strlen(path_part));
                return cmd;
            }
            copy_trimmed(cmd.arg2, sizeof(cmd.arg2), path_part, path_len);
        }
        size_t kind_len = token_len(kind_part);
        copy_trimmed(cmd.arg3, sizeof(cmd.arg3), kind_part, kind_len);
        const char *extra_part = skip_ws(kind_part + kind_len);
        copy_trimmed(cmd.arg4, sizeof(cmd.arg4), extra_part, strlen(extra_part));
    } else if (cmd.kind == FB_CMD_INGEST) {
        size_t path_len = token_len(rest);
        copy_trimmed(cmd.arg1, sizeof(cmd.arg1), rest, path_len);
        const char *after_path = skip_ws(rest + path_len);
        size_t group_len = token_len(after_path);
        copy_trimmed(cmd.arg2, sizeof(cmd.arg2), after_path, group_len);
        const char *after_group = skip_ws(after_path + group_len);
        copy_trimmed(cmd.arg3, sizeof(cmd.arg3), after_group, token_len(after_group));
    } else if (cmd.kind == FB_CMD_LOAD || cmd.kind == FB_CMD_REMOVE) {
        size_t name_len = token_len(rest);
        copy_trimmed(cmd.arg1, sizeof(cmd.arg1), rest, name_len);
        const char *kind_part = skip_ws(rest + name_len);
        copy_trimmed(cmd.arg2, sizeof(cmd.arg2), kind_part, strlen(kind_part));
    } else if (cmd.kind == FB_CMD_UNLOAD || cmd.kind == FB_CMD_LANG ||
               cmd.kind == FB_CMD_DOWNLOAD || cmd.kind == FB_CMD_RECALL) {
        size_t arg_len = (cmd.kind == FB_CMD_DOWNLOAD || cmd.kind == FB_CMD_RECALL)
            ? strlen(rest) : token_len(rest);
        copy_trimmed(cmd.arg1, sizeof(cmd.arg1), rest, arg_len);
    }

    return cmd;
}

static void set_status(FbApp *app, const char *text) {
    strncpy(app->last_submitted, text, FB_CMDLINE_MAX - 1);
    app->last_submitted[FB_CMDLINE_MAX - 1] = '\0';
}

static void set_status_fmt(FbApp *app, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(app->last_submitted, FB_CMDLINE_MAX, fmt, args);
    va_end(args);
}

void fb_command_execute(FbApp *app, const FbCommand *cmd) {
    switch (cmd->kind) {
    case FB_CMD_LOAD: {
        const char *name = cmd->arg1;
        if (name[0] == '\0') {
            if (app->config.model_count == 1 && app->config.models[0].binding_count == 1) {
                name = app->config.models[0].name;
            } else {
                set_status(app, app->config.model_count == 0
                                     ? "usage: /load <model> (nothing registered yet -- try /add)"
                                     : "usage: /load <model> (multiple registered -- tab-completes)");
                return;
            }
        }
        FbConfigModel *m = fb_config_find_model(&app->config, name);
        if (!m) {
            set_status_fmt(app, "unknown model '%s' (use /add <path> first)", name);
            return;
        }

        FbModelBinding *b;
        if (cmd->arg2[0] != '\0') {
            FbBackendKind requested;
            if (parse_backend_kind_arg(cmd->arg2, &requested) != 0) {
                set_status_fmt(app,
                               "unknown backend '%s' (expected llama, siliang, vllm, or sglang)",
                               cmd->arg2);
                return;
            }
            b = fb_config_find_binding(m, requested);
            if (!b) {
                set_status_fmt(app, "'%s' isn't registered for %s", name,
                               backend_kind_label(requested));
                return;
            }
        } else if (m->binding_count == 1) {
            b = &m->bindings[0];
        } else {
            char list[128];
            size_t off = 0;
            for (int i = 0; i < m->binding_count && off < sizeof(list) - 1; ++i) {
                int n = snprintf(list + off, sizeof(list) - off, "%s%s", i > 0 ? ", " : "",
                                  backend_kind_label(m->bindings[i].kind));
                if (n > 0) off += (size_t)n;
            }
            set_status_fmt(app,
                           "'%s' is registered for multiple backends (%s) -- specify which: "
                           "/load %s <backend>",
                           name, list, name);
            return;
        }

        if (backend_kind_wants_gguf_file(b->kind)) {
            if (fb_path_is_dir(b->path)) {
                set_status_fmt(app, "'%s' is registered as a directory, not a .gguf file -- "
                                     "/remove %s then /add it again via the file picker",
                               b->path, name);
                return;
            }
            FILE *probe = fopen(b->path, "rb");
            if (!probe) {
                set_status_fmt(app, "can't read '%s' for '%s' (moved or deleted?)", b->path, name);
                return;
            }
            fclose(probe);
        }

        char backend_exe[FB_BACKEND_PATH_MAX];
        switch (b->kind) {
        case FB_BACKEND_SILIANG: fb_resolve_siliang_exe(&app->config, backend_exe, sizeof(backend_exe)); break;
        case FB_BACKEND_VLLM: fb_resolve_vllm_exe(&app->config, backend_exe, sizeof(backend_exe)); break;
        case FB_BACKEND_SGLANG: fb_resolve_sglang_python(&app->config, backend_exe, sizeof(backend_exe)); break;
        case FB_BACKEND_LLAMA_CPP:
        default: snprintf(backend_exe, sizeof(backend_exe), "%s", app->llama_server_path); break;
        }

        char extra_buf[256];
        snprintf(extra_buf, sizeof(extra_buf), "%s", b->extra_args);
        const char *extra_argv[32];
        tokenize_extra_args(extra_buf, extra_argv, 32);

        if (fb_backend_mgr_load(&app->backend_mgr, name, b->path, b->kind, backend_exe,
                                 app->data_dir, 0, extra_argv[0] ? extra_argv : NULL) != 0) {
            set_status_fmt(app, "failed to start '%s' (already loaded, registry full, or '%s' "
                                 "not found)",
                           name, backend_exe);
            return;
        }
        FbBackendInstance *bi = fb_backend_mgr_find(&app->backend_mgr, name);
        set_status_fmt(app, "loading '%s' (%s) on port %d...", name, backend_kind_label(b->kind),
                       bi ? bi->port : -1);
        return;
    }
    case FB_CMD_UNLOAD: {
        /* Copied, not aliased: fb_backend_mgr_unload() shifts items[] on
           removal, which would otherwise leave `name` dangling when it
           points straight into items[0].name below. */
        char name[FB_BACKEND_NAME_MAX];
        if (cmd->arg1[0] != '\0') {
            strncpy(name, cmd->arg1, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
        } else {
            const char *sole_name = NULL;
            int non_brain_count = count_non_brain_backends(&app->backend_mgr, &sole_name);
            if (non_brain_count == 1) {
                strncpy(name, sole_name, sizeof(name) - 1);
                name[sizeof(name) - 1] = '\0';
            } else {
                set_status(app, non_brain_count == 0
                                     ? "usage: /unload <model> (nothing loaded)"
                                     : "usage: /unload <model> (multiple loaded -- tab-completes)");
                return;
            }
        }
        if (fb_backend_mgr_unload(&app->backend_mgr, name) != 0) {
            set_status_fmt(app, "'%s' is not loaded", name);
            return;
        }
        set_status_fmt(app, "unloaded '%s'", name);
        return;
    }
    case FB_CMD_ADD: {
        if (cmd->arg2[0] == '\0') {
            /* no path given: open the inline file picker instead of erroring */
            strncpy(app->file_picker_pending_name, cmd->arg1, FB_BACKEND_NAME_MAX - 1);
            app->file_picker_pending_name[FB_BACKEND_NAME_MAX - 1] = '\0';

            char start_dir[512];
            if (app->config.last_browse_dir[0] != '\0') {
                strncpy(start_dir, app->config.last_browse_dir, sizeof(start_dir) - 1);
                start_dir[sizeof(start_dir) - 1] = '\0';
            } else if (fb_get_home_dir(start_dir, sizeof(start_dir)) != 0) {
                strncpy(start_dir, ".", sizeof(start_dir) - 1);
            }

            fb_file_picker_open(&app->file_picker, start_dir);
            set_status(app, "browsing for a .gguf file -- arrows to navigate, "
                            "enter to open/select, esc to cancel");
            return;
        }
        FbBackendKind kinds[FB_CONFIG_MAX_BINDINGS];
        int kind_count;
        FbBackendKind single_kind;
        int rc = parse_backend_kind_arg(cmd->arg3, &single_kind);
        if (rc < 0) {
            set_status_fmt(app, "unknown backend '%s' (expected llama, siliang, vllm, sglang, or all)",
                           cmd->arg3);
            return;
        }
        if (rc == 1) {
            /* Empty, or literally "all": the default -- register every
               kind compatible with what this path actually looks like
               (see compatible_kinds_for_path), not literally all four. */
            kind_count = compatible_kinds_for_path(cmd->arg2, kinds);
        } else {
            kinds[0] = single_kind;
            kind_count = 1;
        }

        for (int i = 0; i < kind_count; ++i) {
            if (backend_kind_wants_gguf_file(kinds[i])) {
                if (fb_path_is_dir(cmd->arg2)) {
                    set_status_fmt(app,
                                   "'%s' is a directory -- point at the actual .gguf file inside "
                                   "it (or just /add with no path to browse)",
                                   cmd->arg2);
                    return;
                }
                FILE *probe = fopen(cmd->arg2, "rb");
                if (!probe) {
                    set_status_fmt(app, "can't read '%s' (check the path)", cmd->arg2);
                    return;
                }
                fclose(probe);
            }
            /* vLLM/SGLang: cmd->arg2 may be a local directory (checked to
               exist, since that's cheap and catches typos) or a bare
               Hugging Face repo id (e.g. "meta-llama/Llama-3-8B"), which
               has nothing to check locally at all -- neither engine's own
               resolution is duplicated here, so a bad repo id only
               surfaces when the spawned process itself fails, same as
               any other unreachable model. */
            fb_config_add_binding(&app->config, cmd->arg1, kinds[i], cmd->arg2, cmd->arg4);
        }
        fb_config_save(&app->config, app->data_dir);

        char kind_list[128];
        size_t off = 0;
        for (int i = 0; i < kind_count && off < sizeof(kind_list) - 1; ++i) {
            int n = snprintf(kind_list + off, sizeof(kind_list) - off, "%s%s", i > 0 ? ", " : "",
                              backend_kind_label(kinds[i]));
            if (n > 0) off += (size_t)n;
        }
        set_status_fmt(app, "added '%s' (%s) -> %s", cmd->arg1, kind_list, cmd->arg2);
        return;
    }
    case FB_CMD_REMOVE: {
        /* Copied, not aliased: fb_config_remove_model() shifts models[] on
           removal, same hazard as FB_CMD_UNLOAD's name above. */
        char name[FB_BACKEND_NAME_MAX];
        if (cmd->arg1[0] != '\0') {
            strncpy(name, cmd->arg1, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
        } else if (app->config.model_count == 1) {
            strncpy(name, app->config.models[0].name, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
        } else {
            set_status(app, app->config.model_count == 0
                                 ? "usage: /remove <model> (nothing registered yet)"
                                 : "usage: /remove <model> (multiple registered -- tab-completes)");
            return;
        }

        FbBackendKind kind;
        const FbBackendKind *kind_ptr = NULL;
        if (cmd->arg2[0] != '\0') {
            if (parse_backend_kind_arg(cmd->arg2, &kind) != 0) {
                set_status_fmt(app,
                               "unknown backend '%s' (expected llama, siliang, vllm, or sglang)",
                               cmd->arg2);
                return;
            }
            kind_ptr = &kind;
        }

        fb_backend_mgr_unload(&app->backend_mgr, name); /* no-op if not loaded */
        if (fb_config_remove_model(&app->config, name, kind_ptr) != 0) {
            set_status_fmt(app, kind_ptr ? "'%s' isn't registered for %s" : "'%s' is not registered",
                           name, kind_ptr ? backend_kind_label(*kind_ptr) : "");
            return;
        }
        fb_config_save(&app->config, app->data_dir);
        set_status_fmt(app, kind_ptr ? "removed '%s' (%s)" : "removed '%s'", name,
                       kind_ptr ? backend_kind_label(*kind_ptr) : "");
        return;
    }
    case FB_CMD_LANG:
        if (cmd->arg1[0] == '\0') {
            set_status_fmt(app, "current language preference: %s "
                                 "(informational only -- Orukeet auto-detects spoken language)",
                           app->config.lang_pref);
            return;
        }
        strncpy(app->config.lang_pref, cmd->arg1, sizeof(app->config.lang_pref) - 1);
        app->config.lang_pref[sizeof(app->config.lang_pref) - 1] = '\0';
        fb_config_save(&app->config, app->data_dir);
        set_status_fmt(app, "language preference set to '%s' "
                             "(informational only -- Orukeet auto-detects spoken language)",
                       app->config.lang_pref);
        return;
    case FB_CMD_DOWNLOAD:
        if (cmd->arg1[0] == '\0') {
            set_status(app, "usage: /download <direct .gguf URL> (or /download cancel)");
            return;
        }
        if (strcmp(cmd->arg1, "cancel") == 0) {
            if (app->download_job.state != FB_DOWNLOAD_RUNNING) {
                set_status(app, "no download is running");
                return;
            }
            fb_download_job_cancel(&app->download_job);
            set_status(app, "download cancelled; partial file kept for a later resume");
            return;
        }
        if (app->download_job.state == FB_DOWNLOAD_RUNNING) {
            set_status_fmt(app, "already downloading '%s' (/download cancel to stop)",
                           app->download_job.model_name);
            return;
        }
        if (fb_download_job_start(&app->download_job, cmd->arg1, app->data_dir) != 0) {
            set_status_fmt(app, "download couldn't start: %s", app->download_job.error);
            fb_download_job_consume(&app->download_job);
            return;
        }
        app->download_last_status_ms = 0;
        if (app->download_job.state == FB_DOWNLOAD_DONE) {
            set_status_fmt(app, "found existing '%s'; registering it...",
                           app->download_job.model_name);
        } else if (app->download_job.start_bytes > 0) {
            set_status_fmt(app, "resuming '%s' from %llu bytes...",
                           app->download_job.model_name, app->download_job.start_bytes);
        } else {
            set_status_fmt(app, "downloading '%s'...", app->download_job.model_name);
        }
        return;
    case FB_CMD_LIST: {
        if (app->config.model_count == 0) {
            set_status(app, "no models registered -- /add one first");
            return;
        }
        char line[FB_CMDLINE_MAX];
        size_t off = 0;
        for (int i = 0; i < app->config.model_count && off < sizeof(line) - 1; ++i) {
            const FbConfigModel *m = &app->config.models[i];
            FbBackendInstance *b = fb_backend_mgr_find(&app->backend_mgr, m->name);
            int n = snprintf(line + off, sizeof(line) - off, "%s%s(", i > 0 ? "  " : "", m->name);
            if (n > 0) off += (size_t)n;
            for (int j = 0; j < m->binding_count && off < sizeof(line) - 1; ++j) {
                n = snprintf(line + off, sizeof(line) - off, "%s%s", j > 0 ? "," : "",
                              backend_kind_label(m->bindings[j].kind));
                if (n > 0) off += (size_t)n;
            }
            n = snprintf(line + off, sizeof(line) - off, ")%s", b ? " [loaded]" : "");
            if (n > 0) off += (size_t)n;
        }
        set_status(app, line);
        return;
    }
    case FB_CMD_NEW_PAGE:
        if (app->input_mode == FB_INPUT_MODE_DIARY) {
            if (!app->diary.page_has_content) {
                set_status(app, "diary: already on a fresh page");
                return;
            }
            if (fb_diary_busy(&app->diary)) {
                set_status(app,
                           "diary: still consolidating the previous page -- try again in a moment");
                return;
            }
            fb_diary_close_page(&app->diary);
            fb_chat_reset(&app->diary_chat_job);
            fb_textview_clear(&app->diary_live_view);
            fb_textview_add_line_styled(&app->diary_live_view, "-- new page --",
                                         FB_TEXTVIEW_STYLE_HEADER);
            fb_textview_add_line(&app->diary_live_view, "");
            set_status_fmt(app, "diary: started a new page (%d archived so far)",
                           app->diary.archived_count);
            return;
        }
        if (app->input_mode == FB_INPUT_MODE_SCRATCHPAD) {
            /* Scratchpad keeps no durable record by design -- unlike
               diary's always-safe close, this can actually lose the
               transcript, so it's a confirmation (handled in
               fb_app_handle_event, which owns the y/N/a key reply), not an
               immediate action. */
            app->scratchpad_new_page_confirm_pending = 1;
            set_status(app, "start a new scratchpad page? everything here will be lost unless "
                            "archived -- discard, archive, or cancel? (y/N/a)");
            return;
        }
        set_status(app, "/new_page only applies to scratchpad/diary mode (Tab to switch)");
        return;
    case FB_CMD_INGEST: {
        if (!app->knowledge_available) {
            set_status(app, "no embedding model configured -- set embedding_model in "
                            "config.json");
            return;
        }
        if (cmd->arg1[0] == '\0') {
            set_status(app, "usage: /ingest <path> [group] [summary]");
            return;
        }
        int is_summary = strcmp(cmd->arg3, "summary") == 0;
        int added = fb_knowledge_ingest_file(&app->knowledge, cmd->arg1, cmd->arg2, is_summary);
        if (added < 0) {
            set_status(app, "no embedding model configured");
        } else if (added == 0) {
            set_status_fmt(app, "'%s' produced no chunks (empty, unreadable, or all embed "
                                 "requests failed -- check <data_dir>/logs/%s.log)",
                           cmd->arg1, FB_EMBEDDING_BRAIN_NAME);
        } else {
            /* Report the group it actually landed in (explicit or
               auto-derived) so the caller can confirm a transcript and its
               summary really did link up, e.g. by /ingest-ing the
               counterpart next with the same group shown here. */
            const char *group = app->knowledge.chunks[app->knowledge.chunk_count - 1].group;
            set_status_fmt(app, "ingested '%s' as %s of group '%s': %d chunk%s added (%d total)",
                           cmd->arg1, is_summary ? "the summary" : "raw content", group, added,
                           added == 1 ? "" : "s", app->knowledge.chunk_count);
        }
        return;
    }
    case FB_CMD_RECALL: {
        if (!app->knowledge_available) {
            set_status(app, "no embedding model configured -- set embedding_model in "
                            "config.json");
            return;
        }
        if (cmd->arg1[0] == '\0') {
            set_status(app, "usage: /recall <query>");
            return;
        }
        FbKnowledgeMatch matches[5];
        int n = fb_knowledge_search(&app->knowledge, cmd->arg1, matches, 5);
        if (n < 0) {
            set_status(app, "recall failed (embedding request error -- check "
                            "<data_dir>/logs/" FB_EMBEDDING_BRAIN_NAME ".log)");
        } else if (n == 0) {
            set_status(app, "nothing in the knowledge store yet -- /ingest a file first");
        } else {
            /* If the top hit belongs to a group and isn't itself the
               summary, pull that group's summary in too (see
               fb_knowledge_find_summary) -- so a matched transcript detail
               always surfaces its meeting's overall summary alongside it,
               not just the isolated sentence that happened to score
               highest. */
            FbKnowledgeMatch summary;
            int has_summary = 0;
            if (!matches[0].is_summary) {
                has_summary = fb_knowledge_find_summary(&app->knowledge, matches[0].group, &summary);
            }

            char line[FB_CMDLINE_MAX];
            size_t off = 0;
            if (has_summary) {
                char excerpt[80];
                snprintf(excerpt, sizeof(excerpt), "%s", summary.text);
                int k = snprintf(line + off, sizeof(line) - off, "[summary of '%s'] %s  ",
                                  summary.group, excerpt);
                if (k > 0) off += (size_t)k;
            }
            for (int i = 0; i < n && off < sizeof(line) - 1; ++i) {
                char excerpt[80];
                snprintf(excerpt, sizeof(excerpt), "%s", matches[i].text);
                int k = snprintf(line + off, sizeof(line) - off, "%s[%.2f %s] %s", i > 0 ? "  " : "",
                                  matches[i].score, matches[i].source, excerpt);
                if (k > 0) off += (size_t)k;
            }
            set_status(app, line);
        }
        return;
    }
    case FB_CMD_LEADERBOARD:
        fb_calibr_load_leaderboard(&app->leaderboard_view, app->calibr_root);
        app->view = FB_VIEW_LEADERBOARD;
        set_status(app, app->calibr_root[0] ? "leaderboard -- arrows to scroll, esc to go back"
                                             : "set calibr_root in config.json to view calibr results");
        return;
    case FB_CMD_LOGS:
        app->view = FB_VIEW_LOGS;
        set_status(app, app->calibr_root[0] ? "logs -- arrows to scroll, esc to go back"
                                             : "set calibr_root in config.json to view calibr logs");
        return;
    case FB_CMD_DASHBOARD:
        app->view = FB_VIEW_DASHBOARD;
        set_status(app, "back to dashboard");
        return;
    case FB_CMD_HELP:
        set_status(app,
                   "/load <name> [backend] /unload /add <name> <path> [llama|siliang|vllm|sglang|"
                   "all] [extra args] /remove <name> [backend] /list /new_page /lang "
                   "/ingest /recall /leaderboard /logs /dashboard /download /help /quit -- ctrl+space toggles "
                   "mic recording");
        return;
    case FB_CMD_QUIT:
        app->running = 0;
        return;
    case FB_CMD_UNKNOWN:
    default:
        set_status(app, "unknown command (try /help)");
        return;
    }
}

static int prefix_match_ci(const char *name, const char *prefix, size_t prefix_len) {
    for (size_t i = 0; i < prefix_len; ++i) {
        char a = name[i], b = prefix[i];
        if (a == '\0') return 0;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}

static size_t longest_common_prefix(const char *const *names, int count) {
    size_t len = strlen(names[0]);
    for (int i = 1; i < count; ++i) {
        size_t j = 0;
        while (j < len && names[i][j] != '\0' && names[i][j] == names[0][j]) j++;
        if (j < len) len = j;
    }
    return len;
}

void fb_command_autocomplete(FbApp *app, char *buf, size_t *len, size_t cap) {
    size_t verb_len;
    int is_load;
    if (strncmp(buf, "/load ", 6) == 0) { verb_len = 6; is_load = 1; }
    else if (strncmp(buf, "/unload ", 8) == 0) { verb_len = 8; is_load = 0; }
    else return;

    const char *partial = buf + verb_len;
    size_t partial_len = strlen(partial);
    for (size_t i = 0; i < partial_len; ++i) {
        if (partial[i] == ' ') return; /* already past the name argument */
    }

    const char *candidates[FB_CONFIG_MAX_MODELS];
    int count = 0;
    if (is_load) {
        for (int i = 0; i < app->config.model_count && count < FB_CONFIG_MAX_MODELS; ++i) {
            if (prefix_match_ci(app->config.models[i].name, partial, partial_len)) {
                candidates[count++] = app->config.models[i].name;
            }
        }
    } else {
        for (int i = 0; i < app->backend_mgr.count && count < FB_CONFIG_MAX_MODELS; ++i) {
            if (prefix_match_ci(app->backend_mgr.items[i].name, partial, partial_len)) {
                candidates[count++] = app->backend_mgr.items[i].name;
            }
        }
    }
    if (count == 0) return;

    size_t common_len = longest_common_prefix(candidates, count);
    if (common_len <= partial_len) return; /* nothing new, or genuinely ambiguous */

    size_t new_total = verb_len + common_len;
    if (new_total >= cap) return;

    memcpy(buf + verb_len, candidates[0], common_len);
    *len = new_total;

    if (count == 1 && new_total + 1 < cap) {
        buf[new_total] = ' ';
        new_total++;
        *len = new_total;
    }
    buf[new_total] = '\0';
}
