#include "util/utf8.h"

size_t fb_utf8_decode(const char *s, uint32_t *out_cp) {
    unsigned char c0 = (unsigned char)s[0];

    if (c0 < 0x80) {
        *out_cp = c0;
        return 1;
    }
    if ((c0 & 0xE0) == 0xC0 && ((unsigned char)s[1] & 0xC0) == 0x80) {
        *out_cp = ((uint32_t)(c0 & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
        return 2;
    }
    if ((c0 & 0xF0) == 0xE0 && ((unsigned char)s[1] & 0xC0) == 0x80 &&
        ((unsigned char)s[2] & 0xC0) == 0x80) {
        *out_cp = ((uint32_t)(c0 & 0x0F) << 12) |
                  ((uint32_t)((unsigned char)s[1] & 0x3F) << 6) |
                  ((unsigned char)s[2] & 0x3F);
        return 3;
    }
    if ((c0 & 0xF8) == 0xF0 && ((unsigned char)s[1] & 0xC0) == 0x80 &&
        ((unsigned char)s[2] & 0xC0) == 0x80 && ((unsigned char)s[3] & 0xC0) == 0x80) {
        *out_cp = ((uint32_t)(c0 & 0x07) << 18) |
                  ((uint32_t)((unsigned char)s[1] & 0x3F) << 12) |
                  ((uint32_t)((unsigned char)s[2] & 0x3F) << 6) |
                  ((unsigned char)s[3] & 0x3F);
        return 4;
    }

    *out_cp = c0;
    return 1;
}

size_t fb_utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

size_t fb_utf8_prev_len(const char *buf, size_t pos) {
    if (pos == 0) return 0;

    size_t start = pos - 1;
    size_t back = 0;
    while (start > 0 && ((unsigned char)buf[start] & 0xC0) == 0x80 && back < 3) {
        start--;
        back++;
    }
    return pos - start;
}
