#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "audio/device_selection.h"
#include "platform/platform.h"

static ma_device_info playback[2];
static ma_device_info microphones[3];
static int fail_init;
static int fail_list;

ma_result ma_context_init(const ma_backend *backends, ma_uint32 count,
                          const ma_context_config *config, ma_context *context) {
    (void)backends;
    (void)count;
    (void)config;
    memset(context, 0, sizeof(*context));
    return fail_init ? MA_ERROR : MA_SUCCESS;
}

ma_result ma_context_get_devices(ma_context *context, ma_device_info **outputs, ma_uint32 *output_count,
                                 ma_device_info **inputs, ma_uint32 *input_count) {
    (void)context;
    *outputs = playback;
    *output_count = 2;
    *inputs = microphones;
    *input_count = 3;
    return fail_list ? MA_ERROR : MA_SUCCESS;
}

ma_result ma_context_uninit(ma_context *context) {
    (void)context;
    return MA_SUCCESS;
}

static void wait_for_job(FbAudioDeviceJob *job, FbAudioDeviceStatus *status) {
    assert(job);
    unsigned long long limit = fb_now_ms() + 2000;
    do {
        fb_audio_devices_poll(job, status);
        assert(fb_now_ms() < limit);
        fb_sleep_ms(1);
    } while (!status->done);
    fb_audio_devices_free(job);
}

int main(void) {
    snprintf(playback[0].name, sizeof(playback[0].name), "Speakers");
    snprintf(playback[1].name, sizeof(playback[1].name), "Headphones output");
    snprintf(microphones[0].name, sizeof(microphones[0].name), "Webcam");
    snprintf(microphones[1].name, sizeof(microphones[1].name), "Headset microphone");
    snprintf(microphones[2].name, sizeof(microphones[2].name), "Virtual mixer");
    microphones[0].isDefault = 1;
    microphones[1].id.nullbackend = 123;
    playback[1].id.nullbackend = 456;
    ma_context context;
    ma_device_id id;
    char name[256];
    assert(fb_audio_device_resolve(&context, FB_CAPTURE_BOTH, "2", &id, name, sizeof(name)) == -1);
    assert(fb_audio_device_resolve(&context, FB_CAPTURE_MICROPHONE, "0", &id, name, sizeof(name)) == -1);
    assert(fb_audio_device_resolve(&context, FB_CAPTURE_MICROPHONE, "99", &id, name, sizeof(name)) == -1);
    assert(fb_audio_device_resolve(&context, FB_CAPTURE_MICROPHONE, "Removed USB device", &id, name, sizeof(name)) == -1);
    assert(fb_audio_device_resolve(&context, FB_CAPTURE_MICROPHONE, "2", &id, name, sizeof(name)) == 0);
    assert(id.nullbackend == 123 && strcmp(name, "Headset microphone") == 0);
    assert(fb_audio_device_resolve(&context, FB_CAPTURE_SYSTEM, "Headphones output", &id, name, sizeof(name)) == 0);
    assert(id.nullbackend == 456);
    assert(fb_audio_device_resolve(&context, FB_CAPTURE_MICROPHONE, "default", &id, name, sizeof(name)) == 0);
    assert(name[0] == 0);
    assert(fb_audio_device_resolve(&context, FB_CAPTURE_MICROPHONE, "2", &id, name, 4) == -1);
    snprintf(microphones[2].name, sizeof(microphones[2].name), "Headset microphone");
    assert(fb_audio_device_resolve(&context, FB_CAPTURE_MICROPHONE, "Headset microphone", &id, name, sizeof(name)) == -1);
    snprintf(microphones[2].name, sizeof(microphones[2].name), "Virtual mixer");
    FbAudioDeviceStatus status;
    wait_for_job(fb_audio_devices_start(FB_CAPTURE_MICROPHONE, "2"), &status);
    assert(status.success && strcmp(status.name, "Headset microphone") == 0);
    wait_for_job(fb_audio_devices_start(0, NULL), &status);
    assert(status.success && strstr(status.text, "2. Headset microphone"));
    assert(strstr(status.text, "1. [Windows default] Webcam"));
    assert(fb_audio_devices_list(name, sizeof(name)) == -1);
    fail_list = 1;
    wait_for_job(fb_audio_devices_start(FB_CAPTURE_MICROPHONE, "2"), &status);
    assert(!status.success && strstr(status.text, "unavailable"));
    fail_init = 1;
    wait_for_job(fb_audio_devices_start(0, NULL), &status);
    assert(!status.success);
    assert(!fb_audio_devices_start(FB_CAPTURE_BOTH, "2"));
    assert(!fb_audio_devices_start(FB_CAPTURE_MICROPHONE, NULL));
    return 0;
}
