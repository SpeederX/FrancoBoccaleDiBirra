#include <assert.h>
#include "audio/audio_mix.h"
int main(void) {
    float system[] = {0.2f, 1.0f, -1.0f};
    float mic[] = {0.6f, -1.0f, 1.0f};
    float mixed[3];
    assert(fb_audio_mix(mixed, NULL, NULL, 3) == -1);
    assert(fb_audio_mix(NULL, system, mic, 3) == -1);
    assert(fb_audio_mix(mixed, system, NULL, 3) == 0 && mixed[0] == 0.2f);
    assert(fb_audio_mix(mixed, NULL, mic, 3) == 0 && mixed[0] == 0.6f);
    assert(fb_audio_mix(mixed, system, mic, 3) == 0);
    assert(mixed[0] == 0.4f && mixed[1] == 0 && mixed[2] == 0);
    return 0;
}
