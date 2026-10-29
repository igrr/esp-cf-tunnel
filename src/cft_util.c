#include "cft_util.h"

#include <stdio.h>
#include <string.h>

static const char b64_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void cft_b64_encode_raw(const uint8_t *in, size_t len, char *out)
{
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        *out++ = b64_alphabet[(v >> 18) & 63];
        *out++ = b64_alphabet[(v >> 12) & 63];
        *out++ = b64_alphabet[(v >> 6) & 63];
        *out++ = b64_alphabet[v & 63];
        i += 3;
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        *out++ = b64_alphabet[(v >> 18) & 63];
        *out++ = b64_alphabet[(v >> 12) & 63];
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        *out++ = b64_alphabet[(v >> 18) & 63];
        *out++ = b64_alphabet[(v >> 12) & 63];
        *out++ = b64_alphabet[(v >> 6) & 63];
    }
}

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+' || c == '-') {
        return 62;
    }
    if (c == '/' || c == '_') {
        return 63;
    }
    return -1;
}

int cft_b64_decode(const char *in, size_t len, uint8_t *out, size_t out_size)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        char c = in[i];
        if (c == '=') {
            break;
        }
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            continue;
        }
        int v = b64_value(c);
        if (v < 0) {
            return -1;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= out_size) {
                return -1;
            }
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)n;
}

/* --- JSON --- */

typedef struct {
    const char *p;
    const char *end;
} json_cur_t;

static void json_ws(json_cur_t *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\n' || *c->p == '\r' || *c->p == '\t')) {
        c->p++;
    }
}

/* Skips a string starting at the opening quote. Sets *start and *slen to the raw
 * (still escaped) contents. */
static bool json_skip_string(json_cur_t *c, const char **start, size_t *slen)
{
    if (c->p >= c->end || *c->p != '"') {
        return false;
    }
    c->p++;
    const char *s = c->p;
    while (c->p < c->end && *c->p != '"') {
        if (*c->p == '\\') {
            c->p++;
        }
        c->p++;
    }
    if (c->p >= c->end) {
        return false;
    }
    if (start) {
        *start = s;
        *slen = (size_t)(c->p - s);
    }
    c->p++;
    return true;
}

static bool json_skip_value(json_cur_t *c)
{
    json_ws(c);
    if (c->p >= c->end) {
        return false;
    }
    if (*c->p == '"') {
        return json_skip_string(c, NULL, NULL);
    }
    if (*c->p == '{' || *c->p == '[') {
        int depth = 0;
        while (c->p < c->end) {
            char ch = *c->p;
            if (ch == '"') {
                if (!json_skip_string(c, NULL, NULL)) {
                    return false;
                }
                continue;
            }
            if (ch == '{' || ch == '[') {
                depth++;
            } else if (ch == '}' || ch == ']') {
                depth--;
                if (depth == 0) {
                    c->p++;
                    return true;
                }
            }
            c->p++;
        }
        return false;
    }
    /* number, true, false, null */
    while (c->p < c->end && *c->p != ',' && *c->p != '}' && *c->p != ']' && *c->p != ' ' && *c->p != '\n' &&
            *c->p != '\r' && *c->p != '\t') {
        c->p++;
    }
    return true;
}

/* Positions the cursor at the value of top-level member `key`. */
static bool json_find(const char *json, size_t len, const char *key, json_cur_t *c)
{
    c->p = json;
    c->end = json + len;
    size_t klen = strlen(key);
    json_ws(c);
    if (c->p >= c->end || *c->p != '{') {
        return false;
    }
    c->p++;
    for (;;) {
        json_ws(c);
        if (c->p < c->end && *c->p == '}') {
            return false;
        }
        const char *k;
        size_t kl;
        if (!json_skip_string(c, &k, &kl)) {
            return false;
        }
        json_ws(c);
        if (c->p >= c->end || *c->p != ':') {
            return false;
        }
        c->p++;
        json_ws(c);
        if (kl == klen && memcmp(k, key, klen) == 0) {
            return true;
        }
        if (!json_skip_value(c)) {
            return false;
        }
        json_ws(c);
        if (c->p < c->end && *c->p == ',') {
            c->p++;
        }
    }
}

