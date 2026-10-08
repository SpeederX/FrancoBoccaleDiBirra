#include <stdio.h>
#include <string.h>
#include "platform/platform.h"

int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "bad") == 0) {
            puts("not JSON");
            return 0;
        }
        if (strcmp(argv[i], "failed") == 0) {
            fputs("fixture tool failed\n", stderr);
            return 7;
        }
        if (strcmp(argv[i], "slow") == 0) {
            fb_sleep_ms(3000);
        }
    }
    puts("{\"utterances\":[{\"speaker\":0,\"text\":\"My name is Alice.\",\"start\":0,\"end\":2},"
         "{\"speaker\":1,\"text\":\"Bob will send the draft by Friday.\",\"start\":2,\"end\":5}]}");
    return 0;
}
