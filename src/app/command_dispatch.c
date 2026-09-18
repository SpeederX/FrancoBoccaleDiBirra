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
#include "platform/dirlist.h"
#include "tui/widget_textview.h"
#include "util/paths.h"

/* The always-resident Qwen brain lives in backend_mgr right alongside any
   user-loaded model, so a plain backend_mgr.count == 1 check for "just one
   [user] model loaded" is wrong whenever the brain is running too --
   confirmed the hard way: with the brain up, count is 2 even with a single
   real model loaded, so /unload's auto-select silently never fired. This
   counts (and finds) only the non-brain entries. */
static int count_non_brain_backends(const FbBackendMgr *mgr, const char **out_sole_name) {
    int count = 0;
    for (int i = 0; i < mgr->count; ++i) {
        if (strcmp(mgr->items[i].name, FB_QWEN_BRAIN_NAME) != 0) {
            if (out_sole_name) *out_sole_name = mgr->items[i].name;
            count++;
        }
    }
    return count;
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
        copy_trimmed(cmd.arg2, sizeof(cmd.arg2), path_part, strlen(path_part));
    } else if (cmd.kind == FB_CMD_LOAD || cmd.kind == FB_CMD_UNLOAD ||
               cmd.kind == FB_CMD_REMOVE || cmd.kind == FB_CMD_LANG ||
               cmd.kind == FB_CMD_DOWNLOAD) {
        size_t arg_len = (cmd.kind == FB_CMD_DOWNLOAD) ? strlen(rest) : token_len(rest);
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
            if (app->config.model_count == 1) {
                name = app->config.models[0].name;
            } else {
                set_status(app, app->config.model_count == 0
                                     ? "usage: /load <model> (nothing registered yet -- try /add)"
                                     : "usage: /load <model> (multiple registered -- tab-completes)");
                return;
            }
        }
        char path[FB_BACKEND_PATH_MAX];
        if (!fb_config_find_model(&app->config, name, path, sizeof(path))) {
            set_status_fmt(app, "unknown model '%s' (use /add <path> first)", name);
            return;
        }
        if (fb_path_is_dir(path)) {
            set_status_fmt(app, "'%s' is registered as a directory, not a .gguf file -- "
                                 "/remove %s then /add it again via the file picker",
                           path, name);
            return;
        }
        FILE *probe = fopen(path, "rb");
        if (!probe) {
            set_status_fmt(app, "can't read '%s' for '%s' (moved or deleted?)", path, name);
            return;
        }
        fclose(probe);
        if (fb_backend_mgr_load(&app->backend_mgr, name, path, app->llama_server_path,
                                 app->data_dir, 0, NULL) != 0) {
            set_status_fmt(app, "failed to start '%s' (already loaded, registry full, or "
                                 "llama-server not found — check FB_LLAMA_SERVER)",
                           name);
            return;
        }
        FbBackendInstance *b = fb_backend_mgr_find(&app->backend_mgr, name);
        set_status_fmt(app, "loading '%s' on port %d...", name, b ? b->port : -1);
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
        if (fb_path_is_dir(cmd->arg2)) {
            set_status_fmt(app, "'%s' is a directory -- point at the actual .gguf file inside it "
                                 "(or just /add with no path to browse)",
                           cmd->arg2);
            return;
        }
        FILE *probe = fopen(cmd->arg2, "rb");
        if (!probe) {
            set_status_fmt(app, "can't read '%s' (check the path)", cmd->arg2);
            return;
        }
        fclose(probe);
        fb_config_add_model(&app->config, cmd->arg1, cmd->arg2);
        fb_config_save(&app->config, app->data_dir);
        set_status_fmt(app, "added '%s' -> %s", cmd->arg1, cmd->arg2);
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
        fb_backend_mgr_unload(&app->backend_mgr, name); /* no-op if not loaded */
        if (fb_config_remove_model(&app->config, name) != 0) {
            set_status_fmt(app, "'%s' is not registered", name);
            return;
        }
        fb_config_save(&app->config, app->data_dir);
        set_status_fmt(app, "removed '%s'", name);
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
        set_status(app, "/download: not implemented yet");
        return;
    case FB_CMD_LIST: {
        if (app->config.model_count == 0) {
            set_status(app, "no models registered -- /add one first");
            return;
        }
        char line[FB_CMDLINE_MAX];
        size_t off = 0;
        for (int i = 0; i < app->config.model_count && off < sizeof(line) - 1; ++i) {
            FbBackendInstance *b = fb_backend_mgr_find(&app->backend_mgr, app->config.models[i].name);
            int n = snprintf(line + off, sizeof(line) - off, "%s%s%s", i > 0 ? "  " : "",
                              app->config.models[i].name, b ? " [loaded]" : "");
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
    case FB_CMD_LEADERBOARD:
        fb_calibr_load_leaderboard(&app->leaderboard_view, app->calibr_root);
        app->view = FB_VIEW_LEADERBOARD;
        set_status(app, app->calibr_root[0] ? "leaderboard -- arrows to scroll, esc to go back"
                                             : "set FB_CALIBR_ROOT to view calibr results");
        return;
    case FB_CMD_LOGS:
        app->view = FB_VIEW_LOGS;
        set_status(app, app->calibr_root[0] ? "logs -- arrows to scroll, esc to go back"
                                             : "set FB_CALIBR_ROOT to view calibr logs");
        return;
    case FB_CMD_DASHBOARD:
        app->view = FB_VIEW_DASHBOARD;
        set_status(app, "back to dashboard");
        return;
    case FB_CMD_HELP:
        set_status(app, "/load /unload /add <name> <path> /remove /list /new_page /lang "
                        "/leaderboard /logs /dashboard /download /help /quit -- ctrl+space "
                        "toggles mic recording");
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
