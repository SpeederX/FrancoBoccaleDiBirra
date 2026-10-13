#include "app/app.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app/command_dispatch.h"
#include "audio/mic_capture.h"
#include "audio/wav_writer.h"
#include "calibr/calibr_paths.h"
#include "intent/intent_brain.h"
#include "platform/platform.h"
#include "tui/fx_hypercube.h"
#include "tui/fx_pencil.h"
#include "tui/fx_sphere.h"
#include "tui/fx_tetris.h"
#include "tui/fx_waveform.h"
#include "tui/layout.h"
#include "tui/palette.h"
#include "util/paths.h"

void fb_app_init(FbApp *app, int width, int height) {
    fb_app_init_mode(app, width, height, FB_INPUT_MODE_COMMAND);
}

void fb_app_init_mode(FbApp *app, int width, int height, FbInputMode mode) {
    memset(app, 0, sizeof(*app));
    app->input_mode = mode;
    app->running = 1;
    app->width = width;
    app->height = height;
    app->start_ms = fb_now_ms();
    fb_cmdline_init(&app->cmdline);
    fb_chat_job_init(&app->chat_job);
    fb_chat_job_init(&app->diary_chat_job);
    fb_download_job_init(&app->download_job);

    if (mode != FB_INPUT_MODE_MEETING) {
        fb_mic_init();
    }
    app->mic_available = fb_mic_state() == FB_MIC_READY;
    if (fb_get_data_dir(app->data_dir, sizeof(app->data_dir)) != 0) {
        app->data_dir[0] = '\0';
    }

    fb_backend_mgr_init(&app->backend_mgr);
    fb_config_load(&app->config, app->data_dir);
    fb_meeting_init(&app->meeting, &app->config);
    fb_resolve_llama_server(&app->config, app->llama_server_path, sizeof(app->llama_server_path));
    app->transcribe_available =
        (fb_resolve_transcribe_config(&app->config, app->data_dir, &app->transcribe_cfg) == 0);

    if (fb_calibr_root(&app->config, app->calibr_root, sizeof(app->calibr_root)) != 0) {
        app->calibr_root[0] = '\0';
    }
    fb_calibr_log_tail_init(&app->log_tail);

    char qwen_model[FB_BACKEND_PATH_MAX];
    app->qwen_available = (fb_resolve_qwen_model(&app->config, qwen_model, sizeof(qwen_model)) == 0);
    if (app->qwen_available) {
        /* The brain only ever classifies one short line into a fixed
           intent shape -- it has no business inheriting the model's full
           native context (llama-server's --ctx-size default is 0, i.e.
           "whatever the model was trained with", which for a modern Qwen
           checkpoint can be tens of thousands of tokens of KV-cache
           sitting idle). Capped hard so this always-resident process stays
           the lightweight one. */
        const char *extra_args[] = {"--reasoning", "off", "--ctx-size", "8192", NULL};
        fb_backend_mgr_load(&app->backend_mgr, FB_QWEN_BRAIN_NAME, qwen_model,
                             app->llama_server_path, app->data_dir, fb_resolve_qwen_port(&app->config),
                             extra_args);
    }

    /* Consolidation always targets the brain specifically, not whatever's
       loaded for diary's live chat -- port 0 (brain unconfigured) is a
       valid, handled state: pages still accumulate and archive, they just
       never get distilled into long-term memory until a brain exists. */
    fb_diary_init(&app->diary, app->data_dir,
                  app->qwen_available ? fb_resolve_qwen_port(&app->config) : 0,
                  app->config.diary_page_close_chars, app->config.diary_checkpoint_chars,
                  app->config.diary_idle_close_minutes);

    /* Same always-resident pattern as the Qwen brain, for the same
       reason: knowledge/knowledge.h's retrieval needs to be fast on
       every query, not pay a multi-second model-load cost each time. A
       small embedding model loads in low single-digit seconds (confirmed
       against embeddinggemma-300M directly), so this is cheap to keep up
       whenever it's configured at all. */
    char embedding_model[FB_BACKEND_PATH_MAX];
    app->knowledge_available =
        (fb_resolve_embedding_model(&app->config, embedding_model, sizeof(embedding_model)) == 0);
    if (app->knowledge_available) {
        const char *extra_args[] = {"--embedding", NULL};
        fb_backend_mgr_load(&app->backend_mgr, FB_EMBEDDING_BRAIN_NAME, embedding_model,
                             app->llama_server_path, app->data_dir,
                             fb_resolve_embedding_port(&app->config), extra_args);
    }
    fb_knowledge_init(&app->knowledge, app->data_dir,
                       app->knowledge_available ? fb_resolve_embedding_port(&app->config) : 0);
}

/* The history panel's own rect -- shared by history_append (to know the
   wrap width and visible row count) and fb_app_render (to actually draw
   into it), so the two can never disagree about where the panel is. */
static FbRect compute_history_area(FbApp *app) {
    FbLayout layout;
    fb_layout_compute(app->width, app->height, &layout);
    int split_w = layout.main_area.w >= 60 ? layout.main_area.w * 3 / 5 : layout.main_area.w;
    FbRect history_area = {layout.main_area.x + split_w, layout.main_area.y,
                            layout.main_area.w - split_w, layout.main_area.h};
    return history_area;
}

/* Appends one line to the given right-side panel (each mode owns a
   separate FbTextView -- see app.h). Auto-follows the newest line -- but
   only if the view was already at the bottom before this line arrived:
   PageUp/PageDown (see fb_app_handle_event) let you read back through
   older exchanges, and a new reply landing mid-scroll shouldn't yank that
   view out from under you. Catching up requires scrolling to the bottom
   again, same as any chat/terminal scrollback. */
static void history_append(FbApp *app, FbTextView *view, const char *line) {
    int visible_rows = compute_history_area(app).h;
    int old_max_scroll = view->count - visible_rows;
    if (old_max_scroll < 0) old_max_scroll = 0;
    int was_at_bottom = view->scroll >= old_max_scroll;

    fb_textview_add_line(view, line);

    if (!was_at_bottom) return;
    int max_scroll = view->count - visible_rows;
    view->scroll = max_scroll > 0 ? max_scroll : 0;
}

/* Splits text on '\n' and greedily word-wraps each resulting paragraph to
   the history panel's current width, appending one panel line per visual
   row. FbTextView itself never wraps or respects embedded newlines -- a
   chat reply routinely has both (paragraphs, lists) and a naive single
   fb_textview_add_line call would render the whole thing as one run-on
   line with newlines collapsed to blanks. label prefixes only the very
   first emitted line (e.g. "You: " or "gemma4: "), so the reply still
   reads as one turn rather than a wall of unattributed lines.
   Deliberately shows the model's raw text verbatim -- an earlier pass
   stripped markdown syntax (headers, **bold**, `code`) before wrapping,
   but it visibly mangled real replies (see git history), so this is back
   to plain passthrough rather than a half-working cleanup. */
static void history_append_multiline(FbApp *app, FbTextView *view, const char *label,
                                      const char *text) {
    int wrap_w = compute_history_area(app).w;
    if (wrap_w <= 8) wrap_w = FB_TEXTVIEW_LINE_MAX - 1; /* panel hidden/too narrow: fall back */
    if (wrap_w > FB_TEXTVIEW_LINE_MAX - 1) wrap_w = FB_TEXTVIEW_LINE_MAX - 1;

    char prefix[64];
    int prefix_len = snprintf(prefix, sizeof(prefix), "%s: ", label);
    int first_line = 1;

    const char *raw_para = text;
    for (;;) {
        const char *nl = strchr(raw_para, '\n');
        size_t para_len = nl ? (size_t)(nl - raw_para) : strlen(raw_para);
        const char *para = raw_para;

        if (para_len == 0) {
            history_append(app, view, first_line ? prefix : "");
            first_line = 0;
        } else {
            size_t off = 0;
            while (off < para_len) {
                int room = wrap_w - (first_line ? prefix_len : 0);
                if (room < 8) room = 8;
                size_t take = para_len - off;
                if (take > (size_t)room) {
                    size_t cut = (size_t)room;
                    size_t last_space = 0;
                    int found = 0;
                    for (size_t i = cut; i > 0; --i) {
                        if (para[off + i - 1] == ' ') {
                            last_space = i - 1;
                            found = 1;
                            break;
                        }
                    }
                    take = found ? last_space : cut; /* no space in range: hard-cut one long token */
                }

                char line[FB_TEXTVIEW_LINE_MAX];
                int n = first_line ? snprintf(line, sizeof(line), "%s", prefix) : 0;
                size_t room_left = sizeof(line) - 1 - (size_t)n;
                if (take > room_left) take = room_left;
                memcpy(line + n, para + off, take);
                line[(size_t)n + take] = '\0';
                history_append(app, view, line);
                first_line = 0;

                off += take;
                while (off < para_len && para[off] == ' ') off++; /* skip the space we wrapped on */
                if (take == 0) break; /* room was too small even for one char: avoid an infinite loop */
            }
        }

        if (!nl) break;
        raw_para = nl + 1;
    }
}

/* The model chat mode talks to: the most recently loaded non-brain
   backend. Scanning from the end works because fb_backend_mgr_load always
   appends, and fb_backend_mgr_unload shifts the array down to close the
   gap -- so items[count-1] is always the latest surviving load, skipping
   back over the brain on the rare chance it ended up last (e.g. every user
   model was unloaded, or the brain itself was reloaded). */
