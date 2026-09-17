#ifndef FB_PROC_H
#define FB_PROC_H

/*
 * Cross-platform process spawn/track/stop, chosen by CMake source-list
 * selection (platform/win/proc_win.c vs platform/posix/proc_posix.c) —
 * same pattern as tui/screen.h. Every spawned process is placed in a
 * container (Windows: a Job Object with kill-on-close; POSIX: its own
 * process group) so closing the console never leaves an orphan.
 */

typedef struct FbProcess FbProcess; /* opaque, platform-defined */

/* argv is a NULL-terminated array; argv[0] is the executable path.
   stdout_path/stderr_path, if non-NULL, capture the child's stdout/stderr
   to separate files (truncating any existing file) — the only way to see
   why a spawn failed, since nothing else surfaces that. They must be two
   distinct paths (or at least two distinct handles): a child's stdout and
   stderr are independently buffered by its own CRT and flush to the OS at
   different times, so pointing both at the very same file handle produces
   a torn/interleaved file rather than two coherent streams — confirmed the
   hard way with sherpa-onnx-offline, whose large buffered JSON result on
   stdout got sliced apart by stderr's immediately-flushed status lines. */
FbProcess *fb_proc_spawn(const char *const *argv, const char *stdout_path,
                          const char *stderr_path);

/* Terminates the whole process tree. Safe to call more than once. */
void fb_proc_stop(FbProcess *proc);

/* Non-blocking. Returns 1 if the process has exited, 0 if still running. */
int fb_proc_has_exited(FbProcess *proc);

long fb_proc_pid(const FbProcess *proc);

/* Releases bookkeeping handles; does not stop the process. */
void fb_proc_free(FbProcess *proc);

#endif /* FB_PROC_H */
