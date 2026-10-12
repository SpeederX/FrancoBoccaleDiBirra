#define MA_IMPLEMENTATION
#include "miniaudio.h"

#include "audio/system_capture.h"
#include "audio/device_selection.h"
#include "audio/wav_stream.h"
#include "audio/audio_mix.h"
#include "platform/thread.h"
#include "platform/platform.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define FB_SYSTEM_SAMPLE_RATE 16000
#define FB_SYSTEM_BUFFER_FRAMES 65536

typedef struct {
    FbSystemCapture *capture;
    int index;
    unsigned long long dropped_frames;
} FbCaptureInput;

struct FbSystemCapture {
    FbThread *thread;
    FbMutex *mutex;
    FbCaptureStatus status;
    char path[600];
    int stop_requested;
    ma_pcm_rb audio[2];
    FbCaptureInput inputs[2];
    FbCaptureSource source;
    char device_names[2][256];
    int resume;
    unsigned long long dropped_frames;
};

static int stop_requested(FbSystemCapture *capture) {
    fb_mutex_lock(capture->mutex);
    int result = capture->stop_requested;
    fb_mutex_unlock(capture->mutex);
    return result;
}

static void opening_stage(FbSystemCapture *capture, const char *stage) {
    fb_mutex_lock(capture->mutex);
    snprintf(capture->status.stage, sizeof(capture->status.stage), "%s", stage);
    fb_mutex_unlock(capture->mutex);
}

static void capture_callback(ma_device *device, void *output, const void *input,
                             ma_uint32 frames) {
    (void)output;
    FbCaptureInput *source = device->pUserData;
    FbSystemCapture *capture = source->capture;
    const float *samples = input;
    if (!samples) {
        return;
    }
    ma_uint32 offset = 0;
    while (offset < frames) {
        ma_uint32 take = frames - offset;
        void *buffer = NULL;
        ma_pcm_rb_acquire_write(&capture->audio[source->index], &take, &buffer);
        if (take == 0) {
            source->dropped_frames += frames - offset;
            break;
        }
        memcpy(buffer, samples + offset, take * sizeof(float));
        ma_pcm_rb_commit_write(&capture->audio[source->index], take);
        offset += take;
    }
}

static int drain_audio(FbSystemCapture *capture, FbWavStream *wav, int final) {
    ma_uint32 count = 4096;
    ma_uint32 available[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        if (capture->source & (1 << i)) {
            available[i] = ma_pcm_rb_available_read(&capture->audio[i]);
            if (!final && available[i] < count) {
                count = available[i];
            }
        }
    }
    if (final) {
        ma_uint32 most = available[0] > available[1] ? available[0] : available[1];
        if (count > most) {
            count = most;
        }
    }
    if (count == 0) {
        return 0;
    }
    float data[2][4096] = {{0}};
    float mixed[4096];
    for (int i = 0; i < 2; ++i) {
        ma_uint32 offset = 0;
        while (offset < count && offset < available[i]) {
            ma_uint32 take = count - offset;
            void *samples = NULL;
            ma_pcm_rb_acquire_read(&capture->audio[i], &take, &samples);
            if (!take) {
                break;
            }
            memcpy(data[i] + offset, samples, take * sizeof(float));
            ma_pcm_rb_commit_read(&capture->audio[i], take);
            offset += take;
        }
    }
    fb_audio_mix(mixed, capture->source & FB_CAPTURE_SYSTEM ? data[0] : NULL,
                        capture->source & FB_CAPTURE_MICROPHONE ? data[1] : NULL, count);
    int result = fb_wav_stream_append(wav, mixed, count);
    fb_mutex_lock(capture->mutex);
    capture->status.frames = wav->frames;
    fb_mutex_unlock(capture->mutex);
    return result == 0 ? 1 : -1;
}

