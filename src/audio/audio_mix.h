#ifndef FB_AUDIO_MIX_H
#define FB_AUDIO_MIX_H
#include <stddef.h>
int fb_audio_mix(float *output, const float *system, const float *microphone, size_t frames);
#endif
