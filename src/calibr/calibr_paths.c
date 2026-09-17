#include "calibr/calibr_paths.h"

#include <stdio.h>
#include <stdlib.h>

int fb_calibr_root(char *out, size_t cap) {
    const char *env = getenv("FB_CALIBR_ROOT");
    if (!env || env[0] == '\0') return -1;
    int n = snprintf(out, cap, "%s", env);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}
