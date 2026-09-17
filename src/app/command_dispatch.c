#include "app/command_dispatch.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "backend/backend_mgr.h"
#include "calibr/calibr_results.h"
#include "config/config.h"
#include "platform/dirlist.h"
#include "util/paths.h"

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
        } else if (app->backend_mgr.count == 1) {
            strncpy(name, app->backend_mgr.items[0].name, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
        } else {
            set_status(app, app->backend_mgr.count == 0
                                 ? "usage: /unload <model> (nothing loaded)"
                                 : "usage: /unload <model> (multiple loaded -- tab-completes)");
            return;
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
        if (cmd->arg1[0] == '\0') {
            set_status(app, "usage: /remove <model>");
            return;
        }
        fb_backend_mgr_unload(&app->backend_mgr, cmd->arg1); /* no-op if not loaded */
        if (fb_config_remove_model(&app->config, cmd->arg1) != 0) {
            set_status_fmt(app, "'%s' is not registered", cmd->arg1);
            return;
        }
        fb_config_save(&app->config, app->data_dir);
        set_status_fmt(app, "removed '%s'", cmd->arg1);
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
        set_status(app, "/load /unload /add <name> <path> /remove /lang /leaderboard /logs "
                        "/dashboard /download /help /quit -- ctrl+space toggles mic recording");
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
