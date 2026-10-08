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
    snprintf(meeting->tools.parakeet_library, 512, "%s", cfg->meeting_parakeet_library);
    snprintf(meeting->tools.ffmpeg_exe, 512, "%s", cfg->meeting_ffmpeg_exe[0]
                 ? cfg->meeting_ffmpeg_exe : "ffmpeg");
}

void fb_meeting_init(FbMeeting *meeting, FbConfig *config) {
    memset(meeting, 0, sizeof(*meeting));
    meeting->config = config;
    char executable_dir[512];
    if (config->meeting_folder[0]) {
        snprintf(meeting->root, sizeof(meeting->root), "%s", config->meeting_folder);
    } else if (fb_get_executable_dir(executable_dir, sizeof(executable_dir)) == 0) {
        snprintf(meeting->root, sizeof(meeting->root), "%s/meetings", executable_dir);
    }
    fb_notepad_init(&meeting->notes);
    fb_textview_clear(&meeting->output);
    meeting->output_follow = 1;
    meeting->llm_port = config->meeting_llm_port;
    snprintf(meeting->llm_model, sizeof(meeting->llm_model), "%s", config->meeting_llm_model);
    refresh_tools(meeting);
    meeting->source = config->meeting_capture_source >= 1 && config->meeting_capture_source <= 3
                         ? (FbCaptureSource)config->meeting_capture_source : FB_CAPTURE_BOTH;
    snprintf(meeting->status, sizeof(meeting->status), "F8 commands, /meeting new to begin; /meeting help for setup");
}

static int output_max_scroll(const FbMeeting *meeting) {
    int rows = meeting->output_height > 0 ? meeting->output_height : 1;
    int maximum = meeting->output.count - rows;
    return maximum > 0 ? maximum : 0;
}

static void clamp_output_scroll(FbMeeting *meeting) {
    int maximum = output_max_scroll(meeting);
    if (meeting->output_follow || meeting->output.scroll > maximum) {
        meeting->output.scroll = maximum;
    }
}

static void reflow_output(FbMeeting *meeting) {
    const char *text = meeting->output_text;
    size_t anchor = meeting->output.count > 0
                        ? meeting->output_offsets[meeting->output.scroll] : 0;
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
        if (meeting->output.count == FB_TEXTVIEW_MAX_LINES) {
            memmove(meeting->output_offsets, meeting->output_offsets + 1,
                    (FB_TEXTVIEW_MAX_LINES - 1) * sizeof(meeting->output_offsets[0]));
        }
        fb_textview_add_line(&meeting->output, line);
        meeting->output_offsets[meeting->output.count - 1] = (size_t)(text - meeting->output_text);
        text += count;
        if (*text == '\n') {
            text++;
        } else if (wrapped && *text == ' ') {
            text++;
        }
    }
    for (int i = 0; i < meeting->output.count; ++i) {
        if (meeting->output_offsets[i] <= anchor) {
            meeting->output.scroll = i;
        }
    }
    clamp_output_scroll(meeting);
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

static void show_text(FbMeeting *meeting, const char *text) {
    meeting->output_follow = 0;
    meeting->output.scroll = 0;
    meeting->output_offsets[0] = 0;
    view_text(meeting, text);
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
        view_text(meeting, "No transcript yet. Ctrl+Space starts the selected audio sources.\nTranscription appears in blocks up to 20s. Ctrl+Space pauses/resumes.\nUse /meeting finish when done, or /meeting import <audio/video path>.\nAudio, notes and outputs are kept in the meeting folder.");
    }
}

