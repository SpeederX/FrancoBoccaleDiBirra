#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/app.h"
#include "app/sweep.h"
#include "meeting/meeting_cli.h"
#include "net/sockets_compat.h"
#include "platform/platform.h"
#include "tui/screen.h"

int main(int argc, char **argv) {
    fb_sock_platform_init();
    FbConfig launch_config = {0};
    int meeting_launch = fb_meeting_launch_config(argc, argv, &launch_config);
    if (meeting_launch < 0) {
        fb_sock_platform_cleanup();
        return 2;
    }
    if (argc > 1 && strncmp(argv[1], "--meeting-", 10) == 0) {
        int rc = fb_meeting_cli(argc, argv);
        fb_sock_platform_cleanup();
        return rc;
    }

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
    fb_app_init_mode(app, fb_screen_width(), fb_screen_height(),
        meeting_launch ? FB_INPUT_MODE_MEETING : FB_INPUT_MODE_COMMAND);
    if (meeting_launch) {
        fb_meeting_launch_config(argc, argv, &app->config);
        fb_meeting_shutdown(&app->meeting);
        fb_meeting_init(&app->meeting, &app->config);
        app->input_mode = FB_INPUT_MODE_MEETING;
        fb_meeting_command(&app->meeting, "help");
    }

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
