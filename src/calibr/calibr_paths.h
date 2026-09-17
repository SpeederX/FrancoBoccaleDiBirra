#ifndef FB_CALIBR_PATHS_H
#define FB_CALIBR_PATHS_H

#include <stddef.h>

/* Resolves calibr's root directory (containing its results/, log/, state/
   trees) from the FB_CALIBR_ROOT env var — there's no reliable way to
   auto-discover an arbitrary calibr checkout or install, so this is
   explicit config, same pattern as FB_LLAMA_SERVER/FB_SHERPA_ONNX_EXE/
   FB_ORUKEET_DIR. Returns 0 if set, -1 otherwise (views must treat that as
   "not configured yet", not a parse error). */
int fb_calibr_root(char *out, size_t cap);

#endif /* FB_CALIBR_PATHS_H */