bool cft_json_get_string(const char *json, size_t len, const char *key, char *out, size_t out_size)
{
    json_cur_t c;
    if (!json_find(json, len, key, &c)) {
        return false;
    }
    const char *s;
    size_t sl;
    if (!json_skip_string(&c, &s, &sl)) {
        return false;
    }
    size_t n = 0;
    for (size_t i = 0; i < sl; i++) {
        char ch = s[i];
        if (ch == '\\' && i + 1 < sl) {
            char e = s[++i];
            switch (e) {
            case 'n': ch = '\n'; break;
            case 't': ch = '\t'; break;
            case 'r': ch = '\r'; break;
            case 'b': ch = '\b'; break;
            case 'f': ch = '\f'; break;
            case 'u': return false; /* not needed for the fields we read */
            default: ch = e; break;
            }
        }
        if (n + 1 >= out_size) {
            return false;
        }
        out[n++] = ch;
    }
    out[n] = '\0';
    return true;
}

bool cft_json_get_int(const char *json, size_t len, const char *key, int64_t *out)
{
    json_cur_t c;
    if (!json_find(json, len, key, &c)) {
        return false;
    }
    bool neg = false;
    if (c.p < c.end && *c.p == '-') {
        neg = true;
        c.p++;
    }
    if (c.p >= c.end || *c.p < '0' || *c.p > '9') {
        return false;
    }
    int64_t v = 0;
    while (c.p < c.end && *c.p >= '0' && *c.p <= '9') {
        int64_t d = *c.p - '0';
        if (v > (INT64_MAX - d) / 10) {
            return false; /* does not fit: not a value we can use */
        }
        v = v * 10 + d;
        c.p++;
    }
    *out = neg ? -v : v;
    return true;
}

bool cft_json_get_raw(const char *json, size_t len, const char *key, const char **value, size_t *value_len)
{
    json_cur_t c;
    if (!json_find(json, len, key, &c)) {
        return false;
    }
    const char *start = c.p;
    if (!json_skip_value(&c)) {
        return false;
    }
    *value = start;
    *value_len = (size_t)(c.p - start);
    return true;
}

bool cft_json_array_next(const char *array, size_t len, const char **pos, const char **elem, size_t *elem_len)
{
    json_cur_t c = {.p = *pos ? *pos : array, .end = array + len};
    json_ws(&c);
    if (!*pos) {
        if (c.p >= c.end || *c.p != '[') {
            return false;
        }
        c.p++;
        json_ws(&c);
    } else if (c.p < c.end && *c.p == ',') {
        c.p++;
        json_ws(&c);
    }
    if (c.p >= c.end || *c.p == ']') {
        return false;
    }
    const char *start = c.p;
    if (!json_skip_value(&c)) {
        return false;
    }
    *elem = start;
    *elem_len = (size_t)(c.p - start);
    *pos = c.p;
    return true;
}

/* --- UUID --- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool cft_uuid_parse(const char *s, uint8_t out[16])
{
    int n = 0;
    for (const char *p = s; *p && n < 32; p++) {
        if (*p == '-') {
            continue;
        }
        int v = hexval(*p);
        if (v < 0) {
            return false;
        }
        if (n % 2 == 0) {
            out[n / 2] = (uint8_t)(v << 4);
        } else {
            out[n / 2] |= (uint8_t)v;
        }
        n++;
    }
    return n == 32;
}

void cft_uuid_format(const uint8_t in[16], char out[37])
{
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", in[0], in[1], in[2],
             in[3], in[4], in[5], in[6], in[7], in[8], in[9], in[10], in[11], in[12], in[13], in[14], in[15]);
}

/* --- SHA-1 (RFC 3174) --- */

static uint32_t rol32(uint32_t v, int n)
{
    return (v << n) | (v >> (32 - n));
}

static void sha1_block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

void cft_sha1(const uint8_t *data, size_t len, uint8_t digest[20])
{
    uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    size_t i = 0;
    for (; i + 64 <= len; i += 64) {
        sha1_block(h, data + i);
    }
    uint8_t tail[128] = {0};
    size_t rem = len - i;
    memcpy(tail, data + i, rem);
    tail[rem] = 0x80;
    size_t tail_len = rem + 1 + 8 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int j = 0; j < 8; j++) {
        tail[tail_len - 1 - j] = (uint8_t)(bits >> (8 * j));
    }
    for (size_t j = 0; j < tail_len; j += 64) {
        sha1_block(h, tail + j);
    }
    for (int j = 0; j < 5; j++) {
        digest[4 * j] = (uint8_t)(h[j] >> 24);
        digest[4 * j + 1] = (uint8_t)(h[j] >> 16);
        digest[4 * j + 2] = (uint8_t)(h[j] >> 8);
        digest[4 * j + 3] = (uint8_t)h[j];
    }
}