static void capture_worker(void *arg) {
    FbSystemCapture *capture = arg;
    ma_context context;
    ma_device devices[2];
    ma_device clock_device;
    FbWavStream wav;
    int context_ready = 0;
    int device_ready[2] = {0};
    int clock_ready = 0;
    int ring_ready[2] = {0};
    int wav_ready = 0;
    const char *error = NULL;

    opening_stage(capture, "Audio: initializing Windows capture");
    if (ma_context_init(NULL, 0, NULL, &context) != MA_SUCCESS) {
        error = "could not initialize system audio";
        goto done;
    }
    context_ready = 1;
    if (stop_requested(capture)) {
        goto done;
    }
    if ((capture->source & FB_CAPTURE_SYSTEM) && !ma_context_is_loopback_supported(&context)) {
        error = "system-output capture needs the Windows WASAPI backend; import audio here";
        goto done;
    }
    for (int i = 0; i < 2; ++i) {
        if (!(capture->source & (1 << i))) {
            continue;
        }
        if (stop_requested(capture)) {
            goto done;
        }
        if (ma_pcm_rb_init(ma_format_f32, 1, FB_SYSTEM_BUFFER_FRAMES, NULL, NULL,
                          &capture->audio[i]) != MA_SUCCESS) {
            error = "could not allocate the audio buffer";
            goto done;
        }
        ring_ready[i] = 1;
        ma_device_config config = ma_device_config_init(i == 0 ? ma_device_type_loopback : ma_device_type_capture);
        config.capture.format = ma_format_f32;
        config.capture.channels = 1;
        config.sampleRate = FB_SYSTEM_SAMPLE_RATE;
        config.dataCallback = capture_callback;
        config.pUserData = &capture->inputs[i];
        ma_device_id id;
        char name[256];
        opening_stage(capture, i == 0 ? "Audio: resolving system output" : "Audio: resolving microphone");
        if (fb_audio_device_resolve(&context, (FbCaptureSource)(1 << i), capture->device_names[i],
                                    &id, name, sizeof(name)) != 0) {
            error = i == 0 ? "selected output device unavailable; use /meeting output or /meeting sources"
                           : "selected microphone unavailable; use /meeting microphone or /meeting sources";
            goto done;
        }
        config.capture.pDeviceID = name[0] ? &id : NULL;
        opening_stage(capture, i == 0 ? "Audio: opening system output" : "Audio: opening microphone");
        if (ma_device_init(&context, &config, &devices[i]) != MA_SUCCESS) {
            error = i == 0 ? "could not open selected system output" : "could not open selected microphone";
            goto done;
        }
        device_ready[i] = 1;
    }
    if (stop_requested(capture)) {
        goto done;
    }
    if (capture->source & FB_CAPTURE_SYSTEM) {
        opening_stage(capture, "Audio: starting the system output clock");
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.pDeviceID = &devices[0].capture.id;
        config.playback.format = ma_format_f32;
        config.playback.channels = 1;
        config.sampleRate = FB_SYSTEM_SAMPLE_RATE;
        if (ma_device_init(&context, &config, &clock_device) != MA_SUCCESS) {
            error = "could not keep the output clock running through quiet periods";
            goto done;
        }
        clock_ready = 1;
        if (ma_device_start(&clock_device) != MA_SUCCESS) {
            error = "could not start the output clock";
            goto done;
        }
    }
    if (stop_requested(capture)) {
        goto done;
    }
    fb_mutex_lock(capture->mutex);
    snprintf(capture->status.device_name, sizeof(capture->status.device_name), "%s%s%s",
        device_ready[0] ? devices[0].capture.name : "",
        capture->source == FB_CAPTURE_BOTH ? " + mic: " : "",
        device_ready[1] ? devices[1].capture.name : "");
    fb_mutex_unlock(capture->mutex);
    opening_stage(capture, "Audio: opening the recording file");
    int opened = capture->resume ? fb_wav_stream_resume(&wav, capture->path)
                                : fb_wav_stream_open(&wav, capture->path, FB_SYSTEM_SAMPLE_RATE);
    if (opened != 0) {
        error = "could not create audio.wav in the meeting folder";
        goto done;
    }
    wav_ready = 1;
    for (int i = 0; i < 2; ++i) {
        opening_stage(capture, "Audio: starting the selected sources");
        if (device_ready[i] && ma_device_start(&devices[i]) != MA_SUCCESS) {
            error = "could not start the selected audio source";
            goto done;
        }
    }
    fb_mutex_lock(capture->mutex);
    snprintf(capture->status.stage, sizeof(capture->status.stage), "Audio: recording");
    capture->status.state = capture->stop_requested ? FB_CAPTURE_STOPPING : FB_CAPTURE_RECORDING;
    fb_mutex_unlock(capture->mutex);
    while (!stop_requested(capture)) {
        int result = drain_audio(capture, &wav, 0);
        if (result < 0) {
            error = "audio write failed or exceeded the PCM WAV size limit";
            break;
        }
        if (result == 0) {
            fb_sleep_ms(5);
        }
    }

done:
    for (int i = 0; i < 2; ++i) {
        if (device_ready[i]) {
            ma_device_uninit(&devices[i]);
        }
    }
    if (clock_ready) {
        ma_device_uninit(&clock_device);
    }
    if (wav_ready) {
        while (!error && drain_audio(capture, &wav, 1) > 0) {
        }
        int close_result = fb_wav_stream_close(&wav);
        if (close_result != 0) {
            error = "could not complete the recording; inspect the retained audio file";
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (ring_ready[i]) {
            ma_pcm_rb_uninit(&capture->audio[i]);
        }
    }
    if (context_ready) {
        ma_context_uninit(&context);
    }
    fb_mutex_lock(capture->mutex);
    capture->dropped_frames = capture->inputs[0].dropped_frames + capture->inputs[1].dropped_frames;
    capture->status.dropped_frames = capture->dropped_frames;
    if (capture->dropped_frames && !error) {
        error = "recording has missing audio frames; the retained WAV can be transcribed";
    }
    if (error) {
        snprintf(capture->status.error, sizeof(capture->status.error), "%s", error);
    }
    capture->status.state = error ? FB_CAPTURE_ERROR : FB_CAPTURE_DONE;
    fb_mutex_unlock(capture->mutex);
}

FbSystemCapture *fb_system_capture_start(const char *path) {
    return fb_system_capture_start_source(path, FB_CAPTURE_SYSTEM, 0);
}

FbSystemCapture *fb_system_capture_start_source(const char *path, FbCaptureSource source, int resume) {
    return fb_system_capture_start_devices(path, source, resume, NULL, NULL);
}

FbSystemCapture *fb_system_capture_start_devices(const char *path, FbCaptureSource source, int resume,
                                                const char *output_device, const char *microphone) {
    if (!path || !path[0] || strlen(path) >= 600 || source < 1 || source > 3) {
        return NULL;
    }
    FbSystemCapture *capture = calloc(1, sizeof(*capture));
    if (!capture) {
        return NULL;
    }
    capture->mutex = fb_mutex_create();
    if (!capture->mutex) {
        free(capture);
        return NULL;
    }
    snprintf(capture->path, sizeof(capture->path), "%s", path);
    capture->source = source;
    snprintf(capture->device_names[0], sizeof(capture->device_names[0]), "%s", output_device ? output_device : "");
    snprintf(capture->device_names[1], sizeof(capture->device_names[1]), "%s", microphone ? microphone : "");
    capture->resume = resume;
    for (int i = 0; i < 2; ++i) {
        capture->inputs[i].capture = capture;
        capture->inputs[i].index = i;
    }
    capture->thread = fb_thread_start(capture_worker, capture);
    if (!capture->thread) {
        fb_mutex_free(capture->mutex);
        free(capture);
        return NULL;
    }
    return capture;
}

int fb_system_capture_sources(char *out, size_t capacity) {
    return fb_audio_devices_list(out, capacity);
}

void fb_system_capture_stop(FbSystemCapture *capture) {
    fb_mutex_lock(capture->mutex);
    capture->stop_requested = 1;
    if (capture->status.state == FB_CAPTURE_RECORDING) {
        capture->status.state = FB_CAPTURE_STOPPING;
    }
    fb_mutex_unlock(capture->mutex);
}

void fb_system_capture_poll(FbSystemCapture *capture, FbCaptureStatus *status) {
    fb_mutex_lock(capture->mutex);
    *status = capture->status;
    fb_mutex_unlock(capture->mutex);
}

void fb_system_capture_free(FbSystemCapture *capture) {
    if (capture) {
        fb_system_capture_stop(capture);
        fb_thread_join(capture->thread);
        fb_mutex_free(capture->mutex);
        free(capture);
    }
}
