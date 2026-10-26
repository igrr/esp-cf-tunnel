/*
 * Ingress rules of a remotely managed tunnel (internal): the "ingress" list
 * of the configuration the edge pushes, i.e. the public hostnames configured
 * in the dashboard. Matched as cloudflared does (ingress/ingress.go).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *service;     /* e.g. "http://192.168.1.10:8080", "ssh://10.0.0.2:22", "http_status:404" */
    const char *host_header; /* originRequest.httpHostHeader, or NULL */
} cft_ingress_rule_t;

typedef struct cft_ingress cft_ingress_t;

/* Parses the "config" object of a configuration update. Returns NULL if it
 * has no valid ingress list or memory runs out. */
cft_ingress_t *cft_ingress_parse(const char *json, size_t len);
void cft_ingress_free(cft_ingress_t *ing);

/* First rule matching the request's host (port ignored) and path (query
 * ignored); NULL if none does. */
const cft_ingress_rule_t *cft_ingress_match(const cft_ingress_t *ing, const char *authority, const char *path);

/* True for HTTP services on this device (localhost, 127.0.0.1, [::1]) */
bool cft_ingress_is_local(const char *service);
