#pragma once

#include <stddef.h>

/* Only the capabilities used by cft_alloc.h are needed on the host. */
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4

void *heap_caps_malloc(size_t size, unsigned caps);
void *heap_caps_calloc(size_t count, size_t size, unsigned caps);
void *heap_caps_realloc(void *ptr, size_t size, unsigned caps);

void *heap_caps_malloc_prefer(size_t size, size_t num, ...);
void *heap_caps_calloc_prefer(size_t count, size_t size, size_t num, ...);
void *heap_caps_realloc_prefer(void *ptr, size_t size, size_t num, ...);