static void help(FbMeeting *meeting) {
    show_text(meeting,
        "MEETING\n"
        "Type in Notes; Enter creates a new line. F8 switches to commands.\n"
        "Ctrl+Space starts, pauses and resumes the SAME meeting.\n"
        "F6 closes the meeting, completes transcription, then creates the report.\n"
        "Audio is transcribed in blocks up to 20s and shown here as it arrives.\n"
        "F9 cycles original transcript, cleaned transcript, and report.\n"
        "Report uses notes-used.txt; notes edited afterwards need /meeting process again.\n"
        "PgUp/PgDn scroll output. Tab changes mode; recording continues.\n\n"
        "/meeting new                 create a retained local meeting\n"
        "/meeting list                show meeting folders\n"
        "/meeting folder [path]       show/set storage for NEW meetings\n"
        "/meeting open <folder path>  reopen saved notes and transcript\n"
        "/meeting setup parakeet <executable path>\n"
        "/meeting setup library <Parakeet DLL/shared library path>\n"
        "/meeting setup asr <multilingual GGUF path>\n"
        "/meeting setup diar <diarization GGUF or bundle path>\n"
        "/meeting setup ffmpeg <executable path>\n"
        "/meeting model <port> <model-id> select localhost LLM\n"
        "/meeting sources             list available system/microphone devices\n"
        "/meeting source system|microphone|both  choose audio (default both)\n"
        "/meeting record              start/resume the selected audio sources\n"
        "/meeting stop                pause audio; session stays ready to resume\n"
        "/meeting finish              close, flush final speech, then create report (F6)\n"
        "/meeting import <file path>  import audio/video into a fresh meeting\n"
        "/meeting transcribe          retry ASR on retained audio.wav\n"
        "/meeting process             clean transcript and create report\n"
        "/meeting name <number> <name> anchor a speaker; manual names take priority\n"
        "/meeting speakers            list numbers, names and their evidence\n"
        "/meeting identify            check names with the selected LLM\n"
        "/meeting save                save current notes\n"
        "/meeting cancel              end capture and cancel jobs; retain audio/notes\n\n"
        "Paths may contain spaces; quotes around the entire path are optional.\n"
        "Both mixes system output and your microphone; choose either source separately.\n"
        "Diarization supports up to eight voices; unknown speech remains unattributed.\n"
        "With a selected LLM, names update from introductions or clear addressed-name/reply pairs.\n"
        "All LLM requests go to localhost. Notes used by a report are snapshotted.\n"
        "The UI shows up to 2000 output lines; complete exports remain on disk.");
}

static int start_save(FbMeeting *meeting) {
    if (meeting->save_job || !meeting->directory[0]) {
        return -1;
    }
    int save_document = meeting->document_dirty &&
                        !(meeting->job && meeting->operation == FB_MEETING_JOB_PROCESS);
    meeting->save_job = fb_meeting_job_start(FB_MEETING_JOB_SAVE, NULL, meeting->directory,
                       NULL, meeting->notes.text, save_document ? meeting->document : NULL, 0, NULL);
    if (!meeting->save_job) {
        snprintf(meeting->status, sizeof(meeting->status), "could not start saving notes");
        return -1;
    }
    meeting->saving_revision = meeting->notes.revision;
    meeting->saving_document = save_document;
    meeting->save_failed = 0;
    if (save_document) {
        meeting->document_dirty = 0;
    }
    return 0;
}

