#ifndef FB_PCM_READER_H
#define FB_PCM_READER_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    FILE *file;
    uint64_t remaining;
    int live;
} FbPcmReader;

int fb_pcm_reader_open(FbPcmReader *reader, const char *path, int live);
int fb_pcm_reader_read(FbPcmReader *reader, float *samples, size_t capacity);
void fb_pcm_reader_close(FbPcmReader *reader);

#endif
