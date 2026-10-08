#include "meeting/meeting.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "meeting/meeting_document.h"
#include "platform/dirlist.h"
#include "platform/platform.h"
#include "tui/palette.h"
#include "util/fileutil.h"
#include "util/paths.h"
#include "util/utf8.h"

static void refresh_tools(FbMeeting *meeting) {
    FbConfig *cfg = meeting->config;
    snprintf(meeting->tools.parakeet_exe, 512, "%s", cfg->meeting_parakeet_exe);
    snprintf(meeting->tools.asr_model, 512, "%s", cfg->meeting_asr_model);
    snprintf(meeting->tools.diar_model, 512, "%s", cfg->meeting_diar_model);
    snprintf(meeting->tools.ffmpeg_exe, 512, "%s", cfg->meeting_ffmpeg_exe[0]
                 ? cfg->meeting_ffmpeg_exe : "ffmpeg");
}

void fb_meeting_init(FbMeeting *meeting, FbConfig *config, const char *data_dir) {
    memset(meeting, 0, sizeof(*meeting));
    meeting->config = config;
    if (data_dir[0]) {
        snprintf(meeting->root, sizeof(meeting->root), "%s/meetings", data_dir);
    }
    fb_notepad_init(&meeting->notes);
    fb_textview_clear(&meeting->output);
    meeting->llm_port = config->meeting_llm_port;
    snprintf(meeting->llm_model, sizeof(meeting->llm_model), "%s", config->meeting_llm_model);
    refresh_tools(meeting);
    snprintf(meeting->status, sizeof(meeting->status), "F8 commands, /meeting new to begin; /meeting help for setup");
}

static void reflow_output(FbMeeting *meeting) {
    const char *text = meeting->output_text;
    fb_textview_clear(&meeting->output);
    if (!text) {
        return;
    }
    while (*text) {
        char line[FB_TEXTVIEW_LINE_MAX];
        size_t count = 0;
        size_t last_space = 0;
        int columns = 0;
        int width = meeting->output_width > 0 ? meeting->output_width : 80;
        while (text[count] && text[count] != '\n' && count < sizeof(line) - 5 && columns < width) {
            uint32_t cp;
            size_t bytes = fb_utf8_decode(text + count, &cp);
            if (cp == ' ') {
                last_space = count;
            }
            count += bytes;
            columns++;
        }
        int wrapped = text[count] && text[count] != '\n';
        if (wrapped && last_space > 0) {
            count = last_space;
        }
        memcpy(line, text, count);
        line[count] = '\0';
        fb_textview_add_line(&meeting->output, line);
        text += count;
        if (*text == '\n') {
            text++;
        } else if (wrapped && *text == ' ') {
            text++;
        }
    }
}

static void view_text(FbMeeting *meeting, const char *text) {
    size_t length = text ? strlen(text) : 0;
    char *copy = malloc(length + 1);
    if (!copy) {
        snprintf(meeting->status, sizeof(meeting->status), "could not allocate the output view");
        return;
    }
    memcpy(copy, text ? text : "", length + 1);
    free(meeting->output_text);
    meeting->output_text = copy;
    reflow_output(meeting);
}

static void refresh_output(FbMeeting *meeting) {
    if (meeting->output_kind == 2) {
        cJSON *current = meeting->document ? cJSON_GetObjectItemCaseSensitive(meeting->document, "report_current") : NULL;
        if (meeting->report && cJSON_IsFalse(current)) {
            const char *notice = "Previous report retained; process the current transcript and notes to update it.\n\n";
            size_t length = strlen(notice) + strlen(meeting->report) + 1;
            char *text = malloc(length);
            if (text) {
                snprintf(text, length, "%s%s", notice, meeting->report);
                view_text(meeting, text);
                free(text);
            }
        } else {
            view_text(meeting, meeting->report ? meeting->report : "No report yet. Use /meeting process after transcription.");
        }
    } else if (meeting->document) {
        char *text = fb_meeting_document_text(meeting->document, meeting->output_kind == 1);
        view_text(meeting, text);
        free(text);
    } else {
        view_text(meeting, "No transcript yet. Ctrl+Space records system audio.\nStop to transcribe, or use /meeting import <audio/video path>.\nAudio, notes and outputs are kept in the meeting folder.");
    }
}