static FbBackendInstance *find_chat_target(FbApp *app) {
    for (int i = app->backend_mgr.count - 1; i >= 0; --i) {
        if (strcmp(app->backend_mgr.items[i].name, FB_QWEN_BRAIN_NAME) != 0) {
            return &app->backend_mgr.items[i];
        }
    }
    return NULL;
}

/* The model an *ongoing* scratchpad/diary conversation is actually
   talking to should stay pinned to whatever it already started with, not
   silently drift to "most recently loaded" the moment you load some other
   model for an unrelated reason (e.g. testing something in scratchpad
   while a diary conversation is mid-flight in another mode). find_chat_
   target is only for picking a *fresh* target -- once job->turn_count > 0,
   this looks the cached name back up by name instead, falling back to
   find_chat_target only if that specific backend went away (unloaded). */
static FbBackendInstance *resolve_chat_target(FbApp *app, const FbChatJob *job,
                                               const char *cached_name) {
    if (job->turn_count > 0) {
        FbBackendInstance *pinned = fb_backend_mgr_find(&app->backend_mgr, cached_name);
        if (pinned) return pinned;
    }
    return find_chat_target(app);
}

/* Shared by diary's and scratchpad's live chat: searches the ingested-
   knowledge store (see knowledge/knowledge.h) using `query` -- always the
   user's own raw typed text, never a message already wrapped with
   long-term-memory framing, which would dilute the embedding's semantic
   signal with unrelated priming text -- and, if any result scores above
   a minimum relevance, wraps `message_to_send` (which may already carry
   that long-term-memory prefix) with the matches prepended into
   scratch_buf, returning that; otherwise returns message_to_send
   unchanged. Runs every turn, not just the first -- unlike diary's own
   long-term memory (a one-time "here's who you are" primer), which
   meeting/customer record is relevant depends on what's actually being
   asked *this* message. Distinct framing from the long-term-memory
   prefix so the model (and anyone reading the raw prompt) can tell "your
   own past conversations" apart from "external reference material" --
   semantic memory, not episodic. A low-similarity result is worse than
   none (confidently-wrong-sounding noise), so nothing gets injected
   below min_score. */
static const char *inject_knowledge_context(FbApp *app, const char *query, const char *message_to_send,
                                             char *scratch_buf, size_t scratch_cap) {
    if (!app->knowledge_available) return message_to_send;

    FbKnowledgeMatch matches[3];
    int n = fb_knowledge_search(&app->knowledge, query, matches, 3);
    const float min_score = 0.3f;

    char notes[FB_CHAT_TURN_MAX / 2];
    size_t notes_off = 0;
    int used = 0;
    /* Groups whose summary has already been included this call, so a
       group with two matching detail chunks doesn't get its summary
       injected twice. */
    char seen_summary_groups[3][FB_KNOWLEDGE_SOURCE_MAX];
    int seen_count = 0;
    for (int i = 0; i < n; ++i) {
        if (matches[i].score < min_score) continue;

        /* A matched raw/transcript chunk is one detail out of a whole
           meeting -- also surface that meeting's own summary alongside
           it (see fb_knowledge_find_summary) so the model gets both the
           specific fact and the surrounding context, not an isolated
           sentence with no frame of reference. */
        if (!matches[i].is_summary && matches[i].group[0] != '\0') {
            int already = 0;
            for (int s = 0; s < seen_count; ++s) {
                if (strcmp(seen_summary_groups[s], matches[i].group) == 0) already = 1;
            }
            if (!already) {
                FbKnowledgeMatch summary;
                if (fb_knowledge_find_summary(&app->knowledge, matches[i].group, &summary)) {
                    int w = snprintf(notes + notes_off, sizeof(notes) - notes_off,
                                      "%s[%s summary] %s", used > 0 ? "\n" : "", summary.group,
                                      summary.text);
                    if (w > 0) notes_off += (size_t)w;
                    used++;
                }
                if (seen_count < 3) {
                    snprintf(seen_summary_groups[seen_count], FB_KNOWLEDGE_SOURCE_MAX, "%s",
                             matches[i].group);
                    seen_count++;
                }
            }
        }
        if (notes_off >= sizeof(notes) - 1) break;

        int w = snprintf(notes + notes_off, sizeof(notes) - notes_off, "%s[%s] %s",
                          used > 0 ? "\n" : "", matches[i].source, matches[i].text);
        if (w > 0) notes_off += (size_t)w;
        used++;
        if (notes_off >= sizeof(notes) - 1) break;
    }
    if (used == 0) return message_to_send;

    snprintf(scratch_buf, scratch_cap, "(Relevant notes from your ingested records:\n%s)\n\n%s",
             notes, message_to_send);
    return scratch_buf;
}

/* Rebuilds diary_browse_view from the currently browsed archived page (or
   clears it if browse_index is -1 / "live"). Plain '\n'-splitting, no
   word-wrap -- this is a read-only viewer for raw archived content, not
   the live history panel, so a long line just running off-screen (same as
   /leaderboard and /logs already tolerate) is an acceptable simplicity
   trade-off here. */
static void diary_rebuild_browse_view(FbApp *app) {
    fb_textview_clear(&app->diary_browse_view);
    if (app->diary.browse_index < 0) return;

    char text[FB_DIARY_PAGE_MAX_CHARS + 512];
    if (!fb_diary_read_browsed_page(&app->diary, text, sizeof(text))) {
        fb_textview_add_line(&app->diary_browse_view, "(couldn't read this page)");
        return;
    }

    char header[64];
    snprintf(header, sizeof(header), "-- archived page %d of %d --", app->diary.browse_index + 1,
             app->diary.archived_count);
    fb_textview_add_line_styled(&app->diary_browse_view, header, FB_TEXTVIEW_STYLE_HEADER);
    fb_textview_add_line(&app->diary_browse_view, "");

    char *line = text;
    while (line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        fb_textview_add_line(&app->diary_browse_view, line);
        line = nl ? nl + 1 : NULL;
    }
}

/* Reloads diary_live_view from scratch to show exactly `text` (plain
   '\n'-split, no word-wrap -- matches diary_rebuild_browse_view's own
   trade-off) under a header, or just the header alone for an empty fresh
   page. Used whenever the live page's identity actually changes (reopening
   an archived page, or /new_page) -- as opposed to an automatic idle/size
   close, which is an invisible seam within the *same* ongoing page and
   deliberately leaves the view untouched. */
static void diary_rebuild_live_view(FbApp *app, const char *header, const char *text) {
    fb_textview_clear(&app->diary_live_view);
    fb_textview_add_line_styled(&app->diary_live_view, header, FB_TEXTVIEW_STYLE_HEADER);
    fb_textview_add_line(&app->diary_live_view, "");
    if (!text) return;

    char buf[FB_DIARY_PAGE_MAX_CHARS + 512];
    snprintf(buf, sizeof(buf), "%s", text);
    char *line = buf;
    while (line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        history_append(app, &app->diary_live_view, line);
        line = nl ? nl + 1 : NULL;
    }
}

/* Makes the browsed archived page the live, editable one: whatever was
   open before (today's page, or a different reopened page) gets safely
   archived to its own file first, untouched and still fully intact --
   this page and that one never mix. New turns from here on extend this
   page's own file in place, not a new one. The model's conversational
   context is reset and re-primed with this page's own content rather than
   carrying over whatever it knew a moment ago, since this is now a
   deliberately different conversation. Only ever called after the user
   confirms the y/N prompt. */
static void reopen_archived_page(FbApp *app) {
    int index = app->diary.browse_index;
    int total = app->diary.archived_count;

    if (fb_diary_busy(&app->diary)) {
        snprintf(app->last_submitted, FB_CMDLINE_MAX,
                 "diary: still consolidating -- try reopening again in a moment");
        return; /* still browsing the same page -- nothing else changes */
    }

    char text[FB_DIARY_PAGE_MAX_CHARS + 512];
    int had_text = fb_diary_read_browsed_page(&app->diary, text, sizeof(text));

    if (!fb_diary_reopen_page(&app->diary, index)) {
        snprintf(app->last_submitted, FB_CMDLINE_MAX, "couldn't read that page");
        app->diary.browse_index = -1;
        diary_rebuild_browse_view(app);
        return;
    }

    char header[64];
    snprintf(header, sizeof(header), "-- editing archived page %d of %d --", index + 1, total);
    diary_rebuild_live_view(app, header, had_text ? text : NULL);

    /* Trailing excerpt, not a front truncation, matching the same trade-off
       already made for long-term-memory priming below: the most recent part
       of a long page is more likely to matter than its opening. */
    fb_chat_reset(&app->diary_chat_job);
    if (had_text) {
        size_t text_len = strlen(text);
        const char *excerpt = text;
        const size_t excerpt_max = 1600;
        if (text_len > excerpt_max) excerpt = text + (text_len - excerpt_max);

        char framed[FB_CHAT_TURN_MAX];
        snprintf(framed, sizeof(framed),
                 "(Reopening archived diary page %d of %d from an earlier session -- full raw "
                 "transcript below, continue this conversation:)\n\n%s",
                 index + 1, total, excerpt);
        fb_chat_inject_turn(&app->diary_chat_job, "user", framed);
    }

    diary_rebuild_browse_view(app); /* browse_index is now -1: clears it */
    snprintf(app->last_submitted, FB_CMDLINE_MAX,
             "diary: editing page %d of %d -- new messages extend it directly (F7 for a new page)",
             index + 1, total);
}

