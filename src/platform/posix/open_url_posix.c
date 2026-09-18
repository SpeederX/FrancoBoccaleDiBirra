#include "platform/open_url.h"

#include <unistd.h>

void fb_open_in_browser(const char *path) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        /* New session: detach from this (short-lived CLI) process's
           terminal/lifetime entirely, so the opener/browser survives us
           exiting. */
        setsid();
#ifdef __APPLE__
        execlp("open", "open", path, (char *)NULL);
#else
        execlp("xdg-open", "xdg-open", path, (char *)NULL);
#endif
        _exit(127); /* exec failed */
    }
    /* Parent: deliberately don't waitpid -- that would block until the
       opened browser itself exits. */
}
