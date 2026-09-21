#ifndef FB_CALIBR_PATHS_H
#define FB_CALIBR_PATHS_H

#include <stddef.h>

#include "config/config.h"

/* Resolves calibr's root directory (containing its results/, log/, state/
   trees) from cfg->calibr_root (config.json) -- there's no reliable way to
   auto-discover an arbitrary calibr checkout or install, so this is
   explicit config, same pattern as qwen_model. Returns 0 if set, -1
   otherwise (views must treat that as "not configured yet", not a parse
   error). */
int fb_calibr_root(const FbConfig *cfg, char *out, size_t cap);

#endif /* FB_CALIBR_PATHS_H */
