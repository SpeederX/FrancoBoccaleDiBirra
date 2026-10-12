#include "meeting/meeting_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "audio/system_capture.h"
#include "platform/platform.h"
#include "meeting/meeting_document.h"
#include "meeting/meeting_job.h"
#include "util/fileutil.h"
#include "util/paths.h"

int fb_meeting_launch_config(int argc, char **argv, FbConfig *config) {
    if (argc < 2 || strcmp(argv[1], "--meeting") != 0) {
        return 0;
    }
    for (int i = 2; i < argc; ++i) {
        if (i + 1 == argc) {
            fprintf(stderr, "missing value for %s\n", argv[i]);
            return -1;
        }
        const char *key = argv[i];
        const char *value = argv[++i];
        if (strcmp(key, "--llm-port") == 0) {
            char *end;
            long port = strtol(value, &end, 10);
            if (end == value || *end || port < 1 || port > 65535) {
                fprintf(stderr, "invalid localhost LLM port\n");
                return -1;
            }
            config->meeting_llm_port = (int)port;
            continue;
        }
        struct { const char *key; char *value; size_t capacity; } settings[] = {
            {"--parakeet", config->meeting_parakeet_exe, sizeof(config->meeting_parakeet_exe)},
            {"--library", config->meeting_parakeet_library, sizeof(config->meeting_parakeet_library)},
            {"--asr", config->meeting_asr_model, sizeof(config->meeting_asr_model)},
            {"--diar", config->meeting_diar_model, sizeof(config->meeting_diar_model)},
            {"--ffmpeg", config->meeting_ffmpeg_exe, sizeof(config->meeting_ffmpeg_exe)},
            {"--llm-model", config->meeting_llm_model, sizeof(config->meeting_llm_model)},
            {"--meeting-folder", config->meeting_folder, sizeof(config->meeting_folder)},
            {"--microphone", config->meeting_microphone, sizeof(config->meeting_microphone)},
            {"--output-device", config->meeting_output_device, sizeof(config->meeting_output_device)},
            {"--tts-python", config->meeting_tts_python, sizeof(config->meeting_tts_python)},
            {"--tts-script", config->meeting_tts_script, sizeof(config->meeting_tts_script)},
            {"--tts-model", config->meeting_tts_model, sizeof(config->meeting_tts_model)}
        };
        int found = 0;
        for (size_t n = 0; n < sizeof(settings) / sizeof(settings[0]); ++n) {
            if (strcmp(key, settings[n].key) == 0 && *value && strlen(value) < settings[n].capacity) {
                snprintf(settings[n].value, settings[n].capacity, "%s", value);
                found = 1;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "unknown argument, empty value or path too long: %s\n", key);
            return -1;
        }
    }
    return 1;
}

