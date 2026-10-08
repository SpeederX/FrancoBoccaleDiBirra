#ifndef FB_MEETING_CLI_H
#define FB_MEETING_CLI_H

#include "config/config.h"

int fb_meeting_cli(int argc, char **argv);
int fb_meeting_launch_config(int argc, char **argv, FbConfig *config);

#endif
