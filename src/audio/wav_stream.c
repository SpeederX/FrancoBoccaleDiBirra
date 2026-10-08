#include "audio/wav_stream.h"

#include <math.h>
#include <string.h>
#include "audio/pcm_reader.h"

static void u32le(unsigned char *out, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out[i] = (unsigned char)(value >> (i * 8));
    }
}

static int write_header(FbWavStream *stream) {
    unsigned char header[44] = {0};
    memcpy(header, "RIFF", 4);
    u32le(header + 4, 36 + stream->frames * 2);
    memcpy(header + 8, "WAVEfmt ", 8);
    u32le(header + 16, 16);
    header[20] = 1;
    header[22] = 1;
    u32le(header + 24, (uint32_t)stream->sample_rate);
    u32le(header + 28, (uint32_t)stream->sample_rate * 2);
    header[32] = 2;
    header[34] = 16;
    memcpy(header + 36, "data", 4);
    u32le(header + 40, stream->frames * 2);
    return fwrite(header, 1, sizeof(header), stream->file) == sizeof(header) ? 0 : -1;
}

int fb_wav_stream_open(FbWavStream *stream, const char *path, int sample_rate) {
    memset(stream, 0, sizeof(*stream));
    if (sample_rate <= 0 || !path || !path[0]) {
        return -1;
    }
    stream->sample_rate = sample_rate;
    stream->file = fopen(path, "wb");
    if (!stream->file) {
        return -1;
    }
    if (write_header(stream) != 0) {
        fclose(stream->file);
        stream->file = NULL;
        return -1;
    }
    return 0;
}

int fb_wav_stream_append(FbWavStream *stream, const float *samples, size_t count) {
    if (!stream->file || stream->failed || (!samples && count > 0) ||
        count > (UINT32_MAX - 36u) / 2u - stream->frames) {
        stream->failed = 1;
        return -1;
    }
    unsigned char pcm[8192];
    size_t offset = 0;
    while (offset < count) {
        size_t take = count - offset;
        if (take > sizeof(pcm) / 2) {
            take = sizeof(pcm) / 2;
        }
        for (size_t i = 0; i < take; ++i) {
            float value = samples[offset + i];
            if (!isfinite(value)) {
                stream->failed = 1;
                return -1;
            }
            if (value > 1.0f) {
                value = 1.0f;
            }
            if (value < -1.0f) {
                value = -1.0f;
            }
            uint16_t sample = (uint16_t)(int16_t)(value * 32767.0f);
            pcm[i * 2] = (unsigned char)sample;
            pcm[i * 2 + 1] = (unsigned char)(sample >> 8);
        }
        if (fwrite(pcm, 2, take, stream->file) != take) {
            stream->failed = 1;
            return -1;
        }
        stream->frames += (uint32_t)take;
        offset += take;
    }
    return fflush(stream->file) == 0 ? 0 : -1;
}

int fb_wav_stream_resume(FbWavStream *stream, const char *path) {
    FbPcmReader reader;
    if (fb_pcm_reader_open(&reader, path, 0) != 0 || ftell(reader.file) != 44) {
        fb_pcm_reader_close(&reader);
        return -1;
    }
    uint64_t frames = reader.remaining;
    fb_pcm_reader_close(&reader);
    memset(stream, 0, sizeof(*stream));
    stream->file = fopen(path, "rb+");
    if (!stream->file) {
        return -1;
    }
    stream->sample_rate = 16000;
    stream->frames = (uint32_t)frames;
    if (fseek(stream->file, 0, SEEK_END) != 0 || ftell(stream->file) != 44 + (long long)frames * 2) {
        fclose(stream->file);
        stream->file = NULL;
        return -1;
    }
    return 0;
}

int fb_wav_stream_close(FbWavStream *stream) {
    if (!stream->file) {
        return -1;
    }
    int result = stream->failed ? -1 : 0;
    if (fseek(stream->file, 0, SEEK_SET) != 0 || write_header(stream) != 0) {
        result = -1;
    }
    if (fclose(stream->file) != 0) {
        result = -1;
    }
    stream->file = NULL;
    return result;
}