static int new_meeting(FbMeeting *meeting) {
    if (!meeting->root[0]) {
        snprintf(meeting->status, sizeof(meeting->status), "the app's local data folder is unavailable");
        return -1;
    }
    if (meeting->capture || meeting->job || meeting->asr_job || meeting->save_job ||
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
    meeting->paused = 0;
    meeting->recording_finished = 0;
    meeting->finish_pending = 0;
    meeting->process_pending = 0;
    meeting->identity_done = 0;
    free(meeting->report);
    meeting->report = NULL;
    meeting->output_kind = 0;
    meeting->output_follow = 1;
    meeting->command_focus = 0;
    refresh_output(meeting);
    start_save(meeting);
    snprintf(meeting->status, sizeof(meeting->status), "new local meeting; type notes or Ctrl+Space to start the selected audio sources");
    return 0;
}

static int start_job(FbMeeting *meeting, FbMeetingOperation operation, const char *source) {
    int asr = operation == FB_MEETING_JOB_TRANSCRIBE || operation == FB_MEETING_JOB_LIVE;
    if (!meeting->directory[0] ||
        (asr ? meeting->asr_job != NULL : meeting->job != NULL) ||
        (asr && meeting->capture && operation != FB_MEETING_JOB_LIVE)) {
        snprintf(meeting->status, sizeof(meeting->status), "start a meeting first and finish the current recording/job");
        return -1;
    }
    if (asr && ((!meeting->tools.parakeet_library[0] && !meeting->tools.parakeet_exe[0]) ||
         !meeting->tools.asr_model[0] || !meeting->tools.diar_model[0])) {
        snprintf(meeting->status, sizeof(meeting->status), "configure Parakeet, ASR and diarization paths: /meeting help");
        return -1;
    }
    if ((operation == FB_MEETING_JOB_PROCESS || operation == FB_MEETING_JOB_IDENTIFY) &&
        (!meeting->document || meeting->llm_port < 1 || !meeting->llm_model[0])) {
        snprintf(meeting->status, sizeof(meeting->status), "transcribe first, then select an LLM with /load or /meeting model");
        return -1;
    }
    if (meeting->save_job && (meeting->document_dirty || meeting->saving_document)) {
        snprintf(meeting->status, sizeof(meeting->status), "speaker names are being saved; retry when saving finishes");
        return -1;
    }
    FbMeetingOperation actual = operation == FB_MEETING_JOB_TRANSCRIBE ? FB_MEETING_JOB_TRANSCRIBE_PREVIEW : operation;
    FbMeetingJob *job = fb_meeting_job_start(actual, &meeting->tools, meeting->directory, source,
                       meeting->notes.text, meeting->document, meeting->llm_port, meeting->llm_model);
    if (!job) {
        snprintf(meeting->status, sizeof(meeting->status), "could not start the meeting job");
        return -1;
    }
    if (asr) {
        meeting->asr_job = job;
        meeting->asr_revision = 0;
        meeting->asr_live = operation == FB_MEETING_JOB_LIVE;
    } else {
        meeting->job = job;
        meeting->operation = operation;
        meeting->identifying_total = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(meeting->document, "utterances"));
        meeting->processing_revision = meeting->notes.revision;
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
    if (strcmp(verb, "folder") == 0) {
        if (!*path) {
            view_text(meeting, meeting->root);
            snprintf(meeting->status, sizeof(meeting->status), "meeting folder; default is beside the executable, under meetings");
            return 0;
        }
        if (strlen(arguments) >= sizeof(meeting->config->meeting_folder) || fb_ensure_dir(path) != 0) {
            snprintf(meeting->status, sizeof(meeting->status), "could not use that folder; choose an existing parent and a path under 512 bytes");
            return -1;
        }
        snprintf(meeting->config->meeting_folder, sizeof(meeting->config->meeting_folder), "%s", path);
        snprintf(meeting->root, sizeof(meeting->root), "%s", path);
        snprintf(meeting->status, sizeof(meeting->status), "folder saved for new meetings; current and previous meetings stay in their folders");
        return 1;
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
            {"ffmpeg", meeting->config->meeting_ffmpeg_exe},
            {"library", meeting->config->meeting_parakeet_library}
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
        snprintf(meeting->status, sizeof(meeting->status), "usage: /meeting setup parakeet|library|asr|diar|ffmpeg <path>");
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
        int exists = fb_file_size(audio, &size);
        if (meeting->capture || meeting->recording_finished ||
            (meeting->asr_job && !meeting->asr_live) ||
            (meeting->job && meeting->operation == FB_MEETING_JOB_OPEN) ||
            (exists && (!meeting->asr_job || !meeting->asr_live))) {
            snprintf(meeting->status, sizeof(meeting->status), "resume a paused live meeting; imported/finalized audio needs a new meeting");
            return -1;
        }
        if (!meeting->tools.parakeet_library[0]) {
            snprintf(meeting->status, sizeof(meeting->status), "configure the Parakeet streaming library first: /meeting setup library <path>");
            return -1;
        }
        meeting->capture = fb_system_capture_start_source(audio, meeting->source, exists);
        if (meeting->capture && !meeting->asr_job && start_job(meeting, FB_MEETING_JOB_LIVE, NULL) != 0) {
            fb_system_capture_free(meeting->capture);
            meeting->capture = NULL;
            return -1;
        }
        meeting->paused = 0;
        snprintf(meeting->status, sizeof(meeting->status), "%s", meeting->capture
                     ? "opening the selected default audio devices" : "could not start audio recording");
        return meeting->capture ? 0 : -1;
    }
    if (strcmp(verb, "stop") == 0 && !*arguments) {
        if (!meeting->capture) {
            snprintf(meeting->status, sizeof(meeting->status), "no meeting recording is active");
            return -1;
        }
        fb_system_capture_stop(meeting->capture);
        snprintf(meeting->status, sizeof(meeting->status), "pausing audio; Ctrl+Space resumes the same recording and speaker session");
        return 0;
    }
    if (strcmp(verb, "finish") == 0 && !*arguments) {
        if (!meeting->asr_job || !meeting->asr_live || meeting->finish_pending) {
            snprintf(meeting->status, sizeof(meeting->status), "no open live meeting to finish; /meeting process updates a closed meeting");
            return -1;
        }
        meeting->finish_pending = 1;
        meeting->process_pending = 1;
        if (meeting->capture) {
            fb_system_capture_stop(meeting->capture);
        } else {
            fb_meeting_job_audio_boundary(meeting->asr_job, 1);
            meeting->recording_finished = 1;
            meeting->paused = 0;
        }
        snprintf(meeting->status, sizeof(meeting->status), "closing audio; completing final speech before the report; notes stay editable");
        return 0;
    }
    if (strcmp(verb, "sources") == 0 && !*arguments) {
        char sources[8192];
        if (fb_system_capture_sources(sources, sizeof(sources)) != 0) {
            snprintf(meeting->status, sizeof(meeting->status), "could not enumerate audio sources");
            return -1;
        }
        show_text(meeting, sources);
        return 0;
    }
    if (strcmp(verb, "source") == 0) {
        FbCaptureSource source = strcmp(arguments, "system") == 0 ? FB_CAPTURE_SYSTEM
            : strcmp(arguments, "microphone") == 0 ? FB_CAPTURE_MICROPHONE
            : strcmp(arguments, "both") == 0 ? FB_CAPTURE_BOTH : 0;
        if (!source || meeting->capture) {
            snprintf(meeting->status, sizeof(meeting->status), "pause audio, then use /meeting source system|microphone|both");
            return -1;
        }
        meeting->source = source;
        meeting->config->meeting_capture_source = source;
        snprintf(meeting->status, sizeof(meeting->status), "audio source set to %s; applies on next start/resume", arguments);
        return 1;
    }
    if (strcmp(verb, "transcribe") == 0 && !*arguments) {
        return start_job(meeting, FB_MEETING_JOB_TRANSCRIBE, NULL);
    }
    if (strcmp(verb, "import") == 0 && *path) {
        return start_job(meeting, FB_MEETING_JOB_TRANSCRIBE, path);
    }
    if (strcmp(verb, "process") == 0 && !*arguments) {
        if (meeting->asr_job) {
            if (meeting->asr_live && !meeting->finish_pending) {
                return fb_meeting_command(meeting, "finish");
            }
            meeting->process_pending = 1;
            snprintf(meeting->status, sizeof(meeting->status), "report queued after transcription completes; notes remain editable");
            return 0;
        }
        return start_job(meeting, FB_MEETING_JOB_PROCESS, NULL);
    }
    if (strcmp(verb, "identify") == 0 && !*arguments) {
        return start_job(meeting, FB_MEETING_JOB_IDENTIFY, NULL);
    }
    if (strcmp(verb, "speakers") == 0 && !*arguments) {
        cJSON *speakers = cJSON_GetObjectItemCaseSensitive(meeting->document, "speakers");
        size_t capacity = 256;
        cJSON *speaker;
        cJSON_ArrayForEach(speaker, speakers) {
            cJSON *name = cJSON_GetObjectItemCaseSensitive(speaker, "name");
            cJSON *evidence = cJSON_GetObjectItemCaseSensitive(speaker, "evidence");
            capacity += 128 + (cJSON_IsString(name) ? strlen(name->valuestring) : 0)
                            + (cJSON_IsString(evidence) ? strlen(evidence->valuestring) : 0);
        }
        char *text = malloc(capacity);
        if (!text) {
            return -1;
        }
        size_t used = (size_t)snprintf(text, capacity, "SPEAKERS | /meeting name <number> <name> assigns or changes a name\n\n");
        cJSON_ArrayForEach(speaker, speakers) {
            cJSON *name = cJSON_GetObjectItemCaseSensitive(speaker, "name");
            cJSON *evidence = cJSON_GetObjectItemCaseSensitive(speaker, "evidence");
            const char *source = cJSON_GetObjectItemCaseSensitive(speaker, "source")->valuestring;
            const char *label = strcmp(source, "manual") == 0 ? "assigned by you"
                : strcmp(source, "introduction_inferred") == 0 ? "inferred from self introduction"
                : strcmp(source, "dialogue_inferred") == 0 ? "inferred from addressed name and reply" : "numbered voice";
            used += (size_t)snprintf(text + used, capacity - used, "Speaker %d: %s | %s\n",
                cJSON_GetObjectItemCaseSensitive(speaker, "id")->valueint,
                cJSON_IsString(name) ? name->valuestring : "no name assigned", label);
            if (cJSON_IsString(evidence)) {
                used += (size_t)snprintf(text + used, capacity - used, "Evidence: %s\n", evidence->valuestring);
            }
        }
        show_text(meeting, text);
        free(text);
        snprintf(meeting->status, sizeof(meeting->status), "speaker numbers, names, source and supporting quote; /meeting name <number> <name> to edit");
        return 0;
    }
    if (strcmp(verb, "save") == 0 && !*arguments) {
        return start_save(meeting);
    }
    if (strcmp(verb, "cancel") == 0 && !*arguments && (meeting->job || meeting->asr_job)) {
        meeting->process_pending = 0;
        if (meeting->job) {
            fb_meeting_job_cancel(meeting->job);
        }
        if (meeting->asr_job) {
            if (meeting->asr_live) {
                meeting->finish_pending = 1;
                meeting->recording_finished = 1;
                meeting->paused = 0;
                if (meeting->capture) {
                    fb_system_capture_stop(meeting->capture);
                }
            }
            fb_meeting_job_cancel(meeting->asr_job);
        }
        snprintf(meeting->status, sizeof(meeting->status), "cancelling; any current LLM request must return before the job ends");
        return 0;
    }
    if (strcmp(verb, "name") == 0 && meeting->document) {
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
        meeting->output_follow = 0;
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
        if (meeting->capture || meeting->job || meeting->asr_job || meeting->save_job || meeting->document_dirty ||
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
    if (event->key == FB_KEY_F6) {
        fb_meeting_command(meeting, "finish");
        return 1;
    }
    if (event->key == FB_KEY_F9) {
        meeting->output_kind = (meeting->output_kind + 1) % 3;
        meeting->output_follow = meeting->output_kind != 2;
        meeting->output.scroll = 0;
        meeting->output_offsets[0] = 0;
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
        int maximum = output_max_scroll(meeting);
        if (meeting->output.scroll > maximum) {
            meeting->output.scroll = maximum;
        }
        meeting->output_follow = meeting->output.scroll == maximum;
        return 1;
    }
    if (!meeting->command_focus) {
        if (!meeting->directory[0] && new_meeting(meeting) != 0) {
            return 1;
        }
        int changed = fb_notepad_handle_event(&meeting->notes, event);
        if (changed > 0) {
            meeting->edited_ms = fb_now_ms();
            cJSON *current = cJSON_GetObjectItemCaseSensitive(meeting->document, "report_current");
            if (cJSON_IsTrue(current)) {
                cJSON_DeleteItemFromObjectCaseSensitive(meeting->document, "report_current");
                cJSON_AddBoolToObject(meeting->document, "report_current", 0);
                meeting->document_dirty = 1;
                refresh_output(meeting);
            }
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
        (meeting->saved_revision != meeting->notes.revision || meeting->document_dirty) &&
        fb_now_ms() - meeting->edited_ms >= 750 &&
        !(meeting->job && meeting->operation == FB_MEETING_JOB_PROCESS && meeting->saved_revision == meeting->notes.revision)) {
        start_save(meeting);
    }
    if (meeting->capture) {
        fb_system_capture_poll(meeting->capture, &meeting->capture_status);
        if (meeting->capture_status.state == FB_CAPTURE_DONE || meeting->capture_status.state == FB_CAPTURE_ERROR) {
            int success = meeting->capture_status.state == FB_CAPTURE_DONE;
            fb_system_capture_free(meeting->capture);
            meeting->capture = NULL;
            meeting->paused = !meeting->finish_pending;
            meeting->recording_finished = meeting->finish_pending;
            if (meeting->asr_job) {
                fb_meeting_job_audio_boundary(meeting->asr_job, meeting->finish_pending);
            }
            if (!success) {
                meeting->process_pending = 0;
                snprintf(meeting->status, sizeof(meeting->status), "%s", meeting->capture_status.error);
            } else {
                snprintf(meeting->status, sizeof(meeting->status), "%s", meeting->paused
                    ? "paused; Ctrl+Space resumes this meeting; /meeting finish ends it"
                    : "recording finished; flushing the final Parakeet speech tail");
            }
        }
    }
    if (meeting->asr_job) {
        FbMeetingJobStatus status;
        fb_meeting_job_poll(meeting->asr_job, &status);
        cJSON *updated = fb_meeting_job_snapshot(meeting->asr_job, &meeting->asr_revision);
        if (updated) {
            fb_meeting_document_overlay(updated, meeting->document);
            cJSON_Delete(meeting->document);
            meeting->document = updated;
            meeting->document_dirty = 1;
            refresh_output(meeting);
        }
        if (status.state != FB_MEETING_JOB_RUNNING) {
            if (status.state != FB_MEETING_JOB_DONE) {
                meeting->process_pending = 0;
            }
            if (meeting->asr_live && meeting->capture) {
                meeting->finish_pending = 1;
                fb_system_capture_stop(meeting->capture);
            }
            if (!meeting->asr_revision && status.state == FB_MEETING_JOB_DONE) {
                cJSON *document = fb_meeting_job_take_document(meeting->asr_job);
                if (document) {
                    fb_meeting_document_overlay(document, meeting->document);
                    cJSON_Delete(meeting->document);
                    meeting->document = document;
                    meeting->document_dirty = 1;
                    refresh_output(meeting);
                }
            }
            snprintf(meeting->status, sizeof(meeting->status), "%s", status.state == FB_MEETING_JOB_CANCELLED
                ? "meeting job cancelled; retained files and notes remain available" : status.message);
            fb_meeting_job_free(meeting->asr_job);
            meeting->asr_job = NULL;
        } else if (!meeting->job && !meeting->paused && status.message[0]) {
            snprintf(meeting->status, sizeof(meeting->status), "%s", status.message);
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
            if (meeting->operation != FB_MEETING_JOB_OPEN && document && meeting->document) {
                cJSON *previous_speakers = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(meeting->document, "speakers"), 1);
                fb_meeting_document_overlay(meeting->document, document);
                int count = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(meeting->document, "utterances"));
                int processed = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(document, "utterances"));
                if (meeting->operation == FB_MEETING_JOB_PROCESS) {
                    cJSON *flag = cJSON_GetObjectItemCaseSensitive(document, "report_current");
                    cJSON_DeleteItemFromObjectCaseSensitive(meeting->document, "report_current");
                    int names_match = cJSON_Compare(cJSON_GetObjectItemCaseSensitive(meeting->document, "speakers"),
                                                   cJSON_GetObjectItemCaseSensitive(document, "speakers"), 1);
                    cJSON_AddBoolToObject(meeting->document, "report_current", cJSON_IsTrue(flag) && count == processed
                        && names_match && meeting->processing_revision == meeting->notes.revision);
                    cJSON_DeleteItemFromObjectCaseSensitive(meeting->document, "processed_utterances");
                    cJSON_AddNumberToObject(meeting->document, "processed_utterances", processed);
                } else if (!cJSON_Compare(previous_speakers, cJSON_GetObjectItemCaseSensitive(meeting->document, "speakers"), 1)) {
                    cJSON_DeleteItemFromObjectCaseSensitive(meeting->document, "report_current");
                    cJSON_AddBoolToObject(meeting->document, "report_current", 0);
                }
                cJSON_Delete(previous_speakers);
                meeting->document_dirty = 1;
            } else if (done || meeting->operation != FB_MEETING_JOB_OPEN) {
                if (document || (done && meeting->operation == FB_MEETING_JOB_OPEN)) {
                    cJSON_Delete(meeting->document);
                    meeting->document = document;
                    document = NULL;
                }
            }
            if (meeting->operation == FB_MEETING_JOB_IDENTIFY) {
                meeting->identity_done = meeting->identifying_total;
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
                if (meeting->operation == FB_MEETING_JOB_PROCESS) {
                    meeting->output_kind = 2;
                    meeting->output_follow = 0;
                    meeting->output.scroll = 0;
                    meeting->output_offsets[0] = 0;
                }
            }
            snprintf(meeting->status, sizeof(meeting->status), "%s", status.state == FB_MEETING_JOB_CANCELLED
                       ? "meeting job cancelled; retained files and notes remain available" : status.message);
            fb_meeting_job_free(meeting->job);
            meeting->job = NULL;
            refresh_output(meeting);
        }
    }
    int total = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(meeting->document, "utterances"));
    if (meeting->process_pending && !meeting->capture && !meeting->asr_job && !meeting->job && !meeting->save_job) {
        meeting->process_pending = 0;
        if (total == 0) {
            snprintf(meeting->status, sizeof(meeting->status), "meeting closed; no transcribed speech to summarize; notes remain editable");
        } else if (meeting->llm_port < 1 || !meeting->llm_model[0]) {
            snprintf(meeting->status, sizeof(meeting->status), "meeting closed; select /meeting model <port> <id>, then /meeting process for the report");
        } else {
            start_job(meeting, FB_MEETING_JOB_PROCESS, NULL);
        }
    }
    if (!meeting->process_pending && !meeting->job && meeting->llm_port > 0 && meeting->llm_model[0] &&
        total > meeting->identity_done && !meeting->save_job) {
        start_job(meeting, FB_MEETING_JOB_IDENTIFY, NULL);
    }
}

