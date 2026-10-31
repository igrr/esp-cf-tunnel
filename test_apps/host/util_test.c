/*
 * Unit tests of the JSON string reader in cft_util.c. Exit status 0 if all pass.
 */
#include <stdio.h>
#include <string.h>

#include "cft_util.h"

int cft_host_log_level = 3;

static int failures;

/* Reads key "k" of json and checks the result and, if it is 1, the value. */
static void check(const char *json, size_t out_size, int want_rv, const char *want)
{
    char out[64] = "";
    int rv = cft_json_read_string(json, strlen(json), "k", out, out_size);
    if (rv != want_rv || (rv == 1 && strcmp(out, want) != 0)) {
        printf("FAIL: %s: got %d '%s', want %d '%s'\n", json, rv, rv == 1 ? out : "", want_rv, want ? want : "");
        failures++;
    }
}

int main(void)
{
    check("{\"k\": \"abc\"}", 64, 1, "abc");
    check("{\"k\": \"\"}", 64, 1, "");
    check("{\"a\": 1}", 64, 0, NULL);
    check("{}", 64, 0, NULL);
    check("{\"k\": null}", 64, 0, NULL);
    check("{\"k\": 5}", 64, -1, NULL);
    check("{\"k\": [\"x\"]}", 64, -1, NULL);
    check("{\"a\" 1, \"k\": \"x\"}", 64, -1, NULL);
    check("{\"a\": \"x", 64, -1, NULL);
    check("[\"k\"]", 64, -1, NULL);

    check("{\"k\": \"\\\"\\\\\\/\\b\\f\\n\\r\\t\"}", 64, 1, "\"\\/\b\f\n\r\t");
    check("{\"k\": \"\\x\"}", 64, -1, NULL);

    check("{\"k\": \"\\u003c\\u003E\\u0026\"}", 64, 1, "<>&");
    check("{\"k\": \"caf\\u00e9\"}", 64, 1, "caf\xc3\xa9");
    check("{\"k\": \"\\u20ac\"}", 64, 1, "\xe2\x82\xac");
    check("{\"k\": \"\\ud83d\\ude00!\"}", 64, 1, "\xf0\x9f\x98\x80!");
    check("{\"k\": \"\\ud83d\"}", 64, -1, NULL);
    check("{\"k\": \"\\ud83dx\"}", 64, -1, NULL);
    check("{\"k\": \"\\ud83d\\u0041\"}", 64, -1, NULL);
    check("{\"k\": \"\\ude00\"}", 64, -1, NULL);
    check("{\"k\": \"\\u0000\"}", 64, -1, NULL);
    check("{\"k\": \"\\u12\"}", 64, -1, NULL);
    check("{\"k\": \"\\u12g4\"}", 64, -1, NULL);

    /* out holds the value and its NUL */
    check("{\"k\": \"abc\"}", 4, 1, "abc");
    check("{\"k\": \"abc\"}", 3, -1, NULL);
    check("{\"k\": \"\\u00e9\"}", 3, 1, "\xc3\xa9");
    check("{\"k\": \"\\u00e9\"}", 2, -1, NULL);

    char out[8];
    if (!cft_json_get_string("{\"k\": \"\\u0041\"}", 15, "k", out, sizeof(out)) || strcmp(out, "A") != 0 ||
            cft_json_get_string("{\"k\": null}", 11, "k", out, sizeof(out))) {
        printf("FAIL: cft_json_get_string\n");
        failures++;
    }
    printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
