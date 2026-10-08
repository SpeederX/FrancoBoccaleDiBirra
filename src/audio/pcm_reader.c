#include "audio/pcm_reader.h"
#include <string.h>

static uint32_t u32(const unsigned char *data) {
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
           (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

int fb_pcm_reader_open(FbPcmReader *reader, const char *path, int live) {
    memset(reader, 0, sizeof(*reader));
    reader->file = fopen(path, "rb");
    if (!reader->file) {
        return -1;
    }
    unsigned char header[16];
    if (fread(header, 1, 12, reader->file) != 12 ||
        memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4)) {
        goto invalid;
    }
    int format_seen = 0;
    while (fread(header, 1, 8, reader->file) == 8) {
        uint32_t bytes = u32(header + 4);
        if (!memcmp(header, "fmt ", 4)) {
            if (bytes < 16 || fread(header, 1, 16, reader->file) != 16 ||
                header[0] != 1 || header[1] != 0 || header[2] != 1 || header[3] != 0 ||
                u32(header + 4) != 16000 || header[14] != 16 || header[15] != 0) {
                goto invalid;
            }
            bytes -= 16;
            format_seen = 1;
        } else if (!memcmp(header, "data", 4)) {
            if (!format_seen || bytes % 2) {
                goto invalid;
            }
            reader->remaining = bytes / 2;
            reader->live = live;
            return 0;
        }
        if (bytes > 0x7fffffffu || fseek(reader->file, (long)(bytes + (bytes & 1u)), SEEK_CUR) != 0) {
            goto invalid;
        }
    }
invalid:
    fb_pcm_reader_close(reader);
    return -1;
}

int fb_pcm_reader_read(FbPcmReader *reader, float *samples, size_t capacity) {
    if (!reader->file) {
        return -1;
    }
    if (!reader->live && capacity > reader->remaining) {
        capacity = (size_t)reader->remaining;
    }
    clearerr(reader->file);
    unsigned char bytes[8192];
    size_t total = 0;
    while (total < capacity) {
        size_t count = capacity - total;
        if (count > sizeof(bytes) / 2) {
            count = sizeof(bytes) / 2;
        }
        size_t got = fread(bytes, 2, count, reader->file);
        for (size_t i = 0; i < got; ++i) {
            int16_t pcm = (int16_t)((uint16_t)bytes[i * 2] | (uint16_t)bytes[i * 2 + 1] << 8);
            samples[total + i] = (float)pcm / 32768.0f;
        }
        total += got;
        if (got < count) {
            break;
        }
    }
    if (!reader->live) {
        reader->remaining -= total;
    }
    return ferror(reader->file) ? -1 : (int)total;
}

void fb_pcm_reader_close(FbPcmReader *reader) {
    if (reader->file) {
        fclose(reader->file);
        reader->file = NULL;
    }
}
