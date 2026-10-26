/*
 * Small self-contained helpers: base64 and a minimal JSON field reader.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Length of unpadded base64 for n input bytes. */
static inline size_t cft_b64_raw_len(size_t n)
{
    return (n / 3) * 4 + ((n % 3) ? (n % 3) + 1 : 0);
}

/* Standard alphabet, no padding (Go base64.RawStdEncoding). Writes exactly
 * cft_b64_raw_len(len) bytes, no NUL. */
void cft_b64_encode_raw(const uint8_t *in, size_t len, char *out);

/* Standard or URL-safe alphabet, padding optional, whitespace skipped.
 * Returns decoded length, or -1 on bad input or if out_size is too small. */
int cft_b64_decode(const char *in, size_t len, uint8_t *out, size_t out_size);

/*
 * Read a top-level member of a JSON object. These skip nested values without
 * building a tree, which is enough for the token and configuration payloads.
 * Return false if the input is not an object or the key is missing or has the
 * wrong type.
 */
bool cft_json_get_string(const char *json, size_t len, const char *key, char *out, size_t out_size);
bool cft_json_get_int(const char *json, size_t len, const char *key, int64_t *out);
/* Raw text of a member's value (object, array, string with quotes, ...). */
bool cft_json_get_raw(const char *json, size_t len, const char *key, const char **value, size_t *value_len);
/* Iterates over the elements of an array: start with *pos = NULL. Returns
 * false after the last element. */
bool cft_json_array_next(const char *array, size_t len, const char **pos, const char **elem, size_t *elem_len);

/* Parse "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" into 16 bytes. */
bool cft_uuid_parse(const char *s, uint8_t out[16]);

/* Format 16 bytes as a UUID string; out must hold 37 bytes. */
void cft_uuid_format(const uint8_t in[16], char out[37]);

/* SHA-1 of data (for the WebSocket handshake) */
void cft_sha1(const uint8_t *data, size_t len, uint8_t digest[20]);
