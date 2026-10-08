#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio/wav_stream.h"

int main(void) {
    FbWavStream stream;
    assert(fb_wav_stream_open(&stream, "missing-dir/audio.wav", 16000) == -1);
    assert(fb_wav_stream_open(&stream, "stream.wav", 0) == -1);
    assert(fb_wav_stream_append(&stream, NULL, 1) == -1);
    assert(fb_wav_stream_close(&stream) == -1);
    assert(fb_wav_stream_open(&stream, "stream.wav", 16000) == 0);
    const float samples[] = {-2.0f, 0.0f, 2.0f};
    for (int i = 0; i < 100; ++i) {
        assert(fb_wav_stream_append(&stream, samples, 3) == 0);
    }
    assert(fb_wav_stream_close(&stream) == 0);
    FILE *file = fopen("stream.wav", "rb");
    assert(file);
    unsigned char bytes[650];
    size_t size = fread(bytes, 1, sizeof(bytes), file);
    fclose(file);
    assert(size == 644);
    assert(memcmp(bytes, "RIFF", 4) == 0);
    assert(bytes[40] == 88 && bytes[41] == 2);
    assert(bytes[44] == 1 && bytes[45] == 128);
    assert(bytes[46] == 0 && bytes[47] == 0);
    assert(bytes[48] == 255 && bytes[49] == 127);
    assert(fb_wav_stream_open(&stream, "stream.wav", 16000) == 0);
    float invalid[] = {NAN};
    assert(fb_wav_stream_append(&stream, invalid, 1) == -1);
    assert(fb_wav_stream_close(&stream) == -1);
    remove("stream.wav");
    return 0;
}
