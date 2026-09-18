#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/app.h"
#include "app/sweep.h"
#include "net/sockets_compat.h"
#include "platform/platform.h"
#include "tui/screen.h"

int main(int argc, char **argv) {
    fb_sock_platform_init();

    /* Dev-only batch mode -- see sweep.h. Never touches the TUI/screen at
       all; runs, writes its report, exits. */
    if (argc > 1 && strcmp(argv[1], "--sweep-summarize") == 0) {
        int rc = fb_sweep_run(argc - 2, argv + 2);
        fb_sock_platform_cleanup();
        return rc;
    }

    if (fb_screen_init() != 0) {
        fprintf(stderr, "failed to initialize the terminal screen\n");
        fb_sock_platform_cleanup();
        return 1;
    }

    /* Heap-allocated, not a stack local: FbApp embeds fixed-size text-view
       buffers (calibr leaderboard/log views) that add up to over a
       megabyte, comfortably past a typical 1MB default thread stack. */
    FbApp *app = (FbApp *)malloc(sizeof(FbApp));
    if (!app) {
        fprintf(stderr, "out of memory\n");
        fb_screen_shutdown();
        fb_sock_platform_cleanup();
        return 1;
    }
    fb_app_init(app, fb_screen_width(), fb_screen_height());

    while (app->running) {
        FbEvent ev;
        int got = fb_screen_poll_event(&ev, 33); /* ~30fps cap */
        if (got < 0) break;
        if (got == 1) fb_app_handle_event(app, &ev);

        fb_app_tick(app);
        fb_screen_clear();
        fb_app_render(app);
        fb_screen_present();
    }

    fb_app_shutdown(app);
    free(app);
    fb_screen_shutdown();
    fb_sock_platform_cleanup();
    return 0;
}
