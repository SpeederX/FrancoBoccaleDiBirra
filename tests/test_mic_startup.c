#include <assert.h>
#include <string.h>

#include "miniaudio.h"
#include "audio/mic_capture.h"
#include "platform/platform.h"
#include "platform/thread.h"

static FbMutex *fixture_mutex;
static int entered;
static int released;
static int fail_open;
static int fail_start;
static int starts;
static int closes;
static ma_device *fixture_device;
static ma_device_data_proc fixture_callback;

ma_device_config ma_device_config_init(ma_device_type type) {
    ma_device_config config = {0};
    config.deviceType = type;
    return config;
}

ma_result ma_device_init(ma_context *context, const ma_device_config *config, ma_device *device) {
    (void)context;
    assert(config->capture.channels == 1 && config->sampleRate == 16000);
    fixture_device = device;
    fixture_callback = config->dataCallback;
    fb_mutex_lock(fixture_mutex);
    entered = 1;
    fb_mutex_unlock(fixture_mutex);
    for (;;) {
        fb_mutex_lock(fixture_mutex);
        int proceed = released;
        fb_mutex_unlock(fixture_mutex);
        if (proceed) {
            break;
        }
        fb_sleep_ms(1);
    }
    return fail_open ? MA_ERROR : MA_SUCCESS;
}

ma_result ma_device_start(ma_device *device) {
    (void)device;
    starts++;
    return fail_start ? MA_ERROR : MA_SUCCESS;
}

void ma_device_uninit(ma_device *device) {
    (void)device;
    closes++;
}

static void release_device(void) {
    fb_mutex_lock(fixture_mutex);
    released = 1;
    fb_mutex_unlock(fixture_mutex);
}

static void await_state(FbMicState expected) {
    unsigned long long limit = fb_now_ms() + 2000;
    while (fb_mic_state() != expected) {
        assert(fb_now_ms() < limit);
        fb_sleep_ms(1);
    }
}

static void begin(void) {
    entered = 0;
    released = 0;
    starts = 0;
    closes = 0;
    assert(fb_mic_init() == 0);
    unsigned long long limit = fb_now_ms() + 2000;
    for (;;) {
        fb_mutex_lock(fixture_mutex);
        int pending = entered;
        fb_mutex_unlock(fixture_mutex);
        if (pending) {
            break;
        }
        assert(fb_now_ms() < limit);
        fb_sleep_ms(1);
    }
    assert(fb_mic_state() == FB_MIC_STARTING);
    float level;
    assert(fb_mic_poll_levels(&level, 1) == 0);
    assert(fb_mic_init() == 0);
}

static void delayed_release(void *arg) {
    (void)arg;
    fb_sleep_ms(50);
    release_device();
}

int main(void) {
    fixture_mutex = fb_mutex_create();
    assert(fixture_mutex && fb_mic_state() == FB_MIC_OFF);
    begin();
    release_device();
    await_state(FB_MIC_READY);
    float input[] = {0.5f, -0.5f};
    fb_mic_set_recording(1);
    fixture_callback(fixture_device, NULL, input, 2);
    fb_mic_set_recording(0);
    float level;
    assert(fb_mic_poll_levels(&level, 1) == 1 && level == 0.5f);
    size_t count;
    const float *recorded = fb_mic_recorded_samples(&count);
    assert(count == 2 && memcmp(recorded, input, sizeof(input)) == 0);
    fb_mic_shutdown();
    assert(fb_mic_state() == FB_MIC_OFF && starts == 1 && closes == 1);

    fail_open = 1;
    begin();
    release_device();
    await_state(FB_MIC_ERROR);
    assert(fb_mic_poll_levels(&level, 1) == 0);
    fb_mic_shutdown();
    assert(starts == 0 && closes == 0);

    fail_open = 0;
    fail_start = 1;
    begin();
    release_device();
    await_state(FB_MIC_ERROR);
    fb_mic_shutdown();
    assert(starts == 1 && closes == 1);

    fail_start = 0;
    begin();
    FbThread *release = fb_thread_start(delayed_release, NULL);
    assert(release);
    fb_mic_shutdown();
    fb_thread_join(release);
    assert(fb_mic_state() == FB_MIC_OFF && starts == 0 && closes == 1);
    fb_mutex_free(fixture_mutex);
    return 0;
}
