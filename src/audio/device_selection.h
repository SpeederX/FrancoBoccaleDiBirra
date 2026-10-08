#ifndef FB_DEVICE_SELECTION_H
#define FB_DEVICE_SELECTION_H

#include "miniaudio.h"
#include "audio/system_capture.h"

typedef struct FbAudioDeviceJob FbAudioDeviceJob;

typedef struct {
    int done;
    int success;
    char name[256];
    char text[16384];
} FbAudioDeviceStatus;

int fb_audio_device_resolve(ma_context *context, FbCaptureSource source, const char *selection,
                            ma_device_id *id, char *name, size_t capacity);
int fb_audio_devices_list(char *out, size_t capacity);
FbAudioDeviceJob *fb_audio_devices_start(FbCaptureSource source, const char *selection);
void fb_audio_devices_poll(FbAudioDeviceJob *job, FbAudioDeviceStatus *status);
void fb_audio_devices_free(FbAudioDeviceJob *job);

#endif
