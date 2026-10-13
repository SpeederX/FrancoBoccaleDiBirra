#include "test_check.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "audio/wav_writer.h"

static uint32_t read_u32le(FILE *f) {
    unsigned char b[4];
    fread(b, 1, 4, f);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint16_t read_u16le(FILE *f) {
    unsigned char b[2];
    fread(b, 1, 2, f);
    return (uint16_t)(b[0] | (b[1] << 8));
}

int main(void) {
    const char *path = "test_wav_writer_output.wav";
    const int sample_rate = 16000;
    const size_t count = 100;

    float samples[100];
    for (size_t i = 0; i < count; ++i) {
        samples[i] = sinf((float)i * 0.2f);
    }

    FB_CHECK(fb_wav_write_file(path, samples, count, sample_rate) == 0);

    FILE *f = fopen(path, "rb");
    FB_CHECK(f != NULL);

    char riff[4], wave[4], fmt[4], data[4];
    fread(riff, 1, 4, f);
    FB_CHECK(riff[0] == 'R' && riff[1] == 'I' && riff[2] == 'F' && riff[3] == 'F');
    uint32_t chunk_size = read_u32le(f);
    fread(wave, 1, 4, f);
    FB_CHECK(wave[0] == 'W' && wave[1] == 'A' && wave[2] == 'V' && wave[3] == 'E');

    fread(fmt, 1, 4, f);
    FB_CHECK(fmt[0] == 'f' && fmt[1] == 'm' && fmt[2] == 't' && fmt[3] == ' ');
    uint32_t fmt_size = read_u32le(f);
    FB_CHECK(fmt_size == 16);
    uint16_t audio_format = read_u16le(f);
    FB_CHECK(audio_format == 1); /* PCM */
    uint16_t num_channels = read_u16le(f);
    FB_CHECK(num_channels == 1);
    uint32_t rate = read_u32le(f);
    FB_CHECK(rate == (uint32_t)sample_rate);
    read_u32le(f); /* byte rate, not re-checked */
    read_u16le(f); /* block align, not re-checked */
    uint16_t bits_per_sample = read_u16le(f);
    FB_CHECK(bits_per_sample == 16);

    fread(data, 1, 4, f);
    FB_CHECK(data[0] == 'd' && data[1] == 'a' && data[2] == 't' && data[3] == 'a');
    uint32_t data_bytes = read_u32le(f);
    FB_CHECK(data_bytes == count * sizeof(int16_t));
    FB_CHECK(chunk_size == 36 + data_bytes);

    for (size_t i = 0; i < count; ++i) {
        int16_t v = (int16_t)read_u16le(f);
        float back = (float)v / 32767.0f;
        FB_CHECK(fabsf(back - samples[i]) < 0.001f); /* int16 quantization tolerance */
    }

    fclose(f);
    remove(path);

    printf("test_wav_writer: OK\n");
    return 0;
}
