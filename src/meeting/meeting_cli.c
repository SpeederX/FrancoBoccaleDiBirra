#include "meeting/meeting_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/system_capture.h"
#include "platform/platform.h"

static int capture_cli(int argc, char **argv) {
    const char *output = NULL;
    int seconds = 0;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            char *end = NULL;
            long value = strtol(argv[++i], &end, 10);
            if (*end || value < 1 || value > 86400) {
                fprintf(stderr, "--seconds must be an integer from 1 to 86400\n");
                return 2;
            }
            seconds = (int)value;
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
    if (existing) {
        fclose(existing);
        fprintf(stderr, "output already exists: %s\n", output);
        return 2;
    }
    FbSystemCapture *capture = fb_system_capture_start(output);
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
    if (strcmp(argv[1], "--meeting-record") == 0) {
        return capture_cli(argc, argv);
    }
    fprintf(stderr, "unknown meeting operation: %s\n", argv[1]);
    return 2;
}
