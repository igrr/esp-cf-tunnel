/*
 * Memory counters for tests (internal), compiled in with CFT_DEBUG_STATS.
 * Updated on the tunnel's thread only.
 */
#pragma once

#include <stddef.h>

#ifdef CFT_DEBUG_STATS
typedef struct {
    size_t h2_bytes;  /* allocated by nghttp2 */
    size_t h2_peak;
    size_t sq_bytes;  /* queue segments: responses, proxied requests */
    size_t sq_peak;
    size_t streams;   /* streams of all connections */
    size_t held;      /* request data received for paused streams */
} cft_debug_t;

extern cft_debug_t cft_debug;

#define CFT_DEBUG_ADD(field, n)                                   \
    do {                                                          \
        cft_debug.field += (n);                                   \
    } while (0)
#define CFT_DEBUG_PEAK(field, peak)                               \
    do {                                                          \
        if (cft_debug.field > cft_debug.peak) {                   \
            cft_debug.peak = cft_debug.field;                     \
        }                                                         \
    } while (0)

/* JSON members: ",\"name\":value..." */
int cft_debug_stats(char *buf, size_t size);
#else
#define CFT_DEBUG_ADD(field, n) ((void)0)
#define CFT_DEBUG_PEAK(field, peak) ((void)0)
#endif
