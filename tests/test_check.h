#ifndef FB_TEST_CHECK_H
#define FB_TEST_CHECK_H

#include <stdio.h>
#include <stdlib.h>

static inline void fb_test_check(int passed, const char *expression, const char *file, int line) {
    if (passed) {
        return;
    }

    fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
    exit(EXIT_FAILURE);
}

#define FB_CHECK(expression) \
    fb_test_check((expression), #expression, __FILE__, __LINE__)

#endif
