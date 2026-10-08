#ifndef FB_WAV_STREAM_H
#define FB_WAV_STREAM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    FILE *file;
    uint32_t frames;
    int sample_rate;
    int failed;
} FbWavStream;

int fb_wav_stream_open(FbWavStream *stream, const char *path, int sample_rate);
int fb_wav_stream_append(FbWavStream *stream, const float *samples, size_t count);
int fb_wav_stream_close(FbWavStream *stream);

#endif
