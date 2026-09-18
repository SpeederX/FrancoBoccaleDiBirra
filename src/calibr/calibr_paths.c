#include "calibr/calibr_paths.h"

#include <stdio.h>
#include <stdlib.h>

int fb_calibr_root(const FbConfig *cfg, char *out, size_t cap) {
    const char *env = getenv("FB_CALIBR_ROOT");
    const char *src = (env && env[0] != '\0') ? env
                       : (cfg->calibr_root[0] != '\0') ? cfg->calibr_root
                                                        : NULL;
    if (!src) return -1;
    int n = snprintf(out, cap, "%s", src);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}
