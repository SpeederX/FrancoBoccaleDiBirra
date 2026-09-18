#ifndef FB_UTF8_H
#define FB_UTF8_H

#include <stddef.h>
#include <stdint.h>

/* Decodes one UTF-8 codepoint starting at s (NUL-terminated). Returns the
   number of bytes consumed (1-4) and writes the decoded codepoint to
   *out_cp. An invalid or truncated lead byte decodes as itself (Latin-1
   passthrough) and consumes 1 byte -- there is no corrupted-input recovery
   story here beyond "don't crash, don't read past the terminator": the
   length checks below short-circuit at the first NUL byte, which is always
   in bounds for a NUL-terminated buffer. */
size_t fb_utf8_decode(const char *s, uint32_t *out_cp);

/* Encodes cp into out (which must have room for at least 4 bytes). Returns
   the number of bytes written (1-4). */
size_t fb_utf8_encode(uint32_t cp, char *out);

/* Byte-length of the UTF-8 sequence ending at buf[pos - 1] (buf[0..pos) must
   be valid UTF-8, or at least not disagree with continuation-byte shape).
   Used to step a cursor back by a whole codepoint instead of a lone
   continuation byte. pos == 0 returns 0. */
size_t fb_utf8_prev_len(const char *buf, size_t pos);

#endif /* FB_UTF8_H */
