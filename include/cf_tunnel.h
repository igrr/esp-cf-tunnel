/**
 * @file cf_tunnel.h
 * @brief Cloudflare Tunnel connector core: one HTTP/2 connection to the edge.
 *
 * cft_conn_t is one edge connection. It does no I/O itself: the caller
 * establishes TLS to the edge, passes received bytes to cft_conn_recv() and
 * writes out what cft_conn_send_data() returns.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** TCP port of the tunnel edge */
#define CFT_EDGE_PORT 7844
/** TLS server name to use for the HTTP/2 transport */
#define CFT_EDGE_SNI "h2.cftunnel.com"
/** First edge region (target of the _v2-origintunneld._tcp.argotunnel.com SRV record) */
#define CFT_EDGE_REGION1 "region1.v2.argotunnel.com"
/** Second edge region */
#define CFT_EDGE_REGION2 "region2.v2.argotunnel.com"

/** PEM of the CA that signs the edge certificate, NUL-terminated */
extern const char *const cft_edge_ca_pem_str;

/**
 * @brief Decoded tunnel token
 */
typedef struct {
    char account_tag[64];   /*!< Cloudflare account tag */
    uint8_t tunnel_id[16];  /*!< Tunnel UUID */
    uint8_t secret[64];     /*!< Tunnel secret */
    size_t secret_len;      /*!< Length of the secret in bytes */
} cft_token_t;

/**
 * @brief Parse a tunnel token
 *
 * @param token  Token as shown by the dashboard or `cloudflared tunnel token`
 * @param[out] out  Decoded token
 * @return true on success, false if the token is malformed
 */
bool cft_token_parse(const char *token, cft_token_t *out);

/* --- Requests --- */

typedef struct {
    const char *name;
    const char *value;
} cft_header_t;

typedef struct {
    const char *method;
    const char *path;
    const char *authority;
    const char *scheme;
    const cft_header_t *headers; /* visitor headers, names lower-case */
    size_t num_headers;
    const uint8_t *body;
    size_t body_len;
} cft_request_t;

typedef struct cft_response cft_response_t;

/* Status defaults to 200. Header and body data are copied. */
void cft_response_set_status(cft_response_t *resp, int status);
int cft_response_add_header(cft_response_t *resp, const char *name, const char *value);
int cft_response_set_body(cft_response_t *resp, const void *data, size_t len);

/* Called once the full request (including body) has arrived. */
typedef void (*cft_handler_fn)(void *arg, const cft_request_t *req, cft_response_t *resp);

/* --- Events --- */

typedef enum {
    CFT_EVENT_REGISTERED,          /* connection is live */
    CFT_EVENT_REGISTRATION_FAILED, /* edge refused; see retry fields */
    CFT_EVENT_CONFIG_UPDATED,      /* remote configuration pushed by the edge */
    CFT_EVENT_UNREGISTERED,        /* graceful shutdown acknowledged */
    CFT_EVENT_CLOSED,              /* connection is unusable; close the socket */
} cft_event_type_t;

typedef struct {
    cft_event_type_t type;
    union {
        struct {
            char connection_id[37];
            const char *location;
            bool remotely_managed;
        } registered;
        struct {
            const char *cause;
            int64_t retry_after_ms;
            bool should_retry;
        } failed;
        struct {
            int32_t version;
            const char *json; /* the "config" object, not NUL-terminated */
            size_t json_len;
        } config;
        struct {
            const char *reason;
        } closed;
    };
} cft_event_t;

typedef void (*cft_event_fn)(void *arg, const cft_event_t *ev);

/* --- Connection --- */

typedef struct {
    const cft_token_t *token;
    uint8_t conn_index;            /* 0..3; index 0 receives configuration pushes */
    const uint8_t *client_id;      /* 16 bytes identifying this connector; NULL = random */
    const char *version;           /* reported client version */
    const char *arch;              /* reported architecture, e.g. "esp32p4" */
    const uint8_t *local_ip;       /* our address on the edge socket, informational */
    size_t local_ip_len;           /* 4 or 16 */
    uint8_t num_previous_attempts;
    cft_handler_fn handler;
    void *handler_arg;
    cft_event_fn on_event;
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
 * it returns 0 after every cft_conn_recv() and cft_conn_shutdown(). Returns
 * <0 on error. */
long cft_conn_send_data(cft_conn_t *conn, const uint8_t **data);

/* Begins graceful shutdown: unregisters, then sends GOAWAY once the edge
 * acknowledges. CFT_EVENT_UNREGISTERED follows. */
int cft_conn_shutdown(cft_conn_t *conn);

/* True while the connection has not failed or finished. */
bool cft_conn_is_alive(const cft_conn_t *conn);

#ifdef __cplusplus
}
#endif
