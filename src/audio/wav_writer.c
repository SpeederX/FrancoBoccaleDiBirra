#include "audio/wav_writer.h"

#include <stdint.h>
#include <stdio.h>

static void write_u32le(FILE *f, uint32_t v) {
    unsigned char b[4] = {(unsigned char)(v & 0xff), (unsigned char)((v >> 8) & 0xff),
                           (unsigned char)((v >> 16) & 0xff), (unsigned char)((v >> 24) & 0xff)};
    fwrite(b, 1, 4, f);
}

static void write_u16le(FILE *f, uint16_t v) {
    unsigned char b[2] = {(unsigned char)(v & 0xff), (unsigned char)((v >> 8) & 0xff)};
    fwrite(b, 1, 2, f);
}

int fb_wav_write_file(const char *path, const float *samples, size_t count, int sample_rate) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    uint32_t data_bytes = (uint32_t)(count * sizeof(int16_t));
    uint32_t byte_rate = (uint32_t)sample_rate * 1u * 16u / 8u;
    uint16_t block_align = (uint16_t)(1u * 16u / 8u);

    fwrite("RIFF", 1, 4, f);
    write_u32le(f, 36 + data_bytes);
    fwrite("WAVE", 1, 4, f);

    fwrite("fmt ", 1, 4, f);
    write_u32le(f, 16); /* PCM fmt chunk size */
    write_u16le(f, 1);  /* AudioFormat: PCM */
    write_u16le(f, 1);  /* NumChannels: mono */
    write_u32le(f, (uint32_t)sample_rate);
    write_u32le(f, byte_rate);
    write_u16le(f, block_align);
    write_u16le(f, 16); /* BitsPerSample */

    fwrite("data", 1, 4, f);
    write_u32le(f, data_bytes);

    for (size_t i = 0; i < count; ++i) {
        float s = samples[i];
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        int16_t v = (int16_t)(s * 32767.0f);
        write_u16le(f, (uint16_t)v);
    }

    fclose(f);
    return 0;
}