static int capture_cli(int argc, char **argv) {
    const char *output = NULL;
    int seconds = 0;
    int resume = 0;
    const char *microphone = NULL;
    const char *output_device = NULL;
    FbCaptureSource source = FB_CAPTURE_SYSTEM;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            char *end = NULL;
            long value = strtol(argv[++i], &end, 10);
            if (*end || value < 1 || value > INT_MAX) {
                fprintf(stderr, "--seconds must be a positive integer fitting in 32 bits\n");
                return 2;
            }
            seconds = (int)value;
        } else if (strcmp(argv[i], "--resume") == 0) {
            resume = 1;
        } else if (strcmp(argv[i], "--microphone") == 0 && i + 1 < argc) {
            microphone = argv[++i];
        } else if (strcmp(argv[i], "--output-device") == 0 && i + 1 < argc) {
            output_device = argv[++i];
        } else if (strcmp(argv[i], "--source") == 0 && i + 1 < argc) {
            const char *name = argv[++i];
            source = strcmp(name, "system") == 0 ? FB_CAPTURE_SYSTEM
                : strcmp(name, "microphone") == 0 ? FB_CAPTURE_MICROPHONE
                : strcmp(name, "both") == 0 ? FB_CAPTURE_BOTH : 0;
            if (!source) {
                fprintf(stderr, "--source must be system, microphone or both\n");
                return 2;
            }
        } else {
            fprintf(stderr, "unknown or incomplete argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!output || !seconds) {
        fprintf(stderr, "usage: fbdb --meeting-record --output new.wav --seconds N\n");
        return 2;
    }
    FILE *existing = fopen(output, "rb");
    if (existing && !resume) {
        fclose(existing);
        fprintf(stderr, "output already exists: %s\n", output);
        return 2;
    }
    if (existing && resume) {
        fclose(existing);
    }
    FbSystemCapture *capture = fb_system_capture_start_devices(output, source, resume, output_device, microphone);
    if (!capture) {
        fprintf(stderr, "could not start recording\n");
        return 1;
    }
    FbCaptureStatus status;
    unsigned long long deadline = 0;
    int stopped = 0;
    for (;;) {
        fb_system_capture_poll(capture, &status);
        if (status.state == FB_CAPTURE_ERROR || status.state == FB_CAPTURE_DONE) {
            break;
        }
        if (status.state == FB_CAPTURE_RECORDING && deadline == 0) {
            deadline = fb_now_ms() + (unsigned long long)seconds * 1000;
            fprintf(stderr, "recording system output: %s\n", status.device_name);
        }
        if (deadline && !stopped && fb_now_ms() >= deadline) {
            fb_system_capture_stop(capture);
            stopped = 1;
        }
        fb_sleep_ms(10);
    }
    printf("{\"frames\":%llu,\"dropped_frames\":%llu,\"sample_rate\":16000}\n",
           status.frames, status.dropped_frames);
    if (status.state == FB_CAPTURE_ERROR) {
        fprintf(stderr, "%s\n", status.error);
    }
    fb_system_capture_free(capture);
    return status.state == FB_CAPTURE_ERROR ? 1 : 0;
}

