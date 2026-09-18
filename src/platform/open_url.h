#ifndef FB_OPEN_URL_H
#define FB_OPEN_URL_H

/* Opens `path` (a local file path or a URL) in the user's default
   application -- used to pop the --sweep-summarize HTML report open in a
   browser once it's written. Fire-and-forget and fully detached: the
   opened process is deliberately *not* spawned via platform/proc.h, which
   tracks and kill-on-closes everything it spawns (a Job Object on Windows,
   a process group on POSIX) -- exactly what would make the browser vanish
   the moment this short-lived CLI process exits, which defeats the point.
   Best-effort; a failure here has nothing actionable to do about it beyond
   what the caller already does (print the path directly), so it's silent. */
void fb_open_in_browser(const char *path);

#endif /* FB_OPEN_URL_H */
