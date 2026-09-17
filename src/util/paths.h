#ifndef FB_PATHS_H
#define FB_PATHS_H

#include <stddef.h>

/* Resolves (and creates if missing) this app's per-user data directory:
   %LOCALAPPDATA%\FrancoBoccaleDiBirra on Windows,
   $XDG_DATA_HOME/FrancoBoccaleDiBirra (or ~/.local/share/FrancoBoccaleDiBirra)
   on POSIX — mirrors calibr's own installed-package convention. Returns 0
   on success, writing the path into out (truncated safely to cap). */
int fb_get_data_dir(char *out, size_t cap);

/* %USERPROFILE% on Windows, $HOME on POSIX. Returns 0 on success. */
int fb_get_home_dir(char *out, size_t cap);

/* Creates a directory if it doesn't already exist. Returns 0 on success
   (including "already existed"). */
int fb_ensure_dir(const char *path);

#endif /* FB_PATHS_H */