void fb_meeting_render(FbMeeting *meeting, FbLayout layout, const FbCmdline *command) {
    fb_draw_text(1, layout.header.y, "Meeting | local audio, notes and outputs", 130, 205, 185,
                 FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);
    FbRect left = layout.main_area;
    left.w = layout.main_area.w / 2;
    FbRect right = {left.w + 1, left.y, layout.main_area.w - left.w - 1, left.h};
    int output_height = right.h > 0 ? right.h - 1 : 0;
    int width_changed = meeting->output_width != right.w;
    meeting->output_height = output_height;
    if (width_changed && meeting->output_text && right.w > 0) {
        meeting->output_width = right.w;
        reflow_output(meeting);
    }
    clamp_output_scroll(meeting);
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
        snprintf(state, sizeof(state), "REC %.1fs | Ctrl+Space pauses | F6 finishes | %s",
                 (double)meeting->capture_status.frames / 16000.0, meeting->capture_status.device_name);
    } else if (meeting->paused && meeting->asr_job) {
        snprintf(state, sizeof(state), "PAUSED | Ctrl+Space resumes | F6 closes and creates report | F8 commands | F9 outputs");
    } else if (meeting->recording_finished) {
        snprintf(state, sizeof(state), "CLOSED | notes remain editable | F8 commands | /meeting process updates report | F9 outputs | F7 new");
    } else {
        snprintf(state, sizeof(state), "F1 help | F8 notes/commands | F9 outputs | PgUp/PgDn | Tab modes | F10 quit");
    }
    fb_draw_text(1, layout.hints.y, state, 130, 205, 185, FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);
    char location[700];
    snprintf(location, sizeof(location), "Source: %s | Folder: %s",
        meeting->source == FB_CAPTURE_BOTH ? "system+microphone" : meeting->source == FB_CAPTURE_SYSTEM ? "system" : "microphone",
        meeting->directory[0] ? meeting->directory : meeting->root);
    fb_draw_text(1, layout.hints.y + 1, location, 150, 155, 165,
                 FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);
    if (meeting->command_focus) {
        fb_cmdline_render(command, layout.cmdline, 130, 205, 185);
    } else {
        fb_draw_text(1, layout.cmdline.y, "Editing notes | Enter: new line | F8: commands | Ctrl+Space: record/pause/resume",
                     150, 155, 165, FB_COLOR_BG_R, FB_COLOR_BG_G, FB_COLOR_BG_B);
    }
}

void fb_meeting_shutdown(FbMeeting *meeting) {
    if (meeting->capture) {
        fb_system_capture_free(meeting->capture);
    }
    if (meeting->asr_job) {
        fb_meeting_job_audio_boundary(meeting->asr_job, 1);
        if (!meeting->asr_live) {
            fb_meeting_job_cancel(meeting->asr_job);
        }
        fb_meeting_job_wait(meeting->asr_job);
        cJSON *document = fb_meeting_job_take_document(meeting->asr_job);
        if (fb_meeting_document_valid(document)) {
            fb_meeting_document_overlay(document, meeting->document);
            cJSON_Delete(meeting->document);
            meeting->document = document;
            document = NULL;
        }
        cJSON_Delete(document);
        fb_meeting_job_free(meeting->asr_job);
    }
    if (meeting->job) {
        fb_meeting_job_cancel(meeting->job);
        fb_meeting_job_wait(meeting->job);
        if (meeting->operation != FB_MEETING_JOB_OPEN) {
            cJSON *document = fb_meeting_job_take_document(meeting->job);
            if (document) {
                if (meeting->document) {
                    fb_meeting_document_overlay(meeting->document, document);
                    cJSON_Delete(document);
                } else {
                    meeting->document = document;
                }
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
