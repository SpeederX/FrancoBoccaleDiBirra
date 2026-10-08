#ifndef FB_FILEUTIL_H
#define FB_FILEUTIL_H

#include <stddef.h>

/* Reads an entire file into a malloc'd, NUL-terminated buffer (the NUL is
   extra, not counted in *out_len). Returns NULL if the file doesn't exist
   or can't be read — this is the normal, non-error case for "no config/
   results yet", not just a failure path. Caller frees the result. */
char *fb_read_file(const char *path, size_t *out_len);
int fb_write_file_atomic(const char *path, const char *text);

#endif /* FB_FILEUTIL_H */
