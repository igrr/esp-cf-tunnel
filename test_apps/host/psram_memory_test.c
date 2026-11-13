/*
 * Unit tests of cft_alloc.h and the nghttp2 callbacks using host heap-capability stubs.
 * Exit status 0 if all pass.
 */
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "cft_alloc.h"
#include "cft_h2_alloc.h"
#include "cft_debug.h"

#if defined(malloc) || defined(calloc) || defined(realloc) || defined(strdup)
#error "The data allocator must not redefine libc names"
#endif

static int failures;
static unsigned allocation_calls;
static bool fail_allocation;
static bool fail_psram;
static unsigned last_caps;
static unsigned prefer_calls;
static size_t last_malloc_size;
static unsigned realloc_calls;

static void check(bool condition, const char *message)
{
    if (!condition) {
        printf("FAIL: %s\n", message);
        failures++;
    }
}

static void check_caps(unsigned caps)
{
    check(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) ||
          caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), "byte-accessible PSRAM or internal RAM");
    last_caps = caps;
    allocation_calls++;
}

/* These use the real libc allocator; cft_alloc.h does not rename it. */
void *heap_caps_malloc(size_t size, unsigned caps)
{
    check_caps(caps);
    last_malloc_size = size;
    return fail_allocation || (fail_psram && (caps & MALLOC_CAP_SPIRAM)) ? NULL : malloc(size);
}

void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    check_caps(caps);
    return fail_allocation || (fail_psram && (caps & MALLOC_CAP_SPIRAM)) ? NULL : calloc(count, size);
}

void *heap_caps_realloc(void *ptr, size_t size, unsigned caps)
{
    check_caps(caps);
    realloc_calls++;
    if (!size) {
        free(ptr);
        return NULL;
    }
    return fail_allocation || (fail_psram && (caps & MALLOC_CAP_SPIRAM)) ? NULL : realloc(ptr, size);
}

/* SDK prefer APIs try the supplied capabilities in order. A zero-size realloc
 * frees once and must not retry the now-freed pointer. */
static unsigned next_caps(va_list *args, size_t index)
{
    unsigned caps = va_arg(*args, unsigned);
    check(caps == ((index ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM) | MALLOC_CAP_8BIT),
          "PSRAM precedes internal fallback");
    return caps;
}

void *heap_caps_malloc_prefer(size_t size, size_t num, ...)
{
    check(num == 2, "two preferred heaps");
    prefer_calls++;
    va_list args;
    va_start(args, num);
    void *p = NULL;
    for (size_t i = 0; i < num && !p; i++) {
        p = heap_caps_malloc(size, next_caps(&args, i));
    }
    va_end(args);
    return p;
}

void *heap_caps_calloc_prefer(size_t count, size_t size, size_t num, ...)
{
    check(num == 2, "two preferred heaps");
    prefer_calls++;
    va_list args;
    va_start(args, num);
    void *p = NULL;
    for (size_t i = 0; i < num && !p; i++) {
        p = heap_caps_calloc(count, size, next_caps(&args, i));
    }
    va_end(args);
    return p;
}

void *heap_caps_realloc_prefer(void *ptr, size_t size, size_t num, ...)
{
    check(num == 2, "two preferred heaps");
    prefer_calls++;
    va_list args;
    va_start(args, num);
    void *p = NULL;
    for (size_t i = 0; i < num; i++) {
        p = heap_caps_realloc(ptr, size, next_caps(&args, i));
        if (p || !size) {
            break;
        }
    }
    va_end(args);
    return p;
}

