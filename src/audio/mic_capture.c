#include "miniaudio.h"

#include "audio/mic_capture.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "audio/ring_buffer.h"
#include "platform/platform.h"
#include "platform/thread.h"

#define FB_LEVEL_RB_CAPACITY 256
#define FB_MAX_RECORD_SAMPLES ((size_t)FB_MIC_SAMPLE_RATE * FB_MIC_MAX_RECORD_SECONDS)

static ma_device g_device;
static FbRingBuffer g_level_rb;
static FbThread *g_init_thread;
static FbMutex *g_init_mutex;
static FbMicState g_state;
static int g_stop_requested;

/* g_recording is written by the UI thread and read by the audio callback
   thread with no explicit fence; the callback may write a few extra/fewer
   samples right at a start/stop boundary. Harmless for short voice-command
   recordings, not worth synchronizing further. */
static volatile int g_recording = 0;
static float *g_record_buf = NULL;
static volatile size_t g_record_count = 0;

/* Runs on miniaudio's real-time audio thread: no blocking, no allocation,
   no UI calls. */
static void data_callback(ma_device *device, void *output, const void *input,
                           ma_uint32 frame_count) {
    (void)device;
    (void)output;
    const float *in = (const float *)input;
    if (!in || frame_count == 0) return;

    double sum_sq = 0.0;
    for (ma_uint32 i = 0; i < frame_count; ++i) {
        double s = (double)in[i];
        sum_sq += s * s;
    }
    float rms = (float)sqrt(sum_sq / (double)frame_count);
    if (rms > 1.0f) rms = 1.0f;
    fb_ring_buffer_write(&g_level_rb, &rms, 1);

    if (g_recording && g_record_buf) {
        size_t space = FB_MAX_RECORD_SAMPLES - g_record_count;
        size_t n = (size_t)frame_count < space ? (size_t)frame_count : space;
        if (n > 0) {
            memcpy(g_record_buf + g_record_count, in, n * sizeof(float));
            g_record_count += n;
        }
    }
}

static void init_worker(void *arg) {
    (void)arg;
    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_f32;
    config.capture.channels = 1;
    config.sampleRate = FB_MIC_SAMPLE_RATE;
    config.dataCallback = data_callback;

    if (ma_device_init(NULL, &config, &g_device) != MA_SUCCESS) {
        fb_mutex_lock(g_init_mutex);
        g_state = FB_MIC_ERROR;
        fb_mutex_unlock(g_init_mutex);
        return;
    }

    fb_mutex_lock(g_init_mutex);
    int stopped = g_stop_requested;
    fb_mutex_unlock(g_init_mutex);
    if (stopped) {
        ma_device_uninit(&g_device);
        return;
    }
    if (ma_device_start(&g_device) != MA_SUCCESS) {
        ma_device_uninit(&g_device);
        fb_mutex_lock(g_init_mutex);
        g_state = FB_MIC_ERROR;
        fb_mutex_unlock(g_init_mutex);
        return;
    }

    fb_mutex_lock(g_init_mutex);
    g_state = FB_MIC_READY;
    fb_mutex_unlock(g_init_mutex);
    for (;;) {
        fb_mutex_lock(g_init_mutex);
        int stop = g_stop_requested;
        fb_mutex_unlock(g_init_mutex);
        if (stop) {
            break;
        }
        fb_sleep_ms(10);
    }
    ma_device_uninit(&g_device);
}

int fb_mic_init(void) {
    if (g_init_thread) {
        return 0;
    }
    if (fb_ring_buffer_init(&g_level_rb, FB_LEVEL_RB_CAPACITY) != 0) {
        return -1;
    }
    g_record_buf = malloc(FB_MAX_RECORD_SAMPLES * sizeof(float));
    g_init_mutex = fb_mutex_create();
    if (!g_record_buf || !g_init_mutex) {
        fb_mic_shutdown();
        return -1;
    }
    g_stop_requested = 0;
    g_state = FB_MIC_STARTING;
    g_init_thread = fb_thread_start(init_worker, NULL);
    if (!g_init_thread) {
        fb_mic_shutdown();
        return -1;
    }
    return 0;
}

FbMicState fb_mic_state(void) {
    if (!g_init_mutex) {
        return FB_MIC_OFF;
    }
    fb_mutex_lock(g_init_mutex);
    FbMicState state = g_state;
    fb_mutex_unlock(g_init_mutex);
    return state;
}

void fb_mic_shutdown(void) {
    g_recording = 0;
    if (g_init_thread) {
        fb_mutex_lock(g_init_mutex);
        g_stop_requested = 1;
        fb_mutex_unlock(g_init_mutex);
        fb_thread_join(g_init_thread);
        g_init_thread = NULL;
    }
    fb_mutex_free(g_init_mutex);
    g_init_mutex = NULL;
    g_state = FB_MIC_OFF;
    free(g_record_buf);
    g_record_buf = NULL;
    fb_ring_buffer_free(&g_level_rb);
}

size_t fb_mic_poll_levels(float *out, size_t max_count) {
    if (fb_mic_state() != FB_MIC_READY) {
        return 0;
    }
    return fb_ring_buffer_read(&g_level_rb, out, max_count);
}

void fb_mic_set_recording(int recording) {
    if (recording) {
        g_record_count = 0;
        g_recording = 1;
    } else {
        g_recording = 0;
    }
}

const float *fb_mic_recorded_samples(size_t *out_count) {
    *out_count = g_record_count;
    return g_record_buf;
}
