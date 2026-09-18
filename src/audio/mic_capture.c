#define MA_IMPLEMENTATION
#include "miniaudio.h"

#include "audio/mic_capture.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "audio/ring_buffer.h"

#define FB_LEVEL_RB_CAPACITY 256
#define FB_MAX_RECORD_SAMPLES ((size_t)FB_MIC_SAMPLE_RATE * FB_MIC_MAX_RECORD_SECONDS)

static ma_device g_device;
static int g_device_started = 0;
static FbRingBuffer g_level_rb;

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

int fb_mic_init(void) {
    if (fb_ring_buffer_init(&g_level_rb, FB_LEVEL_RB_CAPACITY) != 0) return -1;

    g_record_buf = (float *)malloc(FB_MAX_RECORD_SAMPLES * sizeof(float));
    if (!g_record_buf) {
        fb_ring_buffer_free(&g_level_rb);
        return -1;
    }

    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_f32;
    config.capture.channels = 1;
    config.sampleRate = FB_MIC_SAMPLE_RATE;
    config.dataCallback = data_callback;

    if (ma_device_init(NULL, &config, &g_device) != MA_SUCCESS) {
        free(g_record_buf);
        g_record_buf = NULL;
        fb_ring_buffer_free(&g_level_rb);
        return -1;
    }

    if (ma_device_start(&g_device) != MA_SUCCESS) {
        ma_device_uninit(&g_device);
        free(g_record_buf);
        g_record_buf = NULL;
        fb_ring_buffer_free(&g_level_rb);
        return -1;
    }

    g_device_started = 1;
    return 0;
}

void fb_mic_shutdown(void) {
    if (g_device_started) {
        ma_device_uninit(&g_device);
        g_device_started = 0;
    }
    free(g_record_buf);
    g_record_buf = NULL;
    fb_ring_buffer_free(&g_level_rb);
}

size_t fb_mic_poll_levels(float *out, size_t max_count) {
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