static void help(FbMeeting *meeting) {
    view_text(meeting,
        "MEETING\n"
        "Type in Notes; Enter creates a new line. F8 switches to commands.\n"
        "Ctrl+Space starts/stops recording the default system output.\n"
        "F9 cycles original transcript, cleaned transcript, and report.\n"
        "Report uses notes-used.txt; notes edited afterwards need /meeting process again.\n"
        "PgUp/PgDn scroll output. Tab changes mode; recording continues.\n\n"
        "/meeting new                 create a retained local meeting\n"
        "/meeting list                show meeting folders\n"
        "/meeting open <folder path>  reopen saved notes and transcript\n"
        "/meeting setup parakeet <executable path>\n"
        "/meeting setup asr <multilingual GGUF path>\n"
        "/meeting setup diar <diarization GGUF or bundle path>\n"
        "/meeting setup ffmpeg <executable path>\n"
        "/meeting model <port> <model-id> select localhost LLM\n"
        "/meeting record              start system-output recording\n"
        "/meeting stop                stop, then transcribe\n"
        "/meeting import <file path>  import audio/video into a fresh meeting\n"
        "/meeting transcribe          retry ASR on retained audio.wav\n"
        "/meeting process             clean transcript and create report\n"
        "/meeting name <number> <name> anchor a speaker; manual names take priority\n"
        "/meeting save                save current notes\n"
        "/meeting cancel              cancel processing; retain audio/notes\n\n"
        "Paths may contain spaces; quotes around the entire path are optional.\n"
        "System output excludes your microphone unless played through the output.\n"
        "Diarization supports up to eight voices; unknown speech remains unattributed.\n"
        "Automatic introduction names are inferred and can be corrected.\n"
        "All LLM requests go to localhost. Notes used by a report are snapshotted.\n"
        "The UI shows up to 2000 output lines; complete exports remain on disk.");
}

static int start_save(FbMeeting *meeting) {
    if (meeting->save_job || !meeting->directory[0]) {
        return -1;
    }
    meeting->save_job = fb_meeting_job_start(FB_MEETING_JOB_SAVE, NULL, meeting->directory,
                       NULL, meeting->notes.text, meeting->document_dirty ? meeting->document : NULL, 0, NULL);
    if (!meeting->save_job) {
        snprintf(meeting->status, sizeof(meeting->status), "could not start saving notes");
        return -1;
    }
    meeting->saving_revision = meeting->notes.revision;
    meeting->saving_document = meeting->document_dirty;
    meeting->save_failed = 0;
    meeting->document_dirty = 0;
    return 0;
}

static int new_meeting(FbMeeting *meeting) {
    if (!meeting->root[0]) {
        snprintf(meeting->status, sizeof(meeting->status), "the app's local data folder is unavailable");
        return -1;
    }
    if (meeting->capture || meeting->job || meeting->save_job ||
        meeting->saved_revision != meeting->notes.revision || meeting->document_dirty) {
        snprintf(meeting->status, sizeof(meeting->status), "save notes and finish/stop current jobs before changing meeting");
        return -1;
    }
    if (fb_ensure_dir(meeting->root) != 0) {
        snprintf(meeting->status, sizeof(meeting->status), "could not create the local meetings folder");
        return -1;
    }
    time_t now = time(NULL);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&now));
    snprintf(meeting->directory, sizeof(meeting->directory), "%s/%s-%llu", meeting->root, stamp, fb_now_ms());
    if (fb_ensure_dir(meeting->directory) != 0) {
        meeting->directory[0] = '\0';
        snprintf(meeting->status, sizeof(meeting->status), "could not create a meeting folder");
        return -1;
    }
    fb_notepad_set(&meeting->notes, "");
    meeting->saved_revision = meeting->notes.revision - 1;
    cJSON_Delete(meeting->document);
    meeting->document = NULL;
    free(meeting->report);
    meeting->report = NULL;
    meeting->output_kind = 0;
    meeting->command_focus = 0;
    refresh_output(meeting);
    start_save(meeting);
    snprintf(meeting->status, sizeof(meeting->status), "new local meeting; type notes or Ctrl+Space to record system output");
    return 0;
}

