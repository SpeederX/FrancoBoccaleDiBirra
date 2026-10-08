#include "audio/device_selection.h"
#include "platform/thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct FbAudioDeviceJob {
    FbThread *thread;
    FbMutex *mutex;
    FbCaptureSource source;
    char selection[256];
    FbAudioDeviceStatus status;
};

int fb_audio_device_resolve(ma_context *context, FbCaptureSource source, const char *selection,
                            ma_device_id *id, char *name, size_t capacity) {
    if (source != FB_CAPTURE_SYSTEM && source != FB_CAPTURE_MICROPHONE) {
        return -1;
    }
    if (!selection || !*selection || strcmp(selection, "default") == 0) {
        name[0] = '\0';
        return 0;
    }
    ma_device_info *playback, *microphones;
    ma_uint32 playback_count, microphone_count;
    if (ma_context_get_devices(context, &playback, &playback_count, &microphones, &microphone_count) != MA_SUCCESS) {
        return -1;
    }
    ma_device_info *devices = source == FB_CAPTURE_SYSTEM ? playback : microphones;
    ma_uint32 count = source == FB_CAPTURE_SYSTEM ? playback_count : microphone_count;
    char *end;
    long number = strtol(selection, &end, 10);
    int numbered = end != selection && !*end;
    int chosen = -1;
    for (ma_uint32 i = 0; i < count; ++i) {
        if ((numbered && number == (long)i + 1) || (!numbered && strcmp(selection, devices[i].name) == 0)) {
            if (chosen >= 0) {
                return -1;
            }
            chosen = (int)i;
        }
    }
    if (chosen < 0 || strlen(devices[chosen].name) >= capacity) {
        return -1;
    }
    *id = devices[chosen].id;
    snprintf(name, capacity, "%s", devices[chosen].name);
    return 0;
}

int fb_audio_devices_list(char *out, size_t capacity) {
    ma_context context;
    if (ma_context_init(NULL, 0, NULL, &context) != MA_SUCCESS) {
        return -1;
    }
    ma_device_info *playback, *microphones;
    ma_uint32 playback_count, microphone_count;
    if (ma_context_get_devices(&context, &playback, &playback_count, &microphones, &microphone_count) != MA_SUCCESS) {
        ma_context_uninit(&context);
        return -1;
    }
    size_t used = (size_t)snprintf(out, capacity,
        "Select /meeting source system|microphone|both\n"
        "Choose /meeting microphone <number or full name> and /meeting output <number or full name>.\n"
        "Use default to follow Windows settings. A selected device name is saved.\n\nSystem output devices:\n");
    for (ma_uint32 i = 0; i < playback_count && used < capacity; ++i) {
        used += (size_t)snprintf(out + used, capacity - used, "%u. %s%s\n",
            i + 1, playback[i].isDefault ? "[Windows default] " : "", playback[i].name);
    }
    if (used < capacity) {
        used += (size_t)snprintf(out + used, capacity - used, "\nMicrophones:\n");
    }
    for (ma_uint32 i = 0; i < microphone_count && used < capacity; ++i) {
        used += (size_t)snprintf(out + used, capacity - used, "%u. %s%s\n",
            i + 1, microphones[i].isDefault ? "[Windows default] " : "", microphones[i].name);
    }
    ma_context_uninit(&context);
    return used < capacity ? 0 : -1;
}

static void device_worker(void *arg) {
    FbAudioDeviceJob *job = arg;
    FbAudioDeviceStatus status = {0};
    if (!job->source) {
        status.success = fb_audio_devices_list(status.text, sizeof(status.text)) == 0;
    } else {
        ma_context context;
        if (ma_context_init(NULL, 0, NULL, &context) == MA_SUCCESS) {
            ma_device_id id;
            status.success = fb_audio_device_resolve(&context, job->source, job->selection,
                &id, status.name, sizeof(status.name)) == 0;
            ma_context_uninit(&context);
        }
    }
    if (!status.success) {
        snprintf(status.text, sizeof(status.text), "device unavailable or ambiguous; /meeting sources lists current devices");
    }
    status.done = 1;
    fb_mutex_lock(job->mutex);
    job->status = status;
    fb_mutex_unlock(job->mutex);
}

FbAudioDeviceJob *fb_audio_devices_start(FbCaptureSource source, const char *selection) {
    if ((source && source != FB_CAPTURE_SYSTEM && source != FB_CAPTURE_MICROPHONE) ||
        (source && (!selection || strlen(selection) >= 256))) {
        return NULL;
    }
    FbAudioDeviceJob *job = calloc(1, sizeof(*job));
    if (!job) {
        return NULL;
    }
    job->source = source;
    snprintf(job->selection, sizeof(job->selection), "%s", selection ? selection : "");
    job->mutex = fb_mutex_create();
    if (!job->mutex) {
        free(job);
        return NULL;
    }
    job->thread = fb_thread_start(device_worker, job);
    if (!job->thread) {
        fb_mutex_free(job->mutex);
        free(job);
        return NULL;
    }
    return job;
}

void fb_audio_devices_poll(FbAudioDeviceJob *job, FbAudioDeviceStatus *status) {
    fb_mutex_lock(job->mutex);
    *status = job->status;
    fb_mutex_unlock(job->mutex);
}

void fb_audio_devices_free(FbAudioDeviceJob *job) {
    if (job) {
        fb_thread_join(job->thread);
        fb_mutex_free(job->mutex);
        free(job);
    }
}
