/* Verify that targets without PSRAM keep normal allocation semantics. */
#include <stdio.h>
#include <string.h>

#include "cft_alloc.h"

#ifdef CFT_ALLOC_PSRAM
#error "This test requires the standard allocator"
#endif
#if defined(malloc) || defined(calloc) || defined(realloc) || defined(strdup)
#error "The data allocator must not redefine libc names"
#endif

int main(void)
{
    char *p = cft_data_strdup("keep");
    if (!p) {
        return 1;
    }
    char *grown = cft_data_realloc(p, 32);
    if (!grown) {
        free(p);
        return 1;
    }
    int failed = strcmp(grown, "keep") != 0;
    free(grown);
    unsigned char *zero = cft_data_calloc(4, 16);
    if (!zero) {
        return 1;
    }
    for (size_t i = 0; i < 64; i++) {
        failed |= zero[i] != 0;
    }
    free(zero);
    failed |= cft_data_calloc(SIZE_MAX, 2) != NULL;
    printf("%d failures\n", failed);
    return failed ? 1 : 0;
}
