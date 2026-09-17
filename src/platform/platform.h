#ifndef FB_PLATFORM_H
#define FB_PLATFORM_H

/*
 * The only file in this codebase allowed a bare #ifdef _WIN32: two tiny
 * timing primitives that every platform's C runtime names differently.
 * Everything else platform-specific lives behind screen.h / proc_*.c /
 * sock_*.c, chosen by CMake source-list selection, not preprocessor branches.
 */

#ifdef _WIN32
#include <windows.h>

static inline void fb_sleep_ms(unsigned ms) { Sleep(ms); }

static inline unsigned long long fb_now_ms(void) {
    return (unsigned long long)GetTickCount64();
}
#else
#include <time.h>

static inline void fb_sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static inline unsigned long long fb_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)(ts.tv_nsec / 1000000L);
}
#endif

#endif /* FB_PLATFORM_H */