int fb_meeting_cli(int argc, char **argv) {
    if (strcmp(argv[1], "--meeting-sources") == 0 && argc == 2) {
        char sources[16384];
        if (fb_system_capture_sources(sources, sizeof(sources)) != 0) {
            return 1;
        }
        printf("%s", sources);
        return 0;
    }
    if (strcmp(argv[1], "--meeting-record") == 0) {
        return capture_cli(argc, argv);
    }
    FbMeetingOperation operation;
    if (strcmp(argv[1], "--meeting-transcribe") == 0) {
        operation = FB_MEETING_JOB_TRANSCRIBE;
    } else if (strcmp(argv[1], "--meeting-process") == 0) {
        operation = FB_MEETING_JOB_PROCESS;
    } else if (strcmp(argv[1], "--meeting-identify") == 0) {
        operation = FB_MEETING_JOB_IDENTIFY;
    } else {
        fprintf(stderr, "unknown meeting operation: %s\n", argv[1]);
        return 2;
    }
    FbMeetingConfig tools = {0};
    const char *directory = NULL;
    const char *source = NULL;
    const char *model = NULL;
    int port = 0;
    snprintf(tools.ffmpeg_exe, sizeof(tools.ffmpeg_exe), "ffmpeg");
    for (int i = 2; i < argc; ++i) {
        if (i + 1 == argc) {
            fprintf(stderr, "missing value for %s\n", argv[i]);
            return 2;
        }
        const char *key = argv[i];
        const char *value = argv[++i];
        if (strcmp(key, "--directory") == 0) {
            directory = value;
        } else if (strcmp(key, "--source") == 0) {
            source = value;
        } else if (strcmp(key, "--llm-model") == 0) {
            model = value;
        } else if (strcmp(key, "--llm-port") == 0) {
            char *end;
            long number = strtol(value, &end, 10);
            if (*end || end == value || number < 1 || number > 65535) {
                fprintf(stderr, "invalid localhost LLM port\n");
                return 2;
            }
            port = (int)number;
        } else {
            struct { const char *key; char *value; size_t capacity; } settings[] = {
                {"--parakeet", tools.parakeet_exe, sizeof(tools.parakeet_exe)},
                {"--library", tools.parakeet_library, sizeof(tools.parakeet_library)},
                {"--asr", tools.asr_model, sizeof(tools.asr_model)},
                {"--diar", tools.diar_model, sizeof(tools.diar_model)},
                {"--ffmpeg", tools.ffmpeg_exe, sizeof(tools.ffmpeg_exe)}
            };
            int found = 0;
            for (size_t n = 0; n < sizeof(settings) / sizeof(settings[0]); ++n) {
                if (strcmp(key, settings[n].key) == 0 && strlen(value) < settings[n].capacity) {
                    snprintf(settings[n].value, settings[n].capacity, "%s", value);
                    found = 1;
                    break;
                }
            }
            if (!found) {
                fprintf(stderr, "unknown argument or path too long: %s\n", key);
                return 2;
            }
        }
    }
    if (!directory || !*directory ||
        (operation == FB_MEETING_JOB_TRANSCRIBE &&
         ((!tools.parakeet_exe[0] && !tools.parakeet_library[0]) || !tools.asr_model[0] || !tools.diar_model[0])) ||
        ((operation == FB_MEETING_JOB_PROCESS || operation == FB_MEETING_JOB_IDENTIFY) && (!port || !model || !*model))) {
        fprintf(stderr, "usage:\n"
            "fbdb --meeting-transcribe --directory FOLDER --parakeet EXE --asr GGUF --diar GGUF [--source AUDIO_OR_VIDEO] [--ffmpeg EXE]\n"
            "fbdb --meeting-process --directory FOLDER --llm-port PORT --llm-model MODEL_ID\n");
        return 2;
    }
    if (fb_ensure_dir(directory) != 0) {
        fprintf(stderr, "could not create meeting directory\n");
        return 1;
    }
    char path[700];
    snprintf(path, sizeof(path), "%s/notes.txt", directory);
    char *notes = fb_read_file(path, NULL);
    if (!notes) {
        if (fb_write_file_atomic(path, "") != 0) {
            fprintf(stderr, "could not create notes.txt\n");
            return 1;
        }
    }
    cJSON *document = NULL;
    {
        snprintf(path, sizeof(path), "%s/transcript.json", directory);
        char *text = fb_read_file(path, NULL);
        document = text ? cJSON_Parse(text) : NULL;
        int required = operation != FB_MEETING_JOB_TRANSCRIBE || text != NULL;
        free(text);
        if (required && !fb_meeting_document_valid(document)) {
            fprintf(stderr, "missing or invalid transcript.json\n");
            cJSON_Delete(document);
            free(notes);
            return 1;
        }
    }
    FbMeetingJob *job = fb_meeting_job_start(operation, &tools, directory, source, notes,
                                           document, port, model);
    free(notes);
    cJSON_Delete(document);
    if (!job) {
        fprintf(stderr, "could not start meeting job\n");
        return 1;
    }
    FbMeetingJobStatus status;
    char previous[256] = "";
    do {
        fb_meeting_job_poll(job, &status);
        if (strcmp(previous, status.message) != 0) {
            fprintf(stderr, "%s\n", status.message);
            snprintf(previous, sizeof(previous), "%s", status.message);
        }
        if (status.state == FB_MEETING_JOB_RUNNING) {
            fb_sleep_ms(20);
        }
    } while (status.state == FB_MEETING_JOB_RUNNING);
    if (status.state == FB_MEETING_JOB_DONE && operation == FB_MEETING_JOB_IDENTIFY) {
        cJSON *identified = fb_meeting_job_take_document(job);
        if (fb_meeting_save_document(directory, identified) != 0) {
            status.state = FB_MEETING_JOB_ERROR;
        }
        cJSON_Delete(identified);
    }
    fb_meeting_job_free(job);
    return status.state == FB_MEETING_JOB_DONE ? 0 : 1;
}