static int start_job(FbMeeting *meeting, FbMeetingOperation operation, const char *source) {
    if (meeting->capture || meeting->job || !meeting->directory[0]) {
        snprintf(meeting->status, sizeof(meeting->status), "start a meeting first and finish the current recording/job");
        return -1;
    }
    if (operation == FB_MEETING_JOB_TRANSCRIBE &&
        (!meeting->tools.parakeet_exe[0] || !meeting->tools.asr_model[0] || !meeting->tools.diar_model[0])) {
        snprintf(meeting->status, sizeof(meeting->status), "configure Parakeet, ASR and diarization paths: /meeting help");
        return -1;
    }
    if (operation == FB_MEETING_JOB_PROCESS &&
        (!meeting->document || meeting->llm_port < 1 || !meeting->llm_model[0])) {
        snprintf(meeting->status, sizeof(meeting->status), "transcribe first, then select an LLM with /load or /meeting model");
        return -1;
    }
    if (meeting->save_job && (meeting->document_dirty || meeting->saving_document)) {
        snprintf(meeting->status, sizeof(meeting->status), "speaker names are being saved; retry when saving finishes");
        return -1;
    }
    meeting->job = fb_meeting_job_start(operation, &meeting->tools, meeting->directory, source,
                       meeting->notes.text, meeting->document, meeting->llm_port, meeting->llm_model);
    meeting->operation = operation;
    if (!meeting->job) {
        snprintf(meeting->status, sizeof(meeting->status), "could not start the meeting job");
        return -1;
    }
    snprintf(meeting->status, sizeof(meeting->status), "meeting job started; notes remain editable");
    return 0;
}

static const char *skip_spaces(const char *text) {
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    return text;
}

