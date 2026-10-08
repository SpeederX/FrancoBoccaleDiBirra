#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio/wav_stream.h"
#include "audio/pcm_reader.h"

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
    assert(fb_wav_stream_resume(&stream, "missing.wav") == -1);
    assert(fb_wav_stream_resume(&stream, "stream.wav") == 0);
    assert(stream.frames == 300);
    FbPcmReader reader;
    assert(fb_pcm_reader_open(&reader, "stream.wav", 1) == 0);
    float readback[310];
    assert(fb_pcm_reader_read(&reader, readback, 310) == 300);
    assert(fb_wav_stream_append(&stream, samples, 3) == 0);
    assert(fb_pcm_reader_read(&reader, readback, 310) == 3);
    assert(readback[0] < -0.99f && readback[1] == 0 && readback[2] > 0.99f);
    fb_pcm_reader_close(&reader);
    assert(fb_wav_stream_close(&stream) == 0);
    assert(fb_pcm_reader_open(&reader, "stream.wav", 0) == 0);
    assert(reader.remaining == 303);
    assert(fb_pcm_reader_read(&reader, readback, 310) == 303);
    assert(fb_pcm_reader_read(&reader, readback, 310) == 0);
    fb_pcm_reader_close(&reader);
    assert(fb_wav_stream_open(&stream, "stream.wav", 16000) == 0);
    float invalid[] = {NAN};
    assert(fb_wav_stream_append(&stream, invalid, 1) == -1);
    assert(fb_wav_stream_close(&stream) == -1);
    remove("stream.wav");
    return 0;
}
