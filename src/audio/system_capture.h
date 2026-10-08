#ifndef FB_SYSTEM_CAPTURE_H
#define FB_SYSTEM_CAPTURE_H

#include <stddef.h>

typedef struct FbSystemCapture FbSystemCapture;
typedef enum {
    FB_CAPTURE_SYSTEM = 1,
    FB_CAPTURE_MICROPHONE = 2,
    FB_CAPTURE_BOTH = 3
} FbCaptureSource;

typedef enum {
    FB_CAPTURE_STARTING,
    FB_CAPTURE_RECORDING,
    FB_CAPTURE_STOPPING,
    FB_CAPTURE_DONE,
    FB_CAPTURE_ERROR
} FbCaptureState;

typedef struct {
    FbCaptureState state;
    unsigned long long frames;
    unsigned long long dropped_frames;
    char device_name[256];
    char error[256];
} FbCaptureStatus;

FbSystemCapture *fb_system_capture_start(const char *path);
FbSystemCapture *fb_system_capture_start_source(const char *path, FbCaptureSource source, int resume);
FbSystemCapture *fb_system_capture_start_devices(const char *path, FbCaptureSource source, int resume,
                                                const char *output_device, const char *microphone);
int fb_system_capture_sources(char *out, size_t capacity);
void fb_system_capture_stop(FbSystemCapture *capture);
void fb_system_capture_poll(FbSystemCapture *capture, FbCaptureStatus *status);
void fb_system_capture_free(FbSystemCapture *capture);

#endif
