/* Not yet runtime-tested (development happened on Windows this pass); it
   must be exercised on a real Linux/macOS machine before being relied on. */

#include "util/paths.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

int fb_ensure_dir(const char *path) {
    if (mkdir(path, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

int fb_get_home_dir(char *out, size_t cap) {
    const char *home = getenv("HOME");
    if (!home) return -1;
    int n = snprintf(out, cap, "%s", home);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

int fb_get_data_dir(char *out, size_t cap) {
    char base[1024];
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0] != '\0') {
        snprintf(base, sizeof(base), "%s", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home) return -1;
        snprintf(base, sizeof(base), "%s/.local/share", home);
    }

    int written = snprintf(out, cap, "%s/FrancoBoccaleDiBirra", base);
    if (written < 0 || (size_t)written >= cap) return -1;

    return fb_ensure_dir(out);
}
