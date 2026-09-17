#ifndef FB_WAV_WRITER_H
#define FB_WAV_WRITER_H

#include <stddef.h>

/* Writes a mono 16-bit PCM RIFF/WAVE file. samples are float in [-1, 1]
   (clamped), converted to int16. Returns 0 on success. */
int fb_wav_write_file(const char *path, const float *samples, size_t count, int sample_rate);

#endif /* FB_WAV_WRITER_H */