int fb_meeting_command(FbMeeting *meeting, const char *command) {
    const char *arguments = skip_spaces(command);
    size_t length = strcspn(arguments, " \t");
    char verb[32];
    if (length >= sizeof(verb)) {
        snprintf(meeting->status, sizeof(meeting->status), "unknown meeting command; use /meeting help");
        return -1;
    }
    memcpy(verb, arguments, length);
    verb[length] = '\0';
    arguments = skip_spaces(arguments + length);
    char path[600];
    snprintf(path, sizeof(path), "%s", arguments);
    size_t path_length = strlen(path);
    if (path_length >= 2 && path[0] == '"' && path[path_length - 1] == '"') {
        memmove(path, path + 1, path_length - 2);
        path[path_length - 2] = '\0';
    }
    if (!verb[0] || strcmp(verb, "help") == 0) {
        help(meeting);
        meeting->command_focus = 1;
        return 0;
    }
    if (strcmp(verb, "new") == 0 && !*arguments) {
        return new_meeting(meeting);
    }
    if (strcmp(verb, "setup") == 0) {
        size_t key_length = strcspn(arguments, " \t");
        char key[32];
        if (key_length >= sizeof(key)) {
            return -1;
        }
        memcpy(key, arguments, key_length);
        key[key_length] = '\0';
        const char *value = skip_spaces(arguments + key_length);
        struct { const char *key; char *value; } settings[] = {
            {"parakeet", meeting->config->meeting_parakeet_exe},
            {"asr", meeting->config->meeting_asr_model},
            {"diar", meeting->config->meeting_diar_model},
            {"ffmpeg", meeting->config->meeting_ffmpeg_exe}
        };
        for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); ++i) {
            if (strcmp(key, settings[i].key) == 0 && *value && strlen(value) < 512) {
                snprintf(settings[i].value, 512, "%s", value);
                size_t n = strlen(settings[i].value);
                if (n >= 2 && settings[i].value[0] == '"' && settings[i].value[n - 1] == '"') {
                    memmove(settings[i].value, settings[i].value + 1, n - 2);
                    settings[i].value[n - 2] = '\0';
                }
                refresh_tools(meeting);
                snprintf(meeting->status, sizeof(meeting->status), "meeting %s configured", key);
                return 1;
            }
        }
        snprintf(meeting->status, sizeof(meeting->status), "usage: /meeting setup parakeet|asr|diar|ffmpeg <path>");
        return -1;
    }
    if (strcmp(verb, "model") == 0) {
        char *end;
        long port = strtol(arguments, &end, 10);
        const char *model = skip_spaces(end);
        if (end == arguments || (*end != ' ' && *end != '\t') || port < 1 || port > 65535 || !*model || strlen(model) >= 128) {
            snprintf(meeting->status, sizeof(meeting->status), "usage: /meeting model <localhost port> <exact model-id>");
            return -1;
        }
        meeting->llm_port = (int)port;
        snprintf(meeting->llm_model, sizeof(meeting->llm_model), "%s", model);
        meeting->config->meeting_llm_port = (int)port;
        snprintf(meeting->config->meeting_llm_model, 128, "%s", model);
        snprintf(meeting->status, sizeof(meeting->status), "LLM: localhost:%ld / %s", port, model);
        return 1;
    }
    if (strcmp(verb, "record") == 0 && !*arguments) {
        if (!meeting->directory[0] && new_meeting(meeting) != 0) {
            return -1;
        }
        char audio[700];
        snprintf(audio, sizeof(audio), "%s/audio.wav", meeting->directory);
        unsigned long long size;
        if (meeting->capture || meeting->job || fb_file_size(audio, &size)) {
            snprintf(meeting->status, sizeof(meeting->status), "finish the job or start a new meeting for another recording");
            return -1;
        }
        meeting->capture = fb_system_capture_start(audio);
        snprintf(meeting->status, sizeof(meeting->status), "%s", meeting->capture
                     ? "opening the default system-output device" : "could not start system-output recording");
        return meeting->capture ? 0 : -1;
    }
    if (strcmp(verb, "stop") == 0 && !*arguments) {
        if (!meeting->capture) {
            snprintf(meeting->status, sizeof(meeting->status), "no meeting recording is active");
            return -1;
        }
        fb_system_capture_stop(meeting->capture);
        snprintf(meeting->status, sizeof(meeting->status), "stopping and saving audio before transcription");
        return 0;
    }
    if (strcmp(verb, "transcribe") == 0 && !*arguments) {
        return start_job(meeting, FB_MEETING_JOB_TRANSCRIBE, NULL);
    }
    if (strcmp(verb, "import") == 0 && *path) {
        return start_job(meeting, FB_MEETING_JOB_TRANSCRIBE, path);
    }
    if (strcmp(verb, "process") == 0 && !*arguments) {
        return start_job(meeting, FB_MEETING_JOB_PROCESS, NULL);
    }
    if (strcmp(verb, "save") == 0 && !*arguments) {
        return start_save(meeting);
    }
    if (strcmp(verb, "cancel") == 0 && !*arguments && meeting->job) {
        fb_meeting_job_cancel(meeting->job);
        snprintf(meeting->status, sizeof(meeting->status), "cancelling; any current LLM request must return before the job ends");
        return 0;
    }
    if (strcmp(verb, "name") == 0 && meeting->document && !meeting->job && !meeting->save_job) {
        char *end;
        long number = strtol(arguments, &end, 10);
        if (end == arguments || (*end != ' ' && *end != '\t') ||
            number <= 0 || number > 2147483647L) {
            snprintf(meeting->status, sizeof(meeting->status), "usage: /meeting name <speaker number> <name>");
            return -1;
        }
        if (fb_meeting_document_name(meeting->document, (int)number, skip_spaces(end),
                                      meeting->status, sizeof(meeting->status)) == 0) {
            meeting->document_dirty = 1;
            refresh_output(meeting);
            start_save(meeting);
            snprintf(meeting->status, sizeof(meeting->status), "speaker %ld anchored; regenerate the report with /meeting process", number);
            return 0;
        }
        return -1;
    }
    if (strcmp(verb, "list") == 0 && !*arguments) {
        FbDirEntry entries[256];
        int count = fb_list_dir(meeting->root, entries, 256);
        free(meeting->output_text);
        meeting->output_text = NULL;
        fb_textview_clear(&meeting->output);
        fb_textview_add_line(&meeting->output, meeting->root);
        for (int i = 0; i < count; ++i) {
            if (entries[i].is_dir && strcmp(entries[i].name, "..") != 0) {
                fb_textview_add_line(&meeting->output, entries[i].name);
            }
        }
        snprintf(meeting->status, sizeof(meeting->status), "use /meeting open <full folder path> to reopen a saved meeting");
        return 0;
    }
    if (strcmp(verb, "open") == 0 && *path) {
        if (meeting->capture || meeting->job || meeting->save_job || meeting->document_dirty ||
            meeting->notes.revision != meeting->saved_revision) {
            snprintf(meeting->status, sizeof(meeting->status), "save notes and finish current jobs before opening another meeting");
            return -1;
        }
        meeting->job = fb_meeting_job_start(FB_MEETING_JOB_OPEN, NULL, path, NULL, NULL, NULL, 0, NULL);
        meeting->operation = FB_MEETING_JOB_OPEN;
        snprintf(meeting->opening_directory, sizeof(meeting->opening_directory), "%s", path);
        return meeting->job ? 0 : -1;
    }
    snprintf(meeting->status, sizeof(meeting->status), "unknown/incomplete command or job still active; /meeting help");
    return -1;
}

