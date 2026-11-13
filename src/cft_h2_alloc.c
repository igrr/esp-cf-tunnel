#include "cft_h2_alloc.h"

#include "cft_alloc.h"
#include "cft_debug.h"

#ifdef CFT_DEBUG_STATS
/* nghttp2 allocator that counts what nghttp2 holds */
static void *h2_malloc(size_t size, void *ud)
{
    if (size > SIZE_MAX - sizeof(size_t) * 2) {
        return NULL;
    }
    size_t *p = cft_data_malloc(size + sizeof(size_t) * 2);
    if (!p) {
        return NULL;
    }
    p[0] = size;
    CFT_DEBUG_ADD(h2_bytes, size);
    CFT_DEBUG_PEAK(h2_bytes, h2_peak);
    return p + 2;
}

static void h2_free(void *ptr, void *ud)
{
    if (ptr) {
        size_t *p = (size_t *)ptr - 2;
        CFT_DEBUG_ADD(h2_bytes, -p[0]);
        free(p);
    }
}

static void *h2_calloc(size_t n, size_t size, void *ud)
{
    if (size && n > SIZE_MAX / size) {
        return NULL;
    }
    void *p = h2_malloc(n * size, ud);
    if (p) {
        memset(p, 0, n * size);
    }
    return p;
}

static void *h2_realloc(void *ptr, size_t size, void *ud)
{
    if (!ptr) {
        return h2_malloc(size, ud);
    }
    size_t old = ((size_t *)ptr - 2)[0];
    void *np = h2_malloc(size, ud);
    if (np) {
        memcpy(np, ptr, old < size ? old : size);
        h2_free(ptr, ud);
    }
    return np;
}

#else
/* Production forwards directly; accounting headers are only needed for debug. */
static void *h2_malloc(size_t size, void *ud)
{
    return cft_data_malloc(size);
}

static void h2_free(void *ptr, void *ud)
{
    free(ptr);
}

static void *h2_calloc(size_t count, size_t size, void *ud)
{
    return cft_data_calloc(count, size);
}

static void *h2_realloc(void *ptr, size_t size, void *ud)
{
    return cft_data_realloc(ptr, size);
}
#endif

static nghttp2_mem h2_mem = {NULL, h2_malloc, h2_free, h2_calloc, h2_realloc};

nghttp2_mem *cft_h2_mem(void)
{
    return &h2_mem;
}
