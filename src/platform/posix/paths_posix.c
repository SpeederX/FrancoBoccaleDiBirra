/* Not yet runtime-tested (development happened on Windows this pass); it
   must be exercised on a real Linux/macOS machine before being relied on. */

#include "util/paths.h"

#include <stdio.h>

int fb_replace_file(const char *source, const char *destination) {
    return rename(source, destination);
}

#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

int fb_get_executable_dir(char *out, size_t cap) {
#ifdef __APPLE__
    uint32_t size = (uint32_t)cap;
    if (_NSGetExecutablePath(out, &size) != 0) {
        return -1;
    }
#else
    ssize_t length = readlink("/proc/self/exe", out, cap - 1);
    if (length < 0 || (size_t)length >= cap - 1) {
        return -1;
    }
    out[length] = '\0';
#endif
    char *last = strrchr(out, '/');
    if (!last) {
        return -1;
    }
    *last = '\0';
    return 0;
}

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
