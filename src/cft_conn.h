/*
 * One tunnel connection over HTTP/2 (internal). It does no I/O: the
 * supervisor (cft_tunnel.c) passes received bytes to cft_conn_recv() and
 * writes out what cft_conn_send_data() returns.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cf_tunnel.h"

#define CFT_EDGE_PORT 7844
#define CFT_EDGE_SNI "h2.cftunnel.com"
/* Targets of the _v2-origintunneld._tcp.argotunnel.com SRV record */
#define CFT_EDGE_REGION1 "region1.v2.argotunnel.com"
#define CFT_EDGE_REGION2 "region2.v2.argotunnel.com"

/* PEM of the CA that signs the edge certificate, NUL-terminated */
extern const char *const cft_edge_ca_pem_str;

typedef struct {
    char account_tag[64];
    uint8_t tunnel_id[16];
    uint8_t secret[64];
    size_t secret_len;
} cft_token_t;

/* Parses the token shown by the dashboard or `cloudflared tunnel token`. */
bool cft_token_parse(const char *token, cft_token_t *out);

typedef struct {
    const cft_token_t *token;
    uint8_t conn_index;            /* 0..3; index 0 receives configuration pushes */
    const uint8_t *client_id;      /* 16 bytes identifying this connector */
    const char *version;           /* reported client version */
    const char *arch;              /* reported architecture, e.g. "esp32p4" */
    const uint8_t *local_ip;       /* our address on the edge socket, informational */
    size_t local_ip_len;           /* 4 or 16 */
    uint8_t num_previous_attempts;
    cft_handler_fn handler;
    void *handler_arg;
    cft_event_fn on_event;         /* events carry conn_index */
    void *event_arg;
    size_t max_request_body;       /* larger requests get 413; 0 = 16 KiB */
    uint32_t max_concurrent_streams; /* requests in flight; more get 503. 0 = 16 */
} cft_conn_config_t;

typedef struct cft_conn cft_conn_t;

/* The config (and the token it points to) must outlive the connection. */
cft_conn_t *cft_conn_new(const cft_conn_config_t *cfg);
void cft_conn_free(cft_conn_t *conn);

/* Feeds bytes read from the TLS connection. Returns <0 if the connection is
 * no longer usable. */
int cft_conn_recv(cft_conn_t *conn, const uint8_t *data, size_t len);

/* Returns bytes to write to the TLS connection, valid until the next call
 * into the connection; 0 when there is nothing to send. Must be called until
 * it returns 0 after every other call into the connection. Returns <0 on
 * error. */
long cft_conn_send_data(cft_conn_t *conn, const uint8_t **data);

/* Timers: keepalive PINGs and the registration timeout. Call at least every
 * second. */
void cft_conn_tick(cft_conn_t *conn, int64_t now_ms);

/* Begins graceful shutdown: unregisters, then closes once the edge
 * acknowledges. CFT_EVENT_UNREGISTERED and CFT_EVENT_CLOSED follow. */
int cft_conn_shutdown(cft_conn_t *conn);

/* True while the connection has not failed or finished. */
bool cft_conn_is_alive(const cft_conn_t *conn);