int fb_meeting_handle_event(FbMeeting *meeting, const FbEvent *event) {
    if (event->type != FB_EVENT_KEY) {
        return 0;
    }
    if (event->key == FB_KEY_F8) {
        meeting->command_focus = !meeting->command_focus;
        return 1;
    }
    if (event->key == FB_KEY_F9) {
        meeting->output_kind = (meeting->output_kind + 1) % 3;
        refresh_output(meeting);
        return 1;
    }
    if (event->key == FB_KEY_CTRL_SPACE) {
        fb_meeting_command(meeting, meeting->capture ? "stop" : "record");
        return 1;
    }
    if (event->key == FB_KEY_PAGE_UP || event->key == FB_KEY_PAGE_DOWN) {
        meeting->output.scroll += event->key == FB_KEY_PAGE_UP ? -10 : 10;
        if (meeting->output.scroll < 0) {
            meeting->output.scroll = 0;
        }
        if (meeting->output.scroll > meeting->output.count - 1) {
            meeting->output.scroll = meeting->output.count > 0 ? meeting->output.count - 1 : 0;
        }
        return 1;
    }
    if (!meeting->command_focus) {
        if (!meeting->directory[0] && new_meeting(meeting) != 0) {
            return 1;
        }
        int changed = fb_notepad_handle_event(&meeting->notes, event);
        if (changed > 0) {
            meeting->edited_ms = fb_now_ms();
        } else if (changed < 0) {
            snprintf(meeting->status, sizeof(meeting->status), "could not allocate space for more notes");
        }
        return 1;
    }
    return 0;
}

void fb_meeting_tick(FbMeeting *meeting) {
    if (meeting->save_job) {
        FbMeetingJobStatus status;
        fb_meeting_job_poll(meeting->save_job, &status);
        if (status.state != FB_MEETING_JOB_RUNNING) {
            if (status.state == FB_MEETING_JOB_DONE) {
                meeting->saved_revision = meeting->saving_revision;
            } else {
                meeting->save_failed = 1;
                if (meeting->saving_document) {
                    meeting->document_dirty = 1;
                }
                snprintf(meeting->status, sizeof(meeting->status), "%s", status.message);
            }
            fb_meeting_job_free(meeting->save_job);
            meeting->save_job = NULL;
            meeting->saving_document = 0;
        }
    }
    if (meeting->directory[0] && !meeting->save_job && !meeting->save_failed &&
        meeting->saved_revision != meeting->notes.revision && fb_now_ms() - meeting->edited_ms >= 750) {
        start_save(meeting);
    }
    if (meeting->capture) {
        fb_system_capture_poll(meeting->capture, &meeting->capture_status);
        if (meeting->capture_status.state == FB_CAPTURE_DONE || meeting->capture_status.state == FB_CAPTURE_ERROR) {
            int success = meeting->capture_status.state == FB_CAPTURE_DONE;
            fb_system_capture_free(meeting->capture);
            meeting->capture = NULL;
            if (success) {
                start_job(meeting, FB_MEETING_JOB_TRANSCRIBE, NULL);
            } else {
                snprintf(meeting->status, sizeof(meeting->status), "%s", meeting->capture_status.error);
            }
        }
    }
    if (meeting->job) {
        FbMeetingJobStatus status;
        fb_meeting_job_poll(meeting->job, &status);
        if (status.state == FB_MEETING_JOB_RUNNING) {
            if (status.message[0]) {
                snprintf(meeting->status, sizeof(meeting->status), "%s", status.message);
            }
        } else {
            int done = status.state == FB_MEETING_JOB_DONE;
            cJSON *document = fb_meeting_job_take_document(meeting->job);
            if (done || meeting->operation != FB_MEETING_JOB_OPEN) {
                if (document || (done && meeting->operation == FB_MEETING_JOB_OPEN)) {
                    cJSON_Delete(meeting->document);
                    meeting->document = document;
                    document = NULL;
                }
            }
            cJSON_Delete(document);
            if (done && meeting->operation == FB_MEETING_JOB_OPEN) {
                char *notes = fb_meeting_job_take_notes(meeting->job);
                fb_notepad_set(&meeting->notes, notes ? notes : "");
                free(notes);
                meeting->saved_revision = meeting->notes.revision;
                snprintf(meeting->directory, sizeof(meeting->directory), "%s", meeting->opening_directory);
            }
            if (done && (meeting->operation == FB_MEETING_JOB_PROCESS || meeting->operation == FB_MEETING_JOB_OPEN)) {
                char path[700];
                snprintf(path, sizeof(path), "%s/report.md", meeting->directory);
                free(meeting->report);
                meeting->report = fb_read_file(path, NULL);
            }
            snprintf(meeting->status, sizeof(meeting->status), "%s", status.state == FB_MEETING_JOB_CANCELLED
                       ? "meeting job cancelled; retained files and notes remain available" : status.message);
            fb_meeting_job_free(meeting->job);
            meeting->job = NULL;
            refresh_output(meeting);
        }
    }
}