static void test_policy(void)
{
    unsigned before = allocation_calls;
    fail_psram = true;
    char *p = cft_data_malloc(8);
#ifdef CFT_TEST_ALLOC_PREFER
    check(p != NULL, "PSRAM exhaustion falls back to internal RAM");
    check(allocation_calls == before + 2, "fallback tries both heaps");
    check(last_caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), "fallback selects internal RAM");
    if (p) {
        strcpy(p, "keep");
        char *grown = cft_data_realloc(p, 32);
        check(grown != NULL, "realloc can fall back to internal RAM");
        if (grown) {
            p = grown;
            check(strcmp(p, "keep") == 0, "fallback realloc preserves data");
        }
        fail_allocation = true;
        check(cft_data_realloc(p, 64) == NULL, "both heaps exhausted preserves the old block");
        check(strcmp(p, "keep") == 0, "failed fallback realloc preserves data");
        fail_allocation = false;
        fail_psram = false;
        grown = cft_data_realloc(p, 48);
        check(grown != NULL, "recovered PSRAM can satisfy a later realloc");
        if (grown) {
            p = grown;
            check(strcmp(p, "keep") == 0, "realloc back to PSRAM preserves data");
        }
        check(last_caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT), "realloc prefers recovered PSRAM");
    }
    free(p);
    fail_psram = true;
    unsigned char *zero = cft_data_calloc(4, 16);
    check(zero != NULL, "calloc falls back to internal RAM");
    if (zero) {
        for (size_t i = 0; i < 64; i++) {
            check(zero[i] == 0, "fallback calloc zeroes memory");
        }
    }
    free(zero);
    char *copy = cft_data_strdup("example");
    check(copy != NULL, "strdup can fall back");
    if (copy) {
        check(strcmp(copy, "example") == 0, "fallback strdup preserves data");
    }
    free(copy);
#else
    check(p == NULL, "PSRAM-only never falls back to available internal RAM");
    check(allocation_calls == before + 1, "PSRAM-only tries one heap");
    check(last_caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT), "PSRAM-only capabilities");
    free(p);
    check(cft_data_calloc(1, 8) == NULL, "PSRAM-only calloc does not fall back");
    check(cft_data_strdup("example") == NULL, "PSRAM-only strdup does not fall back");
    fail_psram = false;
    p = cft_data_malloc(8);
    check(p != NULL, "allocate before PSRAM-only realloc failure");
    if (p) {
        strcpy(p, "keep");
        fail_psram = true;
        before = allocation_calls;
        check(cft_data_realloc(p, 32) == NULL, "PSRAM-only realloc does not fall back");
        check(allocation_calls == before + 1, "PSRAM-only realloc tries one heap");
        check(strcmp(p, "keep") == 0, "PSRAM-only realloc failure preserves data");
        free(p);
    }
#endif
    fail_psram = false;
    p = cft_data_malloc(8);
    check(p != NULL, "allocate before zero-size realloc");
    if (p) {
        before = allocation_calls;
        check(cft_data_realloc(p, 0) == NULL, "zero-size realloc frees the allocation");
        check(allocation_calls == before + 1, "zero-size realloc is not retried");
    }
#ifdef CFT_TEST_ALLOC_PREFER
    check(prefer_calls > 0, "prefer policy uses SDK preference APIs");
#else
    check(prefer_calls == 0, "PSRAM-only uses capability APIs directly");
#endif
}

