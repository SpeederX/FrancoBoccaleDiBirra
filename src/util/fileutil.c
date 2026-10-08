#include "util/fileutil.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "util/paths.h"

int fb_write_file_atomic(const char *path, const char *text) {
    size_t size = strlen(path) + 5;
    char *temporary = malloc(size);
    if (!temporary) {
        return -1;
    }
    snprintf(temporary, size, "%s.tmp", path);
    FILE *file = fopen(temporary, "wb");
    if (!file) {
        free(temporary);
        return -1;
    }
    size_t length = strlen(text);
    int result = fwrite(text, 1, length, file) == length ? 0 : -1;
    if (fclose(file) != 0) {
        result = -1;
    }
    if (result == 0) {
        result = fb_replace_file(temporary, path);
    }
    if (result != 0) {
        remove(temporary);
    }
    free(temporary);
    return result;
}

char *fb_read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[read] = '\0';

    if (out_len) *out_len = read;
    return buf;
}