void fb_meeting_render(FbMeeting *meeting, FbLayout layout, const FbCmdline *command) {
    fb_draw_text(1, layout.header.y, "Meeting | local audio, notes and outputs", 130, 205, 185,
                 FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);
    FbRect left = layout.main_area;
    left.w = layout.main_area.w / 2;
    FbRect right = {left.w + 1, left.y, layout.main_area.w - left.w - 1, left.h};
    if (meeting->output_width != right.w && meeting->output_text) {
        meeting->output_width = right.w;
        reflow_output(meeting);
    }
    fb_draw_text(left.x + 1, left.y, meeting->command_focus ? "Notes | F8 to edit" : "Notes | editing; autosave",
                 130, 205, 185, FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);
    const char *label = meeting->output_kind == 0 ? "Original ASR | F9 next" :
                        meeting->output_kind == 1 ? "Cleaned transcript | F9 next" : "Meeting report | F9 next";
    fb_draw_text(right.x, right.y, label, 130, 205, 185, FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);
    left.y++;
    left.h--;
    right.y++;
    right.h--;
    fb_notepad_render(&meeting->notes, left, !meeting->command_focus);
    fb_textview_render(&meeting->output, right);
    fb_draw_text(1, layout.statusbar.y, meeting->status, 190, 195, 205,
                 FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);
    char state[256];
    if (meeting->capture) {
        snprintf(state, sizeof(state), "REC system output %.1fs | %s | Ctrl+Space stops",
                 (double)meeting->capture_status.frames / 16000.0, meeting->capture_status.device_name);
    } else {
        snprintf(state, sizeof(state), "F1 help | F8 notes/commands | F9 outputs | PgUp/PgDn | Tab modes | F10 quit");
    }
    fb_draw_text(1, layout.hints.y, state, 130, 205, 185, FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);
    char location[700];
    snprintf(location, sizeof(location), "Folder: %s", meeting->directory[0] ? meeting->directory : "create with /meeting new");
    fb_draw_text(1, layout.hints.y + 1, location, 150, 155, 165,
                 FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);
    if (meeting->command_focus) {
        fb_cmdline_render(command, layout.cmdline, 130, 205, 185);
    } else {
        fb_draw_text(1, layout.cmdline.y, "Editing notes | Enter: new line | F8: commands | Ctrl+Space: system audio",
                     150, 155, 165, FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);
    }
}

void fb_meeting_shutdown(FbMeeting *meeting) {
    if (meeting->capture) {
        fb_system_capture_free(meeting->capture);
    }
    if (meeting->job) {
        fb_meeting_job_cancel(meeting->job);
        fb_meeting_job_wait(meeting->job);
        if (meeting->operation != FB_MEETING_JOB_OPEN) {
            cJSON *document = fb_meeting_job_take_document(meeting->job);
            if (document) {
                cJSON_Delete(meeting->document);
                meeting->document = document;
            }
        }
        fb_meeting_job_free(meeting->job);
    }
    if (meeting->save_job) {
        fb_meeting_job_free(meeting->save_job);
    }
    if (meeting->directory[0]) {
        FbMeetingJob *save = fb_meeting_job_start(FB_MEETING_JOB_SAVE, NULL, meeting->directory,
                             NULL, meeting->notes.text, meeting->document, 0, NULL);
        if (save) {
            fb_meeting_job_free(save);
        }
    }
    fb_notepad_free(&meeting->notes);
    cJSON_Delete(meeting->document);
    free(meeting->report);
    free(meeting->output_text);
}