static void test_h2_allocator(void)
{
    const nghttp2_mem *mem = cft_h2_mem();
    unsigned before = allocation_calls;
    fail_allocation = true;
    check(mem->calloc(SIZE_MAX, 2, NULL) == NULL, "nghttp2 calloc rejects overflow");
    check(allocation_calls == before, "nghttp2 calloc overflow does not reach the heap");
#ifdef CFT_DEBUG_STATS
    check(mem->malloc(SIZE_MAX, NULL) == NULL, "nghttp2 accounting header rejects overflow");
    check(allocation_calls == before, "accounting overflow does not reach the heap");
#endif
    check(mem->malloc(8, NULL) == NULL, "nghttp2 malloc reports allocation failure");
    check(mem->calloc(1, 8, NULL) == NULL, "nghttp2 calloc reports allocation failure");

    fail_allocation = false;
    char *p = mem->malloc(8, NULL);
    check(p != NULL, "nghttp2 malloc succeeds");
    if (!p) {
        return;
    }
#ifdef CFT_DEBUG_STATS
    check(last_malloc_size == 8 + 2 * sizeof(size_t), "debug header size");
    check(cft_debug.h2_bytes == 8, "debug tracks payload size");
#else
    check(last_malloc_size == 8, "production has no accounting header");
#endif
    strcpy(p, "keep");
    fail_allocation = true;
    check(mem->realloc(p, 32, NULL) == NULL, "nghttp2 failed realloc reports allocation failure");
    check(strcmp(p, "keep") == 0, "nghttp2 failed realloc preserves data");
#ifdef CFT_DEBUG_STATS
    before = allocation_calls;
    check(mem->realloc(p, SIZE_MAX, NULL) == NULL, "nghttp2 realloc rejects header overflow");
    check(allocation_calls == before, "realloc overflow does not reach the heap");
    check(cft_debug.h2_bytes == 8, "failed realloc preserves accounting");
#endif
    fail_allocation = false;
    before = realloc_calls;
    char *grown = mem->realloc(p, 32, NULL);
    check(grown != NULL, "nghttp2 realloc succeeds");
    if (grown) {
        p = grown;
        check(strcmp(p, "keep") == 0, "nghttp2 realloc preserves data");
    }
#ifdef CFT_DEBUG_STATS
    check(realloc_calls == before, "debug realloc retains accounting implementation");
#else
    check(realloc_calls == before + 1, "production forwards realloc directly to the heap");
#endif
    mem->free(p, NULL);
    mem->free(NULL, NULL);

    unsigned char *zero = mem->calloc(4, 16, NULL);
    check(zero != NULL, "nghttp2 calloc succeeds");
    if (zero) {
        for (size_t i = 0; i < 64; i++) {
            check(zero[i] == 0, "nghttp2 calloc zeroes memory");
        }
    }
    mem->free(zero, NULL);
#ifdef CFT_DEBUG_STATS
    check(cft_debug.h2_bytes == 0, "all nghttp2 payload bytes released");
#endif

    /* Exercise nghttp2's actual use of the callbacks, not only direct calls. */
    nghttp2_session_callbacks *callbacks = NULL;
    int rv = nghttp2_session_callbacks_new(&callbacks);
    check(rv == 0, "nghttp2 callbacks created");
    if (rv != 0) {
        return;
    }
    nghttp2_session *session = NULL;
    fail_allocation = true;
    rv = nghttp2_session_server_new3(&session, callbacks, NULL, NULL, cft_h2_mem());
    check(rv == NGHTTP2_ERR_NOMEM, "nghttp2 reports allocator exhaustion");
    fail_allocation = false;
    before = allocation_calls;
    rv = nghttp2_session_server_new3(&session, callbacks, NULL, NULL, cft_h2_mem());
    check(rv == 0, "nghttp2 session created");
    check(allocation_calls > before, "nghttp2 session uses the selected allocator");
    if (rv == 0) {
        nghttp2_session_del(session);
    }
    nghttp2_session_callbacks_del(callbacks);
#ifdef CFT_DEBUG_STATS
    check(cft_debug.h2_bytes == 0, "nghttp2 session releases all accounted allocations");
#endif
}

int main(void)
{
    char *p = cft_data_malloc(8);
    check(p != NULL, "malloc succeeds");
    if (!p) {
        return 1;
    }
    strcpy(p, "keep");

    fail_allocation = true;
    check(cft_data_realloc(p, 1000) == NULL, "failed realloc reports allocation failure");
    check(strcmp(p, "keep") == 0, "failed realloc preserves the old allocation");
    check(cft_data_malloc(16) == NULL, "failed malloc reports allocation failure");
    check(cft_data_calloc(2, 32) == NULL, "failed calloc reports allocation failure");
    check(cft_data_strdup("example") == NULL, "failed strdup reports allocation failure");

    unsigned before = allocation_calls;
    check(cft_data_calloc(SIZE_MAX, 2) == NULL, "calloc rejects overflow");
    check(allocation_calls == before, "calloc overflow does not reach the heap allocator");

    fail_allocation = false;
    char *grown = cft_data_realloc(p, 1000);
    check(grown != NULL, "realloc succeeds");
    if (grown) {
        p = grown;
        check(strcmp(p, "keep") == 0, "successful realloc preserves data");
    }
    free(p);

    char *copy = cft_data_strdup("example");
    check(copy != NULL, "strdup succeeds");
    if (copy) {
        check(strcmp(copy, "example") == 0, "strdup preserves data");
    }
    free(copy);

    unsigned char *zero = cft_data_calloc(4, 16);
    check(zero != NULL, "calloc succeeds");
    if (zero) {
        for (size_t i = 0; i < 64; i++) {
            check(zero[i] == 0, "calloc zeroes memory");
        }
    }
    free(zero);

    test_policy();
    test_h2_allocator();
    printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
