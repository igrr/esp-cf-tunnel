#include <sys/types.h> /* before regex.h: older newlib needs off_t */
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cft_ingress.h"
#include "cft_port.h"
#include "cft_util.h"

static const char *TAG = "cft_ingress";

#define MAX_FIELD 256

typedef struct {
    cft_ingress_rule_t pub;
    char *hostname; /* NULL or "*" matches any host; "*.example.com" any subdomain */
    regex_t path;   /* POSIX extended regex, searched like Go's MatchString */
    bool has_path;
    bool broken;    /* path regex did not compile: never matches */
} rule_t;

struct cft_ingress {
    rule_t *rules;
    size_t num_rules;
};

static char *get_str(const char *json, size_t len, const char *key)
{
    char buf[MAX_FIELD];
    if (!cft_json_get_string(json, len, key, buf, sizeof(buf)) || buf[0] == '\0') {
        return NULL;
    }
    return strdup(buf);
}

/* originRequest.httpHostHeader of an object, or NULL */
static char *get_host_header(const char *json, size_t len)
{
    const char *orq;
    size_t orq_len;
    if (!cft_json_get_raw(json, len, "originRequest", &orq, &orq_len)) {
        return NULL;
    }
    return get_str(orq, orq_len, "httpHostHeader");
}

static void rule_free(rule_t *r)
{
    free(r->hostname);
    free((char *)r->pub.service);
    free((char *)r->pub.host_header);
    if (r->has_path) {
        regfree(&r->path);
    }
}

void cft_ingress_free(cft_ingress_t *ing)
{
    if (!ing) {
        return;
    }
    for (size_t i = 0; i < ing->num_rules; i++) {
        rule_free(&ing->rules[i]);
    }
    free(ing->rules);
    free(ing);
}

static bool parse_rule(const char *json, size_t len, const char *default_host_header, rule_t *r)
{
    memset(r, 0, sizeof(*r));
    char *service = get_str(json, len, "service");
    if (!service) {
        CFT_LOGW(TAG, "ingress rule without a service");
        return false;
    }
    r->pub.service = service;
    r->hostname = get_str(json, len, "hostname");
    char *host_header = get_host_header(json, len);
    if (!host_header && default_host_header) {
        host_header = strdup(default_host_header);
    }
    r->pub.host_header = host_header;
    char *path = get_str(json, len, "path");
    if (path) {
        int rv = regcomp(&r->path, path, REG_EXTENDED | REG_NOSUB);
        if (rv == 0) {
            r->has_path = true;
        } else {
            CFT_LOGW(TAG, "path regex '%s' not supported; the rule for %s will not match", path,
                     r->hostname ? r->hostname : "*");
            r->broken = true;
        }
        free(path);
    }
    return true;
}

cft_ingress_t *cft_ingress_parse(const char *json, size_t len)
{
    const char *list;
    size_t list_len;
    if (!cft_json_get_raw(json, len, "ingress", &list, &list_len)) {
        return NULL;
    }
    cft_ingress_t *ing = calloc(1, sizeof(*ing));
    if (!ing) {
        return NULL;
    }
    char *default_host_header = get_host_header(json, len);
    const char *pos = NULL;
    const char *elem;
    size_t elem_len;
    size_t n = 0;
    while (cft_json_array_next(list, list_len, &pos, &elem, &elem_len)) {
        n++;
    }
    ing->rules = calloc(n ? n : 1, sizeof(rule_t));
    if (!ing->rules) {
        free(default_host_header);
        free(ing);
        return NULL;
    }
    pos = NULL;
    while (cft_json_array_next(list, list_len, &pos, &elem, &elem_len)) {
        if (parse_rule(elem, elem_len, default_host_header, &ing->rules[ing->num_rules])) {
            ing->num_rules++;
        }
    }
    free(default_host_header);
    if (ing->num_rules == 0) {
        cft_ingress_free(ing);
        return NULL;
    }
    return ing;
}

static bool match_host(const char *rule, const char *host, size_t host_len)
{
    if (!rule || strcmp(rule, "*") == 0) {
        return true;
    }
    size_t rl = strlen(rule);
    if (rl == host_len && strncasecmp(rule, host, host_len) == 0) {
        return true;
    }
    if (rule[0] == '*' && rule[1] == '.') {
        size_t sl = rl - 1; /* ".example.com" */
        return host_len >= sl && strncasecmp(host + host_len - sl, rule + 1, sl) == 0;
    }
    return false;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c |= 0x20;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

const cft_ingress_rule_t *cft_ingress_match(const cft_ingress_t *ing, const char *authority, const char *path)
{
    /* Host without the port; IPv6 literals keep their brackets */
    size_t host_len = strlen(authority);
    const char *colon = strrchr(authority, ':');
    if (colon && !strchr(colon, ']')) {
        host_len = (size_t)(colon - authority);
    }
    /* Path without the query, percent-decoded (Go's URL.Path) */
    char dpath[MAX_FIELD * 2];
    size_t dn = 0;
    for (const char *p = path; *p && *p != '?' && *p != '#' && dn + 1 < sizeof(dpath); p++) {
        int hi, lo;
        if (*p == '%' && (hi = hexval(p[1])) >= 0 && (lo = hexval(p[2])) >= 0 && (hi || lo)) {
            dpath[dn++] = (char)(hi << 4 | lo);
            p += 2;
        } else {
            dpath[dn++] = *p;
        }
    }
    dpath[dn] = '\0';

    for (size_t i = 0; i < ing->num_rules; i++) {
        const rule_t *r = &ing->rules[i];
        if (r->broken || !match_host(r->hostname, authority, host_len)) {
            continue;
        }
        if (r->has_path && regexec(&r->path, dpath, 0, NULL, 0) != 0) {
            continue;
        }
        return &r->pub;
    }
    return NULL;
}

bool cft_ingress_is_local(const char *service)
{
    static const char *const schemes[] = {"http://", "https://", "ws://", "wss://"};
    const char *host = NULL;
    for (size_t i = 0; i < sizeof(schemes) / sizeof(schemes[0]); i++) {
        size_t n = strlen(schemes[i]);
        if (strncasecmp(service, schemes[i], n) == 0) {
            host = service + n;
            break;
        }
    }
    if (!host) {
        return false;
    }
    static const char *const local[] = {"localhost", "127.0.0.1", "[::1]"};
    for (size_t i = 0; i < sizeof(local) / sizeof(local[0]); i++) {
        size_t n = strlen(local[i]);
        if (strncasecmp(host, local[i], n) == 0 && (host[n] == '\0' || host[n] == ':' || host[n] == '/')) {
            return true;
        }
    }
    return false;
}
