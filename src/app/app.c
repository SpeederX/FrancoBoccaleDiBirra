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
#include "tui/fx_sphere.h"
#include "tui/fx_waveform.h"
#include "tui/layout.h"
#include "tui/palette.h"
#include "util/paths.h"

void fb_app_init(FbApp *app, int width, int height) {
    memset(app, 0, sizeof(*app));
    app->running = 1;
    app->width = width;
    app->height = height;
    app->start_ms = fb_now_ms();
    fb_cmdline_init(&app->cmdline);

    app->mic_available = (fb_mic_init() == 0);
    if (fb_get_data_dir(app->data_dir, sizeof(app->data_dir)) != 0) {
        app->data_dir[0] = '\0';
    }

    fb_backend_mgr_init(&app->backend_mgr);
    fb_config_load(&app->config, app->data_dir);
    fb_resolve_llama_server(app->llama_server_path, sizeof(app->llama_server_path));
    app->transcribe_available = (fb_resolve_transcribe_config(&app->transcribe_cfg) == 0);

    if (fb_calibr_root(app->calibr_root, sizeof(app->calibr_root)) != 0) {
        app->calibr_root[0] = '\0';
    }
    fb_calibr_log_tail_init(&app->log_tail);

    char qwen_model[FB_BACKEND_PATH_MAX];
    app->qwen_available = (fb_resolve_qwen_model(qwen_model, sizeof(qwen_model)) == 0);
    if (app->qwen_available) {
        const char *extra_args[] = {"--reasoning", "off", NULL};
        fb_backend_mgr_load(&app->backend_mgr, FB_QWEN_BRAIN_NAME, qwen_model,
                             app->llama_server_path, app->data_dir, fb_resolve_qwen_port(),
                             extra_args);
    }
}

void fb_app_tick(FbApp *app) {
    fb_backend_mgr_tick(&app->backend_mgr);

    if (app->view == FB_VIEW_LOGS) {
        fb_calibr_log_tail_poll(&app->log_tail, app->calibr_root, &app->log_view);
    }

    if (app->transcribe_job.state == FB_TRANSCRIBE_RUNNING) {
        int attempt_before = app->transcribe_job.attempt;
        fb_transcribe_tick(&app->transcribe_job);

        if (app->transcribe_job.state == FB_TRANSCRIBE_DONE) {
            snprintf(app->cmdline.buf, FB_CMDLINE_MAX, "%s", app->transcribe_job.result_text);
            app->cmdline.len = strlen(app->cmdline.buf);
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
    fb_backend_mgr_shutdown(&app->backend_mgr);
    if (app->mic_available) fb_mic_shutdown();
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
        if (ev->key == FB_KEY_CTRL_C) {
            app->running = 0;
            return 1;
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

        if (app->view != FB_VIEW_DASHBOARD) {
            if (ev->key == FB_KEY_ESC) {
                app->view = FB_VIEW_DASHBOARD;
                return 0;
            }
            if (ev->key == FB_KEY_ARROW_UP || ev->key == FB_KEY_ARROW_DOWN) {
                int visible_rows = app->height - 3; /* header + statusbar + cmdline */
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

                snprintf(app->cmdline.buf, FB_CMDLINE_MAX, "/add %s %s", name, path);
                app->cmdline.len = strlen(app->cmdline.buf);
                snprintf(app->last_submitted, FB_CMDLINE_MAX,
                         "picked %s -- press enter to add as '%s'", path, name);
            } else if (r == FB_PICKER_CANCELLED) {
                snprintf(app->last_submitted, FB_CMDLINE_MAX, "file picker cancelled");
            }
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
                             "recorded %zu samples -> %s (set FB_ORUKEET_DIR to transcribe)", n,
                             path);
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
            fb_command_autocomplete(app, app->cmdline.buf, &app->cmdline.len, FB_CMDLINE_MAX);
            return 0;
        }

        FbCmdlineResult r = fb_cmdline_handle_event(&app->cmdline, ev);
        if (r == FB_CMDLINE_SUBMIT) {
            FbCommand cmd = fb_command_parse(app->cmdline.buf);

            /* Free text (not even attempting "/"-syntax) with nowhere else
               to go gets a shot at the intent brain before we give up on
               it -- this is the "simplified non-command interaction
               level". Typos on a real "/" command still just report
               unknown, since the brain has no more idea what a garbled
               verb meant than the parser did. */
            if (cmd.kind == FB_CMD_UNKNOWN && app->cmdline.buf[0] != '/') {
                FbBackendInstance *brain = fb_backend_mgr_find(&app->backend_mgr, FB_QWEN_BRAIN_NAME);
                if (brain && brain->state == FB_BACKEND_UP) {
                    FbCommand brain_cmd;
                    if (fb_intent_brain_classify(brain->port, app->cmdline.buf, &brain_cmd) == 0) {
                        cmd = brain_cmd;
                    }
                }
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

    fb_fill_rect(layout.header, FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);
    char title[128];
    snprintf(title, sizeof(title), " %s v%s", FB_APP_NAME, FB_VERSION_STRING);
    fb_draw_text(layout.header.x, layout.header.y, title, FB_COLOR_ACCENT_R,
                 FB_COLOR_ACCENT_G, FB_COLOR_ACCENT_B, FB_COLOR_BG_R,
                 FB_COLOR_BG_G, FB_COLOR_BG_B);

    const char *mic_status = !app->mic_available ? "[mic unavailable]"
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
    } else if (app->file_picker.active) {
        fb_file_picker_render(&app->file_picker, layout.main_area);
    } else {
        int backend_row_h = 1;
        FbRect backend_row = {layout.main_area.x, layout.main_area.y, layout.main_area.w,
                               backend_row_h};
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
                int n = snprintf(line + off, sizeof(line) - off, " [%s:%d %s]", b->name, b->port,
                                  state_str);
                if (n > 0) off += (size_t)n;
                if (b->state == FB_BACKEND_ERROR && off < sizeof(line) - 1) {
                    n = snprintf(line + off, sizeof(line) - off, " log: %s", b->log_path);
                    if (n > 0) off += (size_t)n;
                }
            }
            uint8_t r = FB_COLOR_ACCENT_R, g = FB_COLOR_ACCENT_G, bcol = FB_COLOR_ACCENT_B;
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
        FbRect wave_area = {layout.main_area.x, layout.main_area.y + layout.main_area.h - wave_h,
                             layout.main_area.w, wave_h};
        FbRect sphere_area = {layout.main_area.x, layout.main_area.y + backend_row_h,
                               layout.main_area.w,
                               layout.main_area.h - wave_h - backend_row_h - 1};

        fb_fx_sphere_render(sphere_area, elapsed_ms, app->sphere_intensity);
        fb_fx_waveform_render(wave_area, app->wave_samples, FB_WAVE_HISTORY);
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

    fb_cmdline_render(&app->cmdline, layout.cmdline);
}
