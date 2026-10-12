#define MA_IMPLEMENTATION
#include "miniaudio.h"
#include "audio/system_capture.h"
#include "audio/wav_stream.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct FbSystemCapture {
    FbCaptureStatus status;
    char path[600];
    int stopped;
    int resume;
};

void fb_capture_fixture_ready(FbSystemCapture *capture, int fail) {
    assert(capture);
    if (fail) {
        capture->status.state = FB_CAPTURE_ERROR;
        snprintf(capture->status.error, sizeof(capture->status.error), "fixture device open failure");
        return;
    }
    FbWavStream wav;
    int opened = capture->resume ? fb_wav_stream_resume(&wav, capture->path)
                                : fb_wav_stream_open(&wav, capture->path, 16000);
    assert(opened == 0);
    float pcm[8000] = {0};
    if (!capture->stopped) {
        assert(fb_wav_stream_append(&wav, pcm, 8000) == 0);
    }
    capture->status.frames = wav.frames;
    assert(fb_wav_stream_close(&wav) == 0);
    capture->status.state = capture->stopped ? FB_CAPTURE_DONE : FB_CAPTURE_RECORDING;
}

FbSystemCapture *fb_system_capture_start_devices(const char *path, FbCaptureSource source, int resume,
                                                const char *output, const char *microphone) {
    (void)source;
    (void)output;
    (void)microphone;
    FbSystemCapture *capture = calloc(1, sizeof(*capture));
    assert(capture);
    snprintf(capture->path, sizeof(capture->path), "%s", path);
    capture->resume = resume;
    return capture;
}

FbSystemCapture *fb_system_capture_start_source(const char *path, FbCaptureSource source, int resume) {
    return fb_system_capture_start_devices(path, source, resume, NULL, NULL);
}

FbSystemCapture *fb_system_capture_start(const char *path) {
    return fb_system_capture_start_source(path, FB_CAPTURE_SYSTEM, 0);
}

int fb_system_capture_sources(char *out, size_t capacity) {
    snprintf(out, capacity, "fixture capture");
    return 0;
}

void fb_system_capture_stop(FbSystemCapture *capture) {
    capture->stopped = 1;
    if (capture->status.state == FB_CAPTURE_RECORDING) {
        capture->status.state = FB_CAPTURE_DONE;
    }
}

void fb_system_capture_poll(FbSystemCapture *capture, FbCaptureStatus *status) {
    *status = capture->status;
}

void fb_system_capture_free(FbSystemCapture *capture) {
    free(capture);
}
