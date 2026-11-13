/* Internal data allocation policy. Task stacks and SDK allocations are separate. */
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
#include "esp_heap_caps.h"
#define CFT_ALLOC_PSRAM 1
#endif
#endif

/* Use PSRAM when enabled, with optional internal fallback. Free blocks with free(). */
static inline void *cft_data_malloc(size_t size)
{
#if defined(CFT_ALLOC_PSRAM) && defined(CONFIG_CF_TUNNEL_PSRAM_FALLBACK)
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#elif defined(CFT_ALLOC_PSRAM)
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(size);
#endif
}

static inline void *cft_data_calloc(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size) {
        return NULL;
    }
#if defined(CFT_ALLOC_PSRAM) && defined(CONFIG_CF_TUNNEL_PSRAM_FALLBACK)
    return heap_caps_calloc_prefer(count, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#elif defined(CFT_ALLOC_PSRAM)
    return heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return calloc(count, size);
#endif
}

static inline void *cft_data_realloc(void *ptr, size_t size)
{
#if defined(CFT_ALLOC_PSRAM) && defined(CONFIG_CF_TUNNEL_PSRAM_FALLBACK)
    return heap_caps_realloc_prefer(ptr, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#elif defined(CFT_ALLOC_PSRAM)
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return realloc(ptr, size);
#endif
}

static inline char *cft_data_strdup(const char *str)
{
    size_t size = strlen(str) + 1;
    char *copy = cft_data_malloc(size);
    if (copy) {
        memcpy(copy, str, size);
    }
    return copy;
}
