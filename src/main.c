#include <stdio.h>
#include <stdlib.h>

#include "app/app.h"
#include "net/sockets_compat.h"
#include "platform/platform.h"
#include "tui/screen.h"

int main(void) {
    fb_sock_platform_init();

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
