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
        v = v * 10 + (*c.p - '0');
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