/* /new_page (F7) in scratchpad: unlike diary, scratchpad deliberately keeps
   no durable record at all -- a clean slate every time you leave and come
   back is the whole point (see fb_app_handle_event's Tab case) -- so
   starting a new one here actually discards something, unlike diary's
   always-safe close. `archive` (the prompt's "a" choice) writes the
   current transcript out first, a flat dump with no consolidation or
   long-term-memory pipeline (scratchpad intentionally has neither of
   those). Only ever called after the user answers the y/N/a prompt. */
static void scratchpad_start_new_page(FbApp *app, int archive) {
    if (archive && app->scratchpad_history_view.count > 0) {
        char dir[600];
        snprintf(dir, sizeof(dir), "%s/scratchpad", app->data_dir);
        fb_ensure_dir(dir);
        char path[700];
        snprintf(path, sizeof(path), "%s/%llu.txt", dir, fb_now_ms());
        FILE *f = fopen(path, "wb");
        if (f) {
            for (int i = 0; i < app->scratchpad_history_view.count; ++i) {
                fprintf(f, "%s\n", app->scratchpad_history_view.lines[i]);
            }
            fclose(f);
        }
    }
    fb_chat_reset(&app->chat_job);
    fb_textview_clear(&app->scratchpad_history_view);
    snprintf(app->last_submitted, FB_CMDLINE_MAX,
             archive ? "scratchpad: archived and started a new page"
                     : "scratchpad: started a new page (discarded)");
}

/* The panel actually shown on the right side right now -- shared by the
   PageUp/PageDown handler (so scrolling acts on whatever you're actually
   looking at) and fb_app_render (so rendering agrees with it). */
static FbTextView *current_panel_view(FbApp *app) {
    if (app->input_mode == FB_INPUT_MODE_DIARY) {
        return app->diary.browse_index >= 0 ? &app->diary_browse_view : &app->diary_live_view;
    }
    if (app->input_mode == FB_INPUT_MODE_SCRATCHPAD) return &app->scratchpad_history_view;
    return &app->history_view;
}


static void format_download_bytes(unsigned long long bytes, char *out, size_t out_cap) {
    const double gib = 1024.0 * 1024.0 * 1024.0;
    const double mib = 1024.0 * 1024.0;
    if ((double)bytes >= gib) {
        snprintf(out, out_cap, "%.2f GiB", (double)bytes / gib);
    } else if ((double)bytes >= mib) {
        snprintf(out, out_cap, "%.1f MiB", (double)bytes / mib);
    } else {
        snprintf(out, out_cap, "%llu KiB", bytes / 1024ULL);
    }
}

static const char *register_downloaded_model(FbApp *app) {
    /* Reuse an existing registration for the exact same path. Otherwise
       preserve the old name and add a stable numeric suffix. */
    for (int i = 0; i < app->config.model_count; ++i) {
        if (strcmp(app->config.models[i].path, app->download_job.final_path) == 0) {
            snprintf(app->download_job.model_name, sizeof(app->download_job.model_name), "%s",
                     app->config.models[i].name);
            return app->download_job.model_name;
        }
    }

    char base[sizeof(app->download_job.model_name)];
    snprintf(base, sizeof(base), "%s", app->download_job.model_name);
    char candidate[sizeof(app->download_job.model_name)];
    snprintf(candidate, sizeof(candidate), "%s", base);
    for (int suffix = 2;; ++suffix) {
        char ignored[512];
        if (!fb_config_find_model(&app->config, candidate, ignored, sizeof(ignored))) break;
        snprintf(candidate, sizeof(candidate), "%.110s-%d", base, suffix);
    }

    if (fb_config_add_model(&app->config, candidate, app->download_job.final_path) != 0) return NULL;
    snprintf(app->download_job.model_name, sizeof(app->download_job.model_name), "%s", candidate);
    return app->download_job.model_name;
}

static void tick_download(FbApp *app) {
    fb_download_job_tick(&app->download_job);
    if (app->download_job.state == FB_DOWNLOAD_RUNNING) {
        unsigned long long now = fb_now_ms();
        if (now - app->download_last_status_ms >= 500) {
            char amount[32];
            format_download_bytes(fb_download_job_bytes(&app->download_job), amount, sizeof(amount));
            snprintf(app->last_submitted, FB_CMDLINE_MAX,
                     "downloading '%s': %s received (/download cancel to stop)",
                     app->download_job.model_name, amount);
            app->download_last_status_ms = now;
        }
        return;
    }
    if (app->download_job.state == FB_DOWNLOAD_DONE) {
        const char *name = register_downloaded_model(app);
        if (!name) {
            snprintf(app->last_submitted, FB_CMDLINE_MAX,
                     "downloaded %s, but the model registry is full", app->download_job.final_path);
        } else if (fb_config_save(&app->config, app->data_dir) != 0) {
            snprintf(app->last_submitted, FB_CMDLINE_MAX,
                     "downloaded '%s', but couldn't save its registration", name);
        } else {
            snprintf(app->last_submitted, FB_CMDLINE_MAX,
                     "downloaded and registered '%s' -> %s", name, app->download_job.final_path);
        }
        fb_download_job_consume(&app->download_job);
    } else if (app->download_job.state == FB_DOWNLOAD_ERROR) {
        snprintf(app->last_submitted, FB_CMDLINE_MAX, "download failed: %s",
                 app->download_job.error);
        fb_download_job_consume(&app->download_job);
    }
}

void fb_app_tick(FbApp *app) {
    if (app->input_mode != FB_INPUT_MODE_MEETING && fb_mic_state() == FB_MIC_OFF) {
        fb_mic_init();
    }
    app->mic_available = fb_mic_state() == FB_MIC_READY;
    fb_meeting_tick(&app->meeting);
    if (app->meeting.settings_dirty) {
        app->meeting.settings_dirty = 0;
        if (fb_config_save(&app->config, app->data_dir) != 0) {
            snprintf(app->meeting.status, sizeof(app->meeting.status), "device selected for this session; could not save settings to config.json");
        }
    }
    fb_backend_mgr_tick(&app->backend_mgr);
    tick_download(app);

    if (app->view == FB_VIEW_LOGS) {
        fb_calibr_log_tail_poll(&app->log_tail, app->calibr_root, &app->log_view);
    }

    fb_chat_tick(&app->chat_job);
    if (app->chat_job.state == FB_CHAT_DONE) {
        history_append_multiline(app, &app->scratchpad_history_view, app->chat_target_name,
                                  app->chat_job.response);
        history_append(app, &app->scratchpad_history_view, "");
        fb_chat_consume(&app->chat_job);
    } else if (app->chat_job.state == FB_CHAT_ERROR) {
        char line[FB_TEXTVIEW_LINE_MAX];
        snprintf(line, sizeof(line), "(chat with %s failed: %s)", app->chat_target_name,
                 app->chat_job.response);
        history_append(app, &app->scratchpad_history_view, line);
        history_append(app, &app->scratchpad_history_view, "");
        fb_chat_consume(&app->chat_job);
    }

    fb_chat_tick(&app->diary_chat_job);
    if (app->diary_chat_job.state == FB_CHAT_DONE) {
        history_append_multiline(app, &app->diary_live_view, app->diary_chat_target_name,
                                  app->diary_chat_job.response);
        history_append(app, &app->diary_live_view, "");
        fb_diary_append_turn(&app->diary, app->diary_chat_target_name,
                              app->diary_chat_job.response, fb_now_ms());
        fb_chat_consume(&app->diary_chat_job);
    } else if (app->diary_chat_job.state == FB_CHAT_ERROR) {
        char line[FB_TEXTVIEW_LINE_MAX];
        snprintf(line, sizeof(line), "(diary chat with %s failed: %s)",
                 app->diary_chat_target_name, app->diary_chat_job.response);
        history_append(app, &app->diary_live_view, line);
        history_append(app, &app->diary_live_view, "");
        fb_chat_consume(&app->diary_chat_job);
    }
    fb_diary_tick(&app->diary, fb_now_ms());
    if (app->diary.just_saved) {
        snprintf(app->last_submitted, FB_CMDLINE_MAX,
                 "diary: autosaved (%d pages, %zu chars memory)", app->diary.archived_count,
                 app->diary.long_term_len);
        app->diary.just_saved = 0;
    }

    if (app->transcribe_job.state == FB_TRANSCRIBE_RUNNING) {
        int attempt_before = app->transcribe_job.attempt;
        fb_transcribe_tick(&app->transcribe_job);

        if (app->transcribe_job.state == FB_TRANSCRIBE_DONE) {
            snprintf(app->cmdline.buf, FB_CMDLINE_MAX, "%s", app->transcribe_job.result_text);
            app->cmdline.len = strlen(app->cmdline.buf);
            app->cmdline.cursor = app->cmdline.len;
            snprintf(app->last_submitted, FB_CMDLINE_MAX,
                     "transcribed -- review/edit, then press enter");
        } else if (app->transcribe_job.state == FB_TRANSCRIBE_ERROR) {
            app->transcribe_confirm_pending = 1;
            snprintf(app->last_submitted, FB_CMDLINE_MAX,
                     "transcription failed after %d attempts -- retry? (y/N)",
                     FB_TRANSCRIBE_MAX_ATTEMPTS);
        } else if (app->transcribe_job.attempt != attempt_before) {
            /* still RUNNING, but fb_transcribe_tick just kicked off a retry */
            snprintf(app->last_submitted, FB_CMDLINE_MAX,
                     "transcription failed (attempt %d of %d) -- retrying...",
                     attempt_before, FB_TRANSCRIBE_MAX_ATTEMPTS);
        }
    }
}

