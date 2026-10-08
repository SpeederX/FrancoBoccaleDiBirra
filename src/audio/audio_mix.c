#include "audio/audio_mix.h"
int fb_audio_mix(float *output, const float *system, const float *microphone, size_t frames) {
    if (!output || (!system && !microphone)) {
        return -1;
    }
    for (size_t i = 0; i < frames; ++i) {
        output[i] = system && microphone ? (system[i] + microphone[i]) * 0.5f
                     : system ? system[i] : microphone[i];
    }
    return 0;
}
