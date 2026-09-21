#include "calibr/calibr_paths.h"

#include <stdio.h>

int fb_calibr_root(const FbConfig *cfg, char *out, size_t cap) {
    if (cfg->calibr_root[0] == '\0') return -1;
    int n = snprintf(out, cap, "%s", cfg->calibr_root);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}
