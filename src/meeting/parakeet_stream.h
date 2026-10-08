#ifndef FB_PARAKEET_STREAM_H
#define FB_PARAKEET_STREAM_H
#include <stddef.h>
typedef struct FbParakeetStream FbParakeetStream;
FbParakeetStream *fb_parakeet_stream_open(const char *library, const char *asr, const char *diar,
                                         char *error, size_t capacity);
char *fb_parakeet_stream_feed(FbParakeetStream *stream, const float *pcm, int frames, int final,
                              char *error, size_t capacity);
void fb_parakeet_stream_free(FbParakeetStream *stream);
#endif
