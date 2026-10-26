/*
 * Platform glue for the portable tunnel core: logging and randomness.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#define CFT_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#define CFT_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define CFT_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define CFT_LOGD(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)

static inline void cft_port_random(void *buf, size_t len)
{
    esp_fill_random(buf, len);
}

/* Monotonic milliseconds, never 0 */
static inline int64_t cft_port_now_ms(void)
{
    return esp_timer_get_time() / 1000 + 1;
}

#else /* host */
#include <stdio.h>
#include <sys/random.h>
#include <time.h>

extern int cft_host_log_level; /* 0 = errors only ... 3 = debug */

#define CFT_LOG_(lvl, c, tag, fmt, ...)                                     \
    do {                                                                    \
        if (cft_host_log_level >= (lvl)) {                                  \
            fprintf(stderr, c " (%s) " fmt "\n", tag, ##__VA_ARGS__);       \
        }                                                                   \
    } while (0)
#define CFT_LOGE(tag, fmt, ...) CFT_LOG_(0, "E", tag, fmt, ##__VA_ARGS__)
#define CFT_LOGW(tag, fmt, ...) CFT_LOG_(1, "W", tag, fmt, ##__VA_ARGS__)
#define CFT_LOGI(tag, fmt, ...) CFT_LOG_(2, "I", tag, fmt, ##__VA_ARGS__)
#define CFT_LOGD(tag, fmt, ...) CFT_LOG_(3, "D", tag, fmt, ##__VA_ARGS__)

static inline void cft_port_random(void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        ssize_t n = getrandom(p, len, 0);
        if (n <= 0) {
            continue;
        }
        p += n;
        len -= (size_t)n;
    }
}

/* Monotonic milliseconds, never 0 */
static inline int64_t cft_port_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + 1;
}
#endif
