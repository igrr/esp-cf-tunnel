/*
 * Allocation failure injection for the host tests: the core library is
 * built with malloc, calloc, realloc and strdup mapped to these. With
 * CFT_FAIL_RATE=p in the environment, each allocation fails with
 * probability p, for CFT_FAIL_SECONDS (default 20) after the first one.
 */
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double s_rate = -1;
static double s_until;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned s_seed = 1;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static bool fail_now(void)
{
    pthread_mutex_lock(&s_lock);
    if (s_rate < 0) {
        const char *r = getenv("CFT_FAIL_RATE");
        const char *d = getenv("CFT_FAIL_SECONDS");
        s_rate = r ? atof(r) : 0;
        s_until = now_s() + (d ? atof(d) : 20);
        s_seed = (unsigned)time(NULL);
    }
    bool fail = s_rate > 0 && now_s() < s_until && rand_r(&s_seed) < s_rate * RAND_MAX;
    pthread_mutex_unlock(&s_lock);
    return fail;
}

void *cft_test_malloc(size_t n)
{
    return fail_now() ? NULL : malloc(n);
}

void *cft_test_calloc(size_t n, size_t size)
{
    return fail_now() ? NULL : calloc(n, size);
}

void *cft_test_realloc(void *p, size_t n)
{
    return fail_now() ? NULL : realloc(p, n);
}

char *cft_test_strdup(const char *s)
{
    return fail_now() ? NULL : strdup(s);
}