void fb_app_shutdown(FbApp *app) {
    fb_meeting_shutdown(&app->meeting);
    fb_download_job_shutdown(&app->download_job);
    fb_chat_shutdown(&app->chat_job); /* joins any in-flight request before app is freed */
    fb_chat_shutdown(&app->diary_chat_job);
    fb_diary_shutdown(&app->diary);
    fb_backend_mgr_shutdown(&app->backend_mgr);
    fb_mic_shutdown();
}

void fb_app_push_wave_sample(FbApp *app, float amplitude) {
    memmove(app->wave_samples, app->wave_samples + 1,
            (FB_WAVE_HISTORY - 1) * sizeof(float));
    app->wave_samples[FB_WAVE_HISTORY - 1] = amplitude;
}

static int contains_ci(const char *haystack, const char *needle) {
    size_t h_len = strlen(haystack), n_len = strlen(needle);
    if (n_len == 0 || h_len < n_len) return 0;
    for (size_t i = 0; i + n_len <= h_len; ++i) {
        size_t j = 0;
        for (; j < n_len; ++j) {
            char a = haystack[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
        }
        if (j == n_len) return 1;
    }
    return 0;
}

/* GGUF filenames are often generic within a model's own folder (quant/
   variant info lives in the directory name instead, e.g.
   ".../gemma4/E4B/model.gguf") — so the default name folds in the
   immediate parent directory too, unless it's already redundant with the
   filename or is a bare drive letter. "D:/models/gemma4/E4B/model.gguf"
   -> "E4B-model". */
static void derive_default_name(const char *path, char *out, size_t cap) {
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *sep = (slash && bslash) ? (slash > bslash ? slash : bslash)
                       : (slash ? slash : bslash);
    const char *filename = sep ? sep + 1 : path;

    char stem[FB_BACKEND_NAME_MAX];
    size_t stem_len = strlen(filename);
    if (stem_len > 5 && strcmp(filename + stem_len - 5, ".gguf") == 0) stem_len -= 5;
    if (stem_len >= sizeof(stem)) stem_len = sizeof(stem) - 1;
    memcpy(stem, filename, stem_len);
    stem[stem_len] = '\0';

    char parent[128];
    parent[0] = '\0';
    if (sep) {
        const char *dir_start = sep;
        while (dir_start > path && dir_start[-1] != '/' && dir_start[-1] != '\\') dir_start--;
        size_t parent_len = (size_t)(sep - dir_start);
        if (parent_len > 0 && parent_len < sizeof(parent)) {
            memcpy(parent, dir_start, parent_len);
            parent[parent_len] = '\0';
        }
    }

    int parent_is_drive_letter = strlen(parent) == 2 && parent[1] == ':';
    if (parent[0] != '\0' && !parent_is_drive_letter && !contains_ci(stem, parent)) {
        snprintf(out, cap, "%s-%s", parent, stem);
    } else {
        snprintf(out, cap, "%s", stem);
    }
}

int fb_app_handle_event(FbApp *app, const FbEvent *ev) {
    if (ev->type == FB_EVENT_QUIT) {
        app->running = 0;
        return 1;
    }

    if (ev->type == FB_EVENT_RESIZE) {
        app->width = ev->width;
        app->height = ev->height;
        return 0;
    }

    if (ev->type == FB_EVENT_KEY) {
        if (ev->key == FB_KEY_CTRL_SPACE && app->meeting.read_job && app->input_mode != FB_INPUT_MODE_MEETING) {
            snprintf(app->last_submitted, FB_CMDLINE_MAX, "Piper is reading; /meeting read stop ends playback before recording");
            return 0;
        }
        if (ev->key == FB_KEY_CTRL_C) {
            app->running = 0;
            return 1;
        }

        if (app->input_mode == FB_INPUT_MODE_MEETING && ev->key == FB_KEY_F1) {
            fb_meeting_command(&app->meeting, "help");
            return 0;
        }

        if (app->transcribe_confirm_pending) {
            if (ev->key == FB_KEY_CHAR && (ev->ch == 'y' || ev->ch == 'Y')) {
                app->transcribe_confirm_pending = 0;
                char wav_copy[600];
                snprintf(wav_copy, sizeof(wav_copy), "%s", app->transcribe_job.wav_path);
                fb_transcribe_start(&app->transcribe_job, &app->transcribe_cfg, wav_copy,
                                     app->data_dir);
                snprintf(app->last_submitted, FB_CMDLINE_MAX,
                         "transcribing... (attempt 1 of %d, ~6s for model load)",
                         FB_TRANSCRIBE_MAX_ATTEMPTS);
                return 0;
            }
            app->transcribe_confirm_pending = 0;
            if ((ev->key == FB_KEY_CHAR && (ev->ch == 'n' || ev->ch == 'N')) ||
                ev->key == FB_KEY_ESC) {
                snprintf(app->last_submitted, FB_CMDLINE_MAX, "transcription discarded");
                return 0;
            }
            /* any other key: treat as an implicit "no" but still let it
               through normally (e.g. the user just started typing a
               command instead of answering) */
        }

        if (app->diary_reopen_confirm_pending) {
            if (ev->key == FB_KEY_CHAR && (ev->ch == 'y' || ev->ch == 'Y')) {
                app->diary_reopen_confirm_pending = 0;
                reopen_archived_page(app);
                return 0;
            }
            app->diary_reopen_confirm_pending = 0;
            if ((ev->key == FB_KEY_CHAR && (ev->ch == 'n' || ev->ch == 'N')) ||
                ev->key == FB_KEY_ESC) {
                snprintf(app->last_submitted, FB_CMDLINE_MAX, "reopen cancelled");
                return 0;
            }
            /* any other key: treat as an implicit "no" but still let it
               through normally */
        }

        if (app->scratchpad_new_page_confirm_pending) {
            if (ev->key == FB_KEY_CHAR && (ev->ch == 'y' || ev->ch == 'Y')) {
                app->scratchpad_new_page_confirm_pending = 0;
                scratchpad_start_new_page(app, 0);
                return 0;
            }
            if (ev->key == FB_KEY_CHAR && (ev->ch == 'a' || ev->ch == 'A')) {
                app->scratchpad_new_page_confirm_pending = 0;
                scratchpad_start_new_page(app, 1);
                return 0;
            }
            app->scratchpad_new_page_confirm_pending = 0;
            if ((ev->key == FB_KEY_CHAR && (ev->ch == 'n' || ev->ch == 'N')) ||
                ev->key == FB_KEY_ESC) {
                snprintf(app->last_submitted, FB_CMDLINE_MAX, "new page cancelled");
                return 0;
            }
            /* any other key: treat as an implicit "no" but still let it
               through normally */
        }

        if (app->view != FB_VIEW_DASHBOARD) {
            if (ev->key == FB_KEY_ESC) {
                app->view = FB_VIEW_DASHBOARD;
                return 0;
            }
            if (ev->key == FB_KEY_ARROW_UP || ev->key == FB_KEY_ARROW_DOWN) {
                FbLayout layout;
                fb_layout_compute(app->width, app->height, &layout);
                int visible_rows = layout.main_area.h;
                FbTextView *tv =
                    (app->view == FB_VIEW_LEADERBOARD) ? &app->leaderboard_view : &app->log_view;
                fb_textview_handle_event(tv, ev, visible_rows);
                return 0;
            }
            /* anything else (typing, enter) still reaches the command line
               below, so /dashboard, /load, etc. keep working while viewing */
        }

        if (app->file_picker.active) {
            char path[FB_BACKEND_PATH_MAX];
            FbPickerResult r = fb_file_picker_handle_event(&app->file_picker, ev, path, sizeof(path));
            if (r == FB_PICKER_SELECTED) {
                strncpy(app->config.last_browse_dir, app->file_picker.cwd,
                        sizeof(app->config.last_browse_dir) - 1);
                fb_config_save(&app->config, app->data_dir);

                char name[FB_BACKEND_NAME_MAX];
                if (app->file_picker_pending_name[0] != '\0') {
                    strncpy(name, app->file_picker_pending_name, sizeof(name) - 1);
                    name[sizeof(name) - 1] = '\0';
                } else {
                    derive_default_name(path, name, sizeof(name));
                }

                if (app->file_picker_pending_load) {
                    /* F2 with nothing registered yet: add and load in one
                       shot instead of leaving the user to press Enter and
                       then type /load themselves. */
                    FbCommand add_cmd;
                    memset(&add_cmd, 0, sizeof(add_cmd));
                    add_cmd.kind = FB_CMD_ADD;
                    strncpy(add_cmd.arg1, name, sizeof(add_cmd.arg1) - 1);
                    strncpy(add_cmd.arg2, path, sizeof(add_cmd.arg2) - 1);
                    fb_command_execute(app, &add_cmd);

                    char check_path[FB_BACKEND_PATH_MAX];
                    if (fb_config_find_model(&app->config, name, check_path, sizeof(check_path))) {
                        FbCommand load_cmd;
                        memset(&load_cmd, 0, sizeof(load_cmd));
                        load_cmd.kind = FB_CMD_LOAD;
                        strncpy(load_cmd.arg1, name, sizeof(load_cmd.arg1) - 1);
                        fb_chat_reset(&app->chat_job);
                        fb_chat_reset(&app->diary_chat_job);
                        fb_command_execute(app, &load_cmd);
                    }
                } else {
                    snprintf(app->cmdline.buf, FB_CMDLINE_MAX, "/add %s %s", name, path);
                    app->cmdline.len = strlen(app->cmdline.buf);
                    app->cmdline.cursor = app->cmdline.len;
                    snprintf(app->last_submitted, FB_CMDLINE_MAX,
                             "picked %s -- press enter to add as '%s'", path, name);
                }
                app->file_picker_pending_load = 0;
            } else if (r == FB_PICKER_CANCELLED) {
                snprintf(app->last_submitted, FB_CMDLINE_MAX, "file picker cancelled");
                app->file_picker_pending_load = 0;
            }
            return 0;
        }

        /* Quick-action shortcuts: the same commands as /help /load /unload
           /add /remove, without typing them out. F2 additionally chains
           into /add's file picker when nothing is registered yet, then
           loads whatever gets picked -- see the FB_PICKER_SELECTED
           handling above. */
        if (ev->key == FB_KEY_F1) {
            FbCommand help_cmd;
            memset(&help_cmd, 0, sizeof(help_cmd));
            help_cmd.kind = FB_CMD_HELP;
            fb_command_execute(app, &help_cmd);
            return 0;
        }
        if (ev->key == FB_KEY_F2) {
            FbCommand cmd;
            memset(&cmd, 0, sizeof(cmd));
            if (app->config.model_count == 0) {
                cmd.kind = FB_CMD_ADD;
                app->file_picker_pending_load = 1;
            } else {
                cmd.kind = FB_CMD_LOAD;
                fb_chat_reset(&app->chat_job);
                fb_chat_reset(&app->diary_chat_job);
            }
            fb_command_execute(app, &cmd);
            return 0;
        }
        if (ev->key == FB_KEY_F3) {
            FbCommand cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.kind = FB_CMD_UNLOAD;
            fb_command_execute(app, &cmd);
            return 0;
        }
        if (ev->key == FB_KEY_F4) {
            FbCommand cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.kind = FB_CMD_ADD;
            app->file_picker_pending_load = 0;
            fb_command_execute(app, &cmd);
            return 0;
        }
        if (ev->key == FB_KEY_F5) {
            FbCommand cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.kind = FB_CMD_REMOVE;
            fb_command_execute(app, &cmd);
            return 0;
        }
        if (ev->key == FB_KEY_F6) {
            if (app->input_mode == FB_INPUT_MODE_MEETING) {
                fb_meeting_handle_event(&app->meeting, ev);
                return 0;
            }
            FbCommand cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.kind = FB_CMD_LIST;
            fb_command_execute(app, &cmd);
            return 0;
        }
        if (ev->key == FB_KEY_F7) {
            if (app->input_mode == FB_INPUT_MODE_MEETING) {
                fb_meeting_command(&app->meeting, "new");
                return 0;
            }
            FbCommand cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.kind = FB_CMD_NEW_PAGE;
            fb_command_execute(app, &cmd);
            return 0;
        }
        if (ev->key == FB_KEY_F10) {
            app->running = 0;
            return 1;
        }

        if (app->input_mode == FB_INPUT_MODE_MEETING && app->view == FB_VIEW_DASHBOARD &&
            (ev->key == FB_KEY_F8 || ev->key == FB_KEY_F9 ||
             ev->key == FB_KEY_PAGE_UP || ev->key == FB_KEY_PAGE_DOWN)) {
            fb_meeting_handle_event(&app->meeting, ev);
            return 0;
        }

        if (app->view == FB_VIEW_DASHBOARD &&
            (ev->key == FB_KEY_PAGE_UP || ev->key == FB_KEY_PAGE_DOWN)) {
            FbTextView *view = current_panel_view(app);
            int visible_rows = compute_history_area(app).h;
            int max_scroll = view->count - visible_rows;
            if (max_scroll < 0) max_scroll = 0;
            int delta = visible_rows > 1 ? visible_rows - 1 : 1; /* one row of overlap */
            view->scroll += (ev->key == FB_KEY_PAGE_UP) ? -delta : delta;
            if (view->scroll < 0) view->scroll = 0;
            if (view->scroll > max_scroll) view->scroll = max_scroll;
            return 0;
        }

        if (app->input_mode == FB_INPUT_MODE_DIARY &&
            (ev->key == FB_KEY_ALT_LEFT || ev->key == FB_KEY_ALT_RIGHT)) {
            fb_diary_browse(&app->diary, ev->key == FB_KEY_ALT_LEFT ? -1 : 1);
            diary_rebuild_browse_view(app);
            if (app->diary.browse_index < 0) {
                snprintf(app->last_submitted, FB_CMDLINE_MAX, "diary: live");
            } else {
                snprintf(app->last_submitted, FB_CMDLINE_MAX,
                         "diary: viewing archived page %d of %d", app->diary.browse_index + 1,
                         app->diary.archived_count);
            }
            return 0;
        }

        /* Enter on an empty cmdline while browsing an archived page means
           "reopen this one", not "send an empty chat message" -- diary's
           normal submit path (below) would otherwise do the latter. Typing
           anything first and pressing Enter is still "send a new message",
           which already snaps back to live on its own. */
        if (app->input_mode == FB_INPUT_MODE_DIARY && app->diary.browse_index >= 0 &&
            ev->key == FB_KEY_ENTER && app->cmdline.len == 0) {
            app->diary_reopen_confirm_pending = 1;
            snprintf(app->last_submitted, FB_CMDLINE_MAX,
                     "this page was from a previous session -- reopen it into the live "
                     "conversation? (y/N)");
            return 0;
        }

        if (ev->key == FB_KEY_CTRL_SPACE && app->input_mode == FB_INPUT_MODE_MEETING) {
            fb_meeting_handle_event(&app->meeting, ev);
            return 0;
        }

        if (ev->key == FB_KEY_CTRL_SPACE && !app->mic_available) {
            snprintf(app->last_submitted, FB_CMDLINE_MAX, "%s",
                fb_mic_state() == FB_MIC_STARTING ? "microphone is opening; wait for it to become ready"
                                                : "microphone unavailable; check the default input device");
            return 0;
        }

        if (ev->key == FB_KEY_CTRL_SPACE && app->mic_available) {
            if (!app->recording) {
                /* Both the WAV and the transcription log live at fixed
                   paths (one recording at a time by design) -- starting a
                   new recording while the previous one is still being
                   transcribed would overwrite the very file sherpa-onnx is
                   reading mid-job. */
                if (app->transcribe_job.state == FB_TRANSCRIBE_RUNNING) {
                    snprintf(app->last_submitted, FB_CMDLINE_MAX,
                             "still transcribing the previous recording -- wait a moment");
                    return 0;
                }
                fb_mic_set_recording(1);
                app->recording = 1;
            } else {
                fb_mic_set_recording(0);
                app->recording = 0;

                size_t n = 0;
                const float *samples = fb_mic_recorded_samples(&n);
                int capped = n >= (size_t)FB_MIC_SAMPLE_RATE * FB_MIC_MAX_RECORD_SECONDS;
                char path[600];
                snprintf(path, sizeof(path), "%s/last_recording.wav", app->data_dir);
                if (fb_wav_write_file(path, samples, n, FB_MIC_SAMPLE_RATE) != 0) {
                    snprintf(app->last_submitted, FB_CMDLINE_MAX, "failed to save recording");
                } else if (!app->transcribe_available) {
                    snprintf(app->last_submitted, FB_CMDLINE_MAX,
                             "recorded %zu samples -> %s (set orukeet_dir in config.json to transcribe)",
                             n, path);
                } else if (fb_transcribe_start(&app->transcribe_job, &app->transcribe_cfg, path,
                                                app->data_dir) == 0) {
                    app->transcribe_confirm_pending = 0;
                    if (capped) {
                        snprintf(app->last_submitted, FB_CMDLINE_MAX,
                                 "recording capped at %ds -- transcribing... (attempt 1 of %d)",
                                 FB_MIC_MAX_RECORD_SECONDS, FB_TRANSCRIBE_MAX_ATTEMPTS);
                    } else {
                        snprintf(app->last_submitted, FB_CMDLINE_MAX,
                                 "transcribing... (attempt 1 of %d, ~6s for model load)",
                                 FB_TRANSCRIBE_MAX_ATTEMPTS);
                    }
                } else {
                    snprintf(app->last_submitted, FB_CMDLINE_MAX, "failed to start transcription");
                }
            }
            return 0;
        }

        if (ev->key == FB_KEY_TAB) {
            /* /load and /unload completion take priority in their own
               context; Tab only means "switch mode" everywhere else, so it
               never steals a keystroke someone's mid-completion with. */
            int is_load = strncmp(app->cmdline.buf, "/load ", 6) == 0;
            int is_unload = strncmp(app->cmdline.buf, "/unload ", 8) == 0;
            if (is_load || is_unload) {
                fb_command_autocomplete(app, app->cmdline.buf, &app->cmdline.len, FB_CMDLINE_MAX);
                app->cmdline.cursor = app->cmdline.len;
            } else {
                app->input_mode = (app->input_mode == FB_INPUT_MODE_COMMAND) ? FB_INPUT_MODE_SCRATCHPAD
                                   : (app->input_mode == FB_INPUT_MODE_SCRATCHPAD) ? FB_INPUT_MODE_DIARY
                                   : (app->input_mode == FB_INPUT_MODE_DIARY) ? FB_INPUT_MODE_MEETING
                                                                           : FB_INPUT_MODE_COMMAND;
                /* Scratchpad is meant to be a clean slate each time you
                   step into it, not a memory that quietly survives being
                   tabbed away from and back -- diary is the opposite by
                   design (that's the whole point of its memory pipeline),
                   so its chat_job is never reset here. */
                if (app->input_mode == FB_INPUT_MODE_SCRATCHPAD) fb_chat_reset(&app->chat_job);
                /* Don't land back in diary mid-browse from last time. */
                app->diary.browse_index = -1;
                const char *mode_desc = app->input_mode == FB_INPUT_MODE_SCRATCHPAD
                                             ? "scratchpad -- free text goes straight to the loaded model"
                                         : app->input_mode == FB_INPUT_MODE_DIARY
                                             ? "diary -- free text goes to the loaded model and is remembered"
                                         : app->input_mode == FB_INPUT_MODE_MEETING
                                             ? "meeting -- type notes; F8 for commands and setup"
                                             : "command -- free text goes through Qwen intent classify";
                snprintf(app->last_submitted, FB_CMDLINE_MAX, "mode: %s", mode_desc);
            }
            return 0;
        }

        if (app->input_mode == FB_INPUT_MODE_MEETING && app->view == FB_VIEW_DASHBOARD &&
            fb_meeting_handle_event(&app->meeting, ev)) {
            return 0;
        }

        FbCmdlineResult r = fb_cmdline_handle_event(&app->cmdline, ev);
        if (r == FB_CMDLINE_SUBMIT) {
            if (app->input_mode == FB_INPUT_MODE_MEETING && app->cmdline.buf[0] != '/') {
                snprintf(app->meeting.status, sizeof(app->meeting.status), "F8 switches to editable notes; commands start with /");
                fb_cmdline_clear(&app->cmdline);
                return 0;
            }
            if (strcmp(app->cmdline.buf, "/meeting") == 0 ||
                strncmp(app->cmdline.buf, "/meeting ", 9) == 0) {
                app->input_mode = FB_INPUT_MODE_MEETING;
                app->view = FB_VIEW_DASHBOARD;
                if (strncmp(app->cmdline.buf + 8, " process", 8) == 0 &&
                    app->meeting.llm_port == 0) {
                    FbBackendInstance *target = find_chat_target(app);
                    if (target && target->state == FB_BACKEND_UP) {
                        app->meeting.llm_port = target->port;
                        snprintf(app->meeting.llm_model, sizeof(app->meeting.llm_model), "%s", target->name);
                    }
                }
                const char *meeting_command = app->cmdline.buf + 8;
                meeting_command += strspn(meeting_command, " \t");
                size_t verb_length = strcspn(meeting_command, " \t");
                if (app->recording && verb_length == 4 && strncmp(meeting_command, "read", 4) == 0) {
                    snprintf(app->meeting.status, sizeof(app->meeting.status), "stop the microphone recording in its original mode before reading text");
                    fb_cmdline_clear(&app->cmdline);
                    return 0;
                }
                int changed = fb_meeting_command(&app->meeting, meeting_command);
                if (changed == 1 && fb_config_save(&app->config, app->data_dir) != 0) {
                    snprintf(app->meeting.status, sizeof(app->meeting.status), "setting applied but config.json could not be saved");
                }
                snprintf(app->last_submitted, FB_CMDLINE_MAX, "%s", app->meeting.status);
                fb_cmdline_clear(&app->cmdline);
                return 0;
            }
            FbCommand cmd = fb_command_parse(app->cmdline.buf);

            /* Free text (not even attempting "/"-syntax) with nowhere else
               to go gets routed by mode: BRAIN gets a shot at the intent
               classifier first (the "simplified non-command interaction
               level"); CHAT goes straight to whatever's loaded, as a real
               conversation turn, no command involved at all. Typos on a
               real "/" command always just report unknown regardless of
               mode -- neither path has any more idea what a garbled verb
               meant than the parser did. */
            if (cmd.kind == FB_CMD_UNKNOWN && app->cmdline.buf[0] != '/' &&
                app->input_mode == FB_INPUT_MODE_SCRATCHPAD) {
                FbBackendInstance *target =
                    resolve_chat_target(app, &app->chat_job, app->chat_target_name);
                if (!target) {
                    snprintf(app->last_submitted, FB_CMDLINE_MAX,
                             "no model loaded to chat with -- /load one first");
                } else if (app->chat_job.state != FB_CHAT_IDLE) {
                    snprintf(app->last_submitted, FB_CMDLINE_MAX,
                             "still waiting on a reply from '%s' -- hold on", app->chat_target_name);
                } else {
                    history_append_multiline(app, &app->scratchpad_history_view, "You",
                                              app->cmdline.buf);

                    strncpy(app->chat_target_name, target->name, sizeof(app->chat_target_name) - 1);
                    app->chat_target_name[sizeof(app->chat_target_name) - 1] = '\0';

                    char knowledge_buf[FB_CHAT_TURN_MAX];
                    const char *to_send = inject_knowledge_context(
                        app, app->cmdline.buf, app->cmdline.buf, knowledge_buf, sizeof(knowledge_buf));

                    if (fb_chat_start(&app->chat_job, target->port, to_send) == 0) {
                        snprintf(app->last_submitted, FB_CMDLINE_MAX, "chatting with '%s'...",
                                 target->name);
                    } else {
                        snprintf(app->last_submitted, FB_CMDLINE_MAX, "failed to start chat request");
                    }
                }
                fb_cmdline_clear(&app->cmdline);
                return 0;
            }

            if (cmd.kind == FB_CMD_UNKNOWN && app->cmdline.buf[0] != '/' &&
                app->input_mode == FB_INPUT_MODE_DIARY) {
                FbBackendInstance *target = resolve_chat_target(app, &app->diary_chat_job,
                                                                  app->diary_chat_target_name);
                if (!target) {
                    snprintf(app->last_submitted, FB_CMDLINE_MAX,
                             "no model loaded to chat with -- /load one first");
                } else if (app->diary_chat_job.state != FB_CHAT_IDLE) {
                    snprintf(app->last_submitted, FB_CMDLINE_MAX,
                             "still waiting on a reply from '%s' -- hold on",
                             app->diary_chat_target_name);
                } else {
                    /* Sending while browsing an old page would otherwise
                       silently write into diary_live_view while the screen
                       keeps showing diary_browse_view -- the reply lands
                       somewhere you're not looking, which just looks like
                       nothing happened. Sending a message is "back to now"
                       by definition, so snap back to live first. */
                    app->diary.browse_index = -1;

                    history_append_multiline(app, &app->diary_live_view, "You", app->cmdline.buf);
                    fb_diary_append_turn(&app->diary, "You", app->cmdline.buf, fb_now_ms());

                    strncpy(app->diary_chat_target_name, target->name,
                            sizeof(app->diary_chat_target_name) - 1);
                    app->diary_chat_target_name[sizeof(app->diary_chat_target_name) - 1] = '\0';

                    /* A fresh live conversation (first message since app
                       start or the last /load) has no idea what the diary
                       already knows -- prime it once with a trailing
                       excerpt of long-term memory. Later messages in the
                       same conversation carry that context forward via
                       chat_job's own turn history, so this only needs to
                       happen once. Excerpt, not the whole store: long-term
                       memory can be up to ~5000 chars, well past one
                       chat turn's budget (FB_CHAT_TURN_MAX) once the
                       user's own message and the framing text are added. */
                    char primed[FB_CHAT_TURN_MAX];
                    const char *to_send = app->cmdline.buf;
                    if (app->diary_chat_job.turn_count == 0 && app->diary.long_term_len > 0) {
                        const char *ltm = app->diary.long_term;
                        const size_t ltm_excerpt_max = 1200;
                        if (app->diary.long_term_len > ltm_excerpt_max) {
                            ltm += app->diary.long_term_len - ltm_excerpt_max;
                        }
                        snprintf(primed, sizeof(primed),
                                 "(Context from long-term memory: %s)\n\n%s", ltm,
                                 app->cmdline.buf);
                        to_send = primed;
                    }

                    /* Knowledge retrieval runs every turn, not just the
                       first -- see inject_knowledge_context's comment for
                       why that differs from the long-term-memory priming
                       just above. Searches on the raw typed text
                       (app->cmdline.buf), not `to_send` -- which may
                       already carry the long-term-memory prefix by this
                       point -- so the embedding query stays clean. */
                    char knowledge_buf[FB_CHAT_TURN_MAX];
                    to_send = inject_knowledge_context(app, app->cmdline.buf, to_send, knowledge_buf,
                                                        sizeof(knowledge_buf));

                    if (fb_chat_start(&app->diary_chat_job, target->port, to_send) == 0) {
                        snprintf(app->last_submitted, FB_CMDLINE_MAX, "diary: chatting with '%s'...",
                                 target->name);
                    } else {
                        snprintf(app->last_submitted, FB_CMDLINE_MAX,
                                 "failed to start diary chat request");
                    }
                }
                fb_cmdline_clear(&app->cmdline);
                return 0;
            }

            if (cmd.kind == FB_CMD_UNKNOWN && app->cmdline.buf[0] != '/') {
                FbBackendInstance *brain = fb_backend_mgr_find(&app->backend_mgr, FB_QWEN_BRAIN_NAME);
                if (brain && brain->state == FB_BACKEND_UP) {
                    history_append_multiline(app, &app->history_view, "You", app->cmdline.buf);

                    FbCommand brain_cmd;
                    char raw[256];
                    if (fb_intent_brain_classify(brain->port, app->cmdline.buf, &brain_cmd, raw,
                                                  sizeof(raw)) == 0) {
                        cmd = brain_cmd;
                    }

                    history_append_multiline(app, &app->history_view, "Qwen",
                                              raw[0] != '\0' ? raw : "(no response)");
                    history_append(app, &app->history_view, "");
                }
            }

            if (cmd.kind == FB_CMD_ADD) app->file_picker_pending_load = 0;
            /* A new /load can change which model chat mode targets --
               reset rather than risk handing a different model a
               conversation it was never actually part of. Simplest safe
               choice even though a failed /load attempt (nothing actually
               changed) also resets it. */
            if (cmd.kind == FB_CMD_LOAD) {
                fb_chat_reset(&app->chat_job);
                fb_chat_reset(&app->diary_chat_job);
            }
            fb_command_execute(app, &cmd);
            fb_cmdline_clear(&app->cmdline);
        }
    }

    return 0;
}

void fb_app_render(FbApp *app) {
    FbLayout layout;
    fb_layout_compute(app->width, app->height, &layout);
    if (app->input_mode == FB_INPUT_MODE_MEETING && app->view == FB_VIEW_DASHBOARD &&
        !app->file_picker.active) {
        fb_meeting_render(&app->meeting, layout, &app->cmdline);
        return;
    }

    /* The active mode's color, used throughout this function for anything
       that's "part of the current mode's chrome" (title, F-key legend,
       mode tag, cmdline prompt/cursor) -- computed once up front so it's
       available for the header too, not just the bottom of the screen. */
    uint8_t mode_r = app->input_mode == FB_INPUT_MODE_SCRATCHPAD ? FB_COLOR_WARM_R
                     : app->input_mode == FB_INPUT_MODE_DIARY    ? FB_COLOR_DIARY_R
                                                                  : FB_COLOR_ACCENT_R;
    uint8_t mode_g = app->input_mode == FB_INPUT_MODE_SCRATCHPAD ? FB_COLOR_WARM_G
                     : app->input_mode == FB_INPUT_MODE_DIARY    ? FB_COLOR_DIARY_G
                                                                  : FB_COLOR_ACCENT_G;
    uint8_t mode_b = app->input_mode == FB_INPUT_MODE_SCRATCHPAD ? FB_COLOR_WARM_B
                     : app->input_mode == FB_INPUT_MODE_DIARY    ? FB_COLOR_DIARY_B
                                                                  : FB_COLOR_ACCENT_B;

    fb_fill_rect(layout.header, FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);
    char title[128];
    snprintf(title, sizeof(title), " %s v%s", FB_APP_NAME, FB_VERSION_STRING);
    fb_draw_text(layout.header.x, layout.header.y, title, mode_r, mode_g, mode_b, FB_COLOR_BG_R,
                 FB_COLOR_BG_G, FB_COLOR_BG_B);

    const char *mic_status = fb_mic_state() == FB_MIC_STARTING ? "[mic opening]"
                              : !app->mic_available ? "[mic unavailable]"
                              : app->recording    ? "[REC - ctrl+space to stop]"
                                                   : "[ctrl+space to record]";
    int mic_status_x = layout.header.w - (int)strlen(mic_status) - 1;
    if (mic_status_x > (int)strlen(title)) {
        if (!app->mic_available) {
            fb_draw_text(mic_status_x, layout.header.y, mic_status, FB_COLOR_DIM_R,
                         FB_COLOR_DIM_G, FB_COLOR_DIM_B, FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);
        } else if (app->recording) {
            fb_draw_text(mic_status_x, layout.header.y, mic_status, FB_COLOR_HOT_R,
                         FB_COLOR_HOT_G, FB_COLOR_HOT_B, FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);
        } else {
            fb_draw_text(mic_status_x, layout.header.y, mic_status, FB_COLOR_DIM_R,
                         FB_COLOR_DIM_G, FB_COLOR_DIM_B, FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);
        }
    }

    fb_fill_rect(layout.main_area, FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);

    unsigned long long elapsed_ms = fb_now_ms() - app->start_ms;

    if (app->mic_available) {
        float levels[64];
        size_t n = fb_mic_poll_levels(levels, 64);
        for (size_t i = 0; i < n; ++i) fb_app_push_wave_sample(app, levels[i]);
    } else {
        fb_app_push_wave_sample(app, 0.0f);
    }
    app->sphere_intensity = app->recording ? 1.0f : 0.0f;

    if (app->view == FB_VIEW_LEADERBOARD) {
        fb_textview_render(&app->leaderboard_view, layout.main_area);
    } else if (app->view == FB_VIEW_LOGS) {
        fb_textview_render(&app->log_view, layout.main_area);
    } else {
        /* Dashboard only: reserve the right side for the You/Qwen exchange
           log, so a conversation with the brain stays visible instead of
           only flashing through the status bar. Too narrow a terminal just
           drops the split rather than squeezing both to nothing. */
        FbRect history_area = compute_history_area(app);
        FbRect left_area = {layout.main_area.x, layout.main_area.y,
                             layout.main_area.w - history_area.w, layout.main_area.h};
        if (history_area.w > 0) fb_textview_render(current_panel_view(app), history_area);

        if (app->file_picker.active) {
            fb_file_picker_render(&app->file_picker, left_area);
        } else {
            int backend_row_h = 1;
            FbRect backend_row = {left_area.x, left_area.y, left_area.w, backend_row_h};
            if (app->backend_mgr.count == 0) {
                fb_draw_text(backend_row.x, backend_row.y, " no models loaded (see /help)",
                             FB_COLOR_DIM_R, FB_COLOR_DIM_G, FB_COLOR_DIM_B, FB_COLOR_BG_R,
                             FB_COLOR_BG_G, FB_COLOR_BG_B);
            } else {
                char line[256];
                size_t off = 0;
                for (int i = 0; i < app->backend_mgr.count && off < sizeof(line) - 1; ++i) {
                    const FbBackendInstance *b = &app->backend_mgr.items[i];
                    const char *state_str = b->state == FB_BACKEND_UP         ? "UP"
                                             : b->state == FB_BACKEND_STARTING ? "STARTING"
                                             : b->state == FB_BACKEND_ERROR    ? "ERROR"
                                                                               : "STOPPED";
                    int n = snprintf(line + off, sizeof(line) - off, " [%s:%d %s]", b->name,
                                      b->port, state_str);
                    if (n > 0) off += (size_t)n;
                    if (b->state == FB_BACKEND_ERROR && off < sizeof(line) - 1) {
                        n = snprintf(line + off, sizeof(line) - off, " log: %s", b->log_path);
                        if (n > 0) off += (size_t)n;
                    }
                }
                uint8_t r = mode_r, g = mode_g, bcol = mode_b;
                for (int i = 0; i < app->backend_mgr.count; ++i) {
                    if (app->backend_mgr.items[i].state == FB_BACKEND_ERROR) {
                        r = FB_COLOR_HOT_R; g = FB_COLOR_HOT_G; bcol = FB_COLOR_HOT_B;
                        break;
                    }
                }
                fb_draw_text(backend_row.x, backend_row.y, line, r, g, bcol, FB_COLOR_BG_R,
                             FB_COLOR_BG_G, FB_COLOR_BG_B);
            }

            int wave_h = 3;
            FbRect wave_area = {left_area.x, left_area.y + left_area.h - wave_h, left_area.w,
                                 wave_h};
            FbRect sphere_area = {left_area.x, left_area.y + backend_row_h, left_area.w,
                                   left_area.h - wave_h - backend_row_h - 1};

            /* The centerpiece effect is mode-specific: command keeps the
               original sphere, scratchpad gets the pencil, diary gets the
               hypercube. Project (tetris) is built and ready in
               fx_tetris.c but has no mode to attach to yet -- see
               FbInputMode's comment in app.h. */
            if (app->input_mode == FB_INPUT_MODE_SCRATCHPAD) {
                fb_fx_pencil_render(sphere_area, elapsed_ms, mode_r, mode_g, mode_b);
            } else if (app->input_mode == FB_INPUT_MODE_DIARY) {
                fb_fx_hypercube_render(sphere_area, elapsed_ms, mode_r, mode_g, mode_b);
            } else {
                fb_fx_sphere_render(sphere_area, elapsed_ms, app->sphere_intensity);
            }
            fb_fx_waveform_render(wave_area, app->wave_samples, FB_WAVE_HISTORY);
        }
    }

    fb_fill_rect(layout.statusbar, FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);
    fb_draw_hline(layout.statusbar.x, layout.statusbar.y, layout.statusbar.w,
                  ' ', FB_COLOR_DIM_R, FB_COLOR_DIM_G, FB_COLOR_DIM_B);
    if (app->last_submitted[0] != '\0') {
        char status[FB_CMDLINE_MAX + 16];
        snprintf(status, sizeof(status), " last: %s", app->last_submitted);
        fb_draw_text(layout.statusbar.x, layout.statusbar.y, status,
                     FB_COLOR_DIM_R, FB_COLOR_DIM_G, FB_COLOR_DIM_B,
                     FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);
    }

    fb_fill_rect(layout.hints, FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);
    int fkeys_end_x;
    {
        static const struct { const char *key; const char *label; } fkeys[] = {
            {"F1", "Help"}, {"F2", "Load"}, {"F3", "Unload"}, {"F4", "Add"}, {"F5", "Remove"},
            {"F6", "List"}, {"F7", "New pg"},
            {"F10", "Quit"},
        };
        int x = layout.hints.x + 1;
        for (size_t i = 0; i < sizeof(fkeys) / sizeof(fkeys[0]); ++i) {
            /* /new_page is a scratchpad/diary concept (and project mode's,
               later) -- command mode has no notion of a "page" at all, so
               don't advertise a shortcut that would just tell you so. */
            if (strcmp(fkeys[i].key, "F7") == 0 && app->input_mode == FB_INPUT_MODE_COMMAND) {
                continue;
            }
            fb_draw_text(x, layout.hints.y, fkeys[i].key, mode_r, mode_g, mode_b, FB_COLOR_BG_R,
                         FB_COLOR_BG_G, FB_COLOR_BG_B);
            x += (int)strlen(fkeys[i].key);
            char label[32];
            snprintf(label, sizeof(label), " %s  ", fkeys[i].label);
            fb_draw_text(x, layout.hints.y, label, FB_COLOR_DIM_R, FB_COLOR_DIM_G, FB_COLOR_DIM_B,
                         FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);
            x += (int)strlen(label);
        }
        fkeys_end_x = x;
    }

    /* Two rows, right-aligned, same color the cmdline's prompt/cursor use
       below so the two reinforce each other: row 1 is "what mode, what
       navigation keys work right now" (short, stable -- never grows past a
       screen edge case the way a stats line packed with numbers can, which
       is exactly what was silently running off-screen and disappearing
       before this split); row 2 is the mode's live stats -- a spinner while
       a request is in flight (there's no visibility into a "reasoning"
       sub-phase specifically without streaming, so this covers the whole
       wait, not just reasoning), then the last request's token/context/
       speed stats once one has completed. */
    char top_line[80];
    char bottom_line[200];
    bottom_line[0] = '\0';

    if (app->input_mode == FB_INPUT_MODE_SCRATCHPAD) {
        snprintf(top_line, sizeof(top_line), "[Tab] SCRATCHPAD");
        FbBackendInstance *target = resolve_chat_target(app, &app->chat_job, app->chat_target_name);
        const char *target_name = target ? target->name : "(none loaded)";
        if (app->chat_job.state == FB_CHAT_RUNNING) {
            static const char spinner_chars[] = "|/-\\";
            char spin = spinner_chars[(elapsed_ms / 150) % 4];
            snprintf(bottom_line, sizeof(bottom_line), "%s  %c generating...", target_name, spin);
        } else if (app->chat_job.total_tokens > 0) {
            int ctx_shown = app->chat_job.n_ctx > 0 ? app->chat_job.n_ctx : app->chat_job.total_tokens;
            snprintf(bottom_line, sizeof(bottom_line), "%s  %d/%d ctx  %.0f/%.0f t/s (pre/dec)",
                     target_name, app->chat_job.prompt_tokens, ctx_shown,
                     app->chat_job.prompt_tok_per_sec, app->chat_job.predicted_tok_per_sec);
        } else {
            snprintf(bottom_line, sizeof(bottom_line), "%s", target_name);
        }
    } else if (app->input_mode == FB_INPUT_MODE_DIARY) {
        FbBackendInstance *target =
            resolve_chat_target(app, &app->diary_chat_job, app->diary_chat_target_name);
        const char *target_name = target ? target->name : "(none loaded)";
        static const char spinner_chars[] = "|/-\\";
        char spin = spinner_chars[(elapsed_ms / 150) % 4];

        if (app->diary.browse_index >= 0) {
            snprintf(top_line, sizeof(top_line), "[Tab] DIARY  (Alt+Left/Right browse, Enter reopen)");
        } else {
            snprintf(top_line, sizeof(top_line), "[Tab] DIARY  (Alt+Left/Right: browse pages)");
        }

        /* Which numbered page is actually being written right now, always
           shown regardless of state -- previously only visible while
           browsing or editing an old one, never for the ordinary case. */
        int current_page_no =
            app->diary.edit_target >= 0 ? app->diary.edit_target + 1 : app->diary.archived_count + 1;

        if (app->diary_chat_job.state == FB_CHAT_RUNNING) {
            snprintf(bottom_line, sizeof(bottom_line), "%s  page #%d  %c generating...", target_name,
                     current_page_no, spin);
        } else if (fb_diary_busy(&app->diary)) {
            const char *task =
                app->diary.job_task == FB_DIARY_TASK_CHECKPOINT ? "checkpointing" : "sleeping";
            snprintf(bottom_line, sizeof(bottom_line), "%s  page #%d  %c %s...", target_name,
                     current_page_no, spin, task);
        } else if (app->diary.browse_index >= 0) {
            snprintf(bottom_line, sizeof(bottom_line), "viewing archived page %d of %d",
                     app->diary.browse_index + 1, app->diary.archived_count);
        } else if (app->diary.edit_target >= 0) {
            snprintf(bottom_line, sizeof(bottom_line),
                     "%s  editing page #%d  %zu/%zu chars (F7: new page)", target_name,
                     current_page_no, app->diary.page_len, app->diary.page_close_chars);
        } else {
            /* Persistent reassurance, not just the one-shot "autosaved"
               flash in last_submitted -- that flash scrolls away the
               moment you do anything else, this stays put. */
            char saved_suffix[32] = "";
            if (app->diary.last_saved_ms > 0) {
                unsigned long long since_ms = fb_now_ms() - app->diary.last_saved_ms;
                snprintf(saved_suffix, sizeof(saved_suffix), "  saved %llus ago", since_ms / 1000);
            }
            /* Distinct numbers, explicitly labeled since several counts in
               characters side by side was genuinely confusing unlabeled:
               - page #N: which page this even is.
               - N/M chars: that page's own progress toward its own
                 autosave/close trigger (page_close_chars).
               - archived: how many pages have already been closed.
               - memory: the long-term (consolidated) memory's total size,
                 a completely different, already-summarized pool. */
            snprintf(bottom_line, sizeof(bottom_line),
                     "%s  page #%d: %zu/%zu chars  archived: %d pages  memory: %zu chars%s",
                     target_name, current_page_no, app->diary.page_len, app->diary.page_close_chars,
                     app->diary.archived_count, app->diary.long_term_len, saved_suffix);
        }
    } else {
        snprintf(top_line, sizeof(top_line), "[Tab] COMMAND");
    }

    int top_y = layout.hints.y;
    int bottom_y = layout.hints.y + 1;

    int top_x = layout.hints.x + layout.hints.w - (int)strlen(top_line) - 1;
    if (top_x > fkeys_end_x) {
        fb_draw_text(top_x, top_y, top_line, mode_r, mode_g, mode_b, FB_COLOR_BG_R, FB_COLOR_BG_G,
                     FB_COLOR_BG_B);
    }

    /* PageUp/PageDown scroll the panel in every mode -- drawn just left of
       the mode tag on the same top row, dim so it doesn't compete with it. */
    const char *scroll_hint = "PgUp/PgDn scroll";
    int hint_x = top_x - (int)strlen(scroll_hint) - 2;
    if (hint_x > fkeys_end_x) {
        fb_draw_text(hint_x, top_y, scroll_hint, FB_COLOR_DIM_R, FB_COLOR_DIM_G, FB_COLOR_DIM_B,
                     FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);
    }

    if (bottom_line[0] != '\0') {
        int bottom_x = layout.hints.x + layout.hints.w - (int)strlen(bottom_line) - 1;
        if (bottom_x < layout.hints.x) bottom_x = layout.hints.x;
        fb_draw_text(bottom_x, bottom_y, bottom_line, mode_r, mode_g, mode_b, FB_COLOR_BG_R,
                     FB_COLOR_BG_G, FB_COLOR_BG_B);
    }

    fb_cmdline_render(&app->cmdline, layout.cmdline, mode_r, mode_g, mode_b);
}
