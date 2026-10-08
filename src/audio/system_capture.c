#define MA_IMPLEMENTATION
#include "miniaudio.h"

#include "audio/system_capture.h"
#include "audio/wav_stream.h"
#include "platform/thread.h"
#include "platform/platform.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define FB_SYSTEM_SAMPLE_RATE 16000
#define FB_SYSTEM_BUFFER_FRAMES 65536

struct FbSystemCapture {
    FbThread *thread;
    FbMutex *mutex;
    FbCaptureStatus status;
    char path[600];
    int stop_requested;
    ma_pcm_rb audio;
    unsigned long long dropped_frames;
};

static int stop_requested(FbSystemCapture *capture) {
    fb_mutex_lock(capture->mutex);
    int result = capture->stop_requested;
    fb_mutex_unlock(capture->mutex);
    return result;
}

static void capture_callback(ma_device *device, void *output, const void *input,
                             ma_uint32 frames) {
    (void)output;
    FbSystemCapture *capture = device->pUserData;
    const float *samples = input;
    if (!samples) {
        return;
    }
    ma_uint32 offset = 0;
    while (offset < frames) {
        ma_uint32 take = frames - offset;
        void *buffer = NULL;
        ma_pcm_rb_acquire_write(&capture->audio, &take, &buffer);
        if (take == 0) {
            capture->dropped_frames += frames - offset;
            break;
        }
        memcpy(buffer, samples + offset, take * sizeof(float));
        ma_pcm_rb_commit_write(&capture->audio, take);
        offset += take;
    }
}

static int drain_audio(FbSystemCapture *capture, FbWavStream *wav) {
    ma_uint32 count = 4096;
    void *samples = NULL;
    ma_pcm_rb_acquire_read(&capture->audio, &count, &samples);
    if (count == 0) {
        return 0;
    }
    int result = fb_wav_stream_append(wav, samples, count);
    ma_pcm_rb_commit_read(&capture->audio, count);
    fb_mutex_lock(capture->mutex);
    capture->status.frames = wav->frames;
    fb_mutex_unlock(capture->mutex);
    return result == 0 ? 1 : -1;
}

static void capture_worker(void *arg) {
    FbSystemCapture *capture = arg;
    ma_context context;
    ma_device device;
    ma_device clock_device;
    FbWavStream wav;
    int context_ready = 0;
    int device_ready = 0;
    int clock_ready = 0;
    int ring_ready = 0;
    int wav_ready = 0;
    const char *error = NULL;

    if (ma_context_init(NULL, 0, NULL, &context) != MA_SUCCESS) {
        error = "could not initialize system audio";
        goto done;
    }
    context_ready = 1;
    if (!ma_context_is_loopback_supported(&context)) {
        error = "system-output capture needs the Windows WASAPI backend; import audio here";
        goto done;
    }
    if (ma_pcm_rb_init(ma_format_f32, 1, FB_SYSTEM_BUFFER_FRAMES, NULL, NULL,
                      &capture->audio) != MA_SUCCESS) {
        error = "could not allocate the system-audio buffer";
        goto done;
    }
    ring_ready = 1;
    ma_device_config config = ma_device_config_init(ma_device_type_loopback);
    config.capture.format = ma_format_f32;
    config.capture.channels = 1;
    config.sampleRate = FB_SYSTEM_SAMPLE_RATE;
    config.dataCallback = capture_callback;
    config.pUserData = capture;
    if (ma_device_init(&context, &config, &device) != MA_SUCCESS) {
        error = "could not open the default system-output device";
        goto done;
    }
    device_ready = 1;
    ma_device_config clock_config = ma_device_config_init(ma_device_type_playback);
    clock_config.playback.pDeviceID = &device.capture.id;
    clock_config.playback.format = ma_format_f32;
    clock_config.playback.channels = 1;
    clock_config.sampleRate = FB_SYSTEM_SAMPLE_RATE;
    if (ma_device_init(&context, &clock_config, &clock_device) != MA_SUCCESS) {
        error = "could not keep the output clock running through quiet periods";
        goto done;
    }
    clock_ready = 1;
    fb_mutex_lock(capture->mutex);
    snprintf(capture->status.device_name, sizeof(capture->status.device_name),
             "%s", device.capture.name);
    fb_mutex_unlock(capture->mutex);
    if (fb_wav_stream_open(&wav, capture->path, FB_SYSTEM_SAMPLE_RATE) != 0) {
        error = "could not create audio.wav in the meeting folder";
        goto done;
    }
    wav_ready = 1;
    if (ma_device_start(&clock_device) != MA_SUCCESS || ma_device_start(&device) != MA_SUCCESS) {
        error = "could not start system-output recording";
        goto done;
    }
    fb_mutex_lock(capture->mutex);
    capture->status.state = capture->stop_requested ? FB_CAPTURE_STOPPING : FB_CAPTURE_RECORDING;
    fb_mutex_unlock(capture->mutex);
    while (!stop_requested(capture)) {
        int result = drain_audio(capture, &wav);
        if (result < 0) {
            error = "audio write failed or exceeded the PCM WAV size limit";
            break;
        }
        if (result == 0) {
            fb_sleep_ms(5);
        }
    }

done:
    if (device_ready) {
        ma_device_uninit(&device);
    }
    if (clock_ready) {
        ma_device_uninit(&clock_device);
    }
    if (wav_ready) {
        while (!error && drain_audio(capture, &wav) > 0) {
        }
        int close_result = fb_wav_stream_close(&wav);
        if (close_result != 0) {
            error = "could not complete the recording; inspect the retained audio file";
        }
    }
    if (ring_ready) {
        ma_pcm_rb_uninit(&capture->audio);
    }
    if (context_ready) {
        ma_context_uninit(&context);
    }
    fb_mutex_lock(capture->mutex);
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
    if (!path || !path[0] || strlen(path) >= 600) {
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
    capture->thread = fb_thread_start(capture_worker, capture);
    if (!capture->thread) {
        fb_mutex_free(capture->mutex);
        free(capture);
        return NULL;
    }
    return capture;
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
