/*
 * Cloudflare Tunnel connector: serves HTTP requests that arrive through a
 * Cloudflare Tunnel, using cloudflared's HTTP/2 protocol.
 *
 * cft_tunnel_t keeps the configured number of connections to the edge open,
 * reconnecting as needed. It runs on any platform that provides a
 * cft_transport_ops_t (TLS over a non-blocking socket); on ESP-IDF,
 * cf_tunnel_esp.h provides one and runs the tunnel in a task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
    CFT_EVENT_DRAINING,            /* edge asked to move off this connection */
    CFT_EVENT_UNREGISTERED,        /* graceful shutdown acknowledged */
    CFT_EVENT_CLOSED,              /* connection closed */
} cft_event_type_t;

typedef struct {
    cft_event_type_t type;
    uint8_t conn_index;            /* which connection (0..3) */
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

/* --- Transport --- */

/*
 * TLS client over a non-blocking socket. All functions are called from the
 * tunnel's thread.
 */
typedef struct {
    /* Starts connecting. ca_pem verifies the server, sni is also the name to
     * check. Returns a connection handle or NULL. May block for DNS. */
    void *(*open)(void *ctx, const char *host, uint16_t port, const char *sni, const char *ca_pem);
    /* Advances the connection and handshake: 1 done, 0 in progress, -1 failed */
    int (*connect)(void *conn);
    /* Socket to wait on */
    int (*get_fd)(void *conn);
    /* >0 bytes read, 0 nothing available now, <0 closed or failed */
    long (*read)(void *conn, uint8_t *buf, size_t len);
    /* >=0 bytes written (0: try again later), <0 failed. After a short write,
     * the next call must pass the remaining bytes again. */
    long (*write)(void *conn, const uint8_t *buf, size_t len);
    /* Decrypted bytes buffered in the transport, readable without waiting */
    size_t (*pending)(void *conn);
    /* Local address of the socket; returns its length (4 or 16) or 0 */
    size_t (*local_ip)(void *conn, uint8_t ip[16]);
    void (*close)(void *conn);
    void *ctx;
} cft_transport_ops_t;

/* --- Tunnel --- */

typedef struct {
    const char *token;               /* tunnel token; copied */
    cft_handler_fn handler;          /* called from the tunnel thread */
    void *handler_arg;
    cft_event_fn on_event;           /* optional, called from the tunnel thread */
    void *event_arg;
    uint8_t num_connections;         /* 1..4; 0 = 1 */
    const uint8_t *connector_id;     /* 16 bytes; NULL = random. Keep it stable across restarts:
                                        the edge then replaces the connections of the previous run
                                        instead of routing requests to them until they time out. */
    const char *edge_hosts[2];       /* edge addresses (one per region); NULL = Cloudflare's */
    uint16_t edge_port;              /* 0 = 7844 */
    const char *version;             /* reported to the edge; NULL = component default */
    const char *arch;                /* reported to the edge; NULL = "unknown" */
    size_t max_request_body;         /* larger requests get 413; 0 = 16 KiB */
    uint32_t max_concurrent_streams; /* requests in flight per connection; more get 503. 0 = 16 */
} cft_tunnel_config_t;

typedef struct cft_tunnel cft_tunnel_t;

/* Returns NULL if the token is invalid or memory runs out. */
cft_tunnel_t *cft_tunnel_new(const cft_tunnel_config_t *cfg, const cft_transport_ops_t *transport);

/* Runs the tunnel until cft_tunnel_request_stop() is called and the
 * connections have unregistered (or the stop timeout has passed). */
void cft_tunnel_run(cft_tunnel_t *tunnel);

/* Thread-safe; cft_tunnel_run() returns soon after. */
void cft_tunnel_request_stop(cft_tunnel_t *tunnel, uint32_t stop_timeout_ms);

void cft_tunnel_free(cft_tunnel_t *tunnel);

#ifdef __cplusplus
}
#endif
