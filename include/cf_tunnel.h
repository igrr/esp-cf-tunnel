/**
 * @file cf_tunnel.h
 * @brief Cloudflare Tunnel connector: serves HTTP requests and WebSockets
 * arriving through a Cloudflare Tunnel, using cloudflared's HTTP/2 protocol.
 *
 * A cft_tunnel_t keeps the configured number of connections to the edge
 * open. Each request arrives as a cft_stream_t on the tunnel's thread. On
 * ESP-IDF, use cf_tunnel_esp.h, which runs the tunnel in a task.
 *
 * Requests can also be forwarded to other hosts, as cloudflared does:
 * see cft_tunnel_config_t.use_ingress_rules and cft_stream_proxy().
 *
 * All functions except cft_tunnel_post() and cft_tunnel_request_stop() must
 * be called from the tunnel's thread, i.e. from the callbacks.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief HTTP header */
typedef struct {
    const char *name;  /*!< Header name; lower-case in requests */
    const char *value; /*!< Header value */
} cft_header_t;

/** @brief Request, as received from the edge */
typedef struct {
    const char *method;          /*!< e.g. "GET" */
    const char *path;            /*!< Path and query, e.g. "/api?x=1" */
    const char *authority;       /*!< Host the visitor used */
    const char *scheme;          /*!< "https" or "http" */
    const cft_header_t *headers; /*!< Visitor's headers, plus Cloudflare's (cf-connecting-ip, ...) */
    size_t num_headers;          /*!< Number of entries in headers */
    int64_t content_length;      /*!< Request body length, or -1 if unknown */
    bool is_websocket;           /*!< The visitor asks for a WebSocket; see cft_ws_accept() */
    const char *service;         /*!< Ingress service the request matched (e.g. "http://localhost:80"), or NULL */
} cft_request_t;

/** @brief One request and its response */
typedef struct cft_stream cft_stream_t;

/** @brief WebSocket message types */
typedef enum {
    CFT_WS_TEXT = 1,   /*!< Text message (UTF-8) */
    CFT_WS_BINARY = 2, /*!< Binary message */
    CFT_WS_CLOSE = 8,  /*!< Close frame */
    CFT_WS_PING = 9,   /*!< Ping frame */
    CFT_WS_PONG = 10,  /*!< Pong frame */
} cft_ws_opcode_t;

/**
 * @brief Per-stream callbacks, see cft_stream_set_handlers()
 *
 * All members are optional.
 */
typedef struct {
    /** Part of the request body */
    void (*on_body)(cft_stream_t *s, void *ctx, const uint8_t *data, size_t len);
    /** The request body is complete (also called for requests without a body) */
    void (*on_body_end)(cft_stream_t *s, void *ctx);
    /** All queued response data has been handed to the HTTP/2 session; write the next chunk now */
    void (*on_writable)(cft_stream_t *s, void *ctx);
    /** A complete WebSocket text or binary message (after cft_ws_accept()) */
    void (*on_ws_message)(cft_stream_t *s, void *ctx, cft_ws_opcode_t opcode, const uint8_t *data, size_t len);
    /**
     * The stream has closed: the response completed, the visitor disconnected,
     * or the connection closed. The stream must not be used afterwards; free ctx
     * here. completed is true if the full response was sent.
     */
    void (*on_close)(cft_stream_t *s, void *ctx, bool completed);
} cft_stream_handlers_t;

/**
 * @brief Called when the headers of a new request have arrived
 *
 * Respond right away (cft_stream_respond()), read the body
 * (cft_stream_read_body()), accept a WebSocket (cft_ws_accept()), or set
 * handlers (cft_stream_set_handlers()) and respond later. A stream with no
 * response stays open until the visitor disconnects.
 */
typedef void (*cft_request_fn)(void *arg, cft_stream_t *s, const cft_request_t *req);

/** @brief Request of this stream; valid until on_close */
const cft_request_t *cft_stream_request(const cft_stream_t *s);

/**
 * @brief Value of a request header
 *
 * @param s  Stream
 * @param name  Header name, lower-case
 * @return The value, or NULL if the header is absent
 */
const char *cft_stream_header(const cft_stream_t *s, const char *name);

/**
 * @brief Sets the callbacks of a stream
 *
 * @param s  Stream
 * @param handlers  Callbacks; copied
 * @param ctx  Passed to the callbacks
 */
void cft_stream_set_handlers(cft_stream_t *s, const cft_stream_handlers_t *handlers, void *ctx);

/**
 * @brief Collects the request body, then calls a function with it
 *
 * Replaces the on_body and on_body_end handlers. Bodies over max_len get a
 * 413 response, and fn is not called.
 *
 * @param s  Stream
 * @param max_len  Largest accepted body; 0 for the tunnel's max_request_body
 * @param fn  Called with the complete body (NULL and 0 if there is none)
 * @param ctx  Passed to fn
 * @return 0 on success, -1 if out of memory
 */
int cft_stream_read_body(cft_stream_t *s, size_t max_len,
                         void (*fn)(cft_stream_t *s, void *ctx, const uint8_t *body, size_t len), void *ctx);

/**
 * @brief Starts the response
 *
 * Follow with cft_stream_write() and cft_stream_end(). Include
 * content-length if known; otherwise the edge sends the body chunked.
 *
 * @param s  Stream
 * @param status  HTTP status
 * @param headers  Response headers; copied
 * @param num_headers  Number of headers
 * @return 0 on success, -1 on error (e.g. a response was already started)
 */
int cft_stream_send_headers(cft_stream_t *s, int status, const cft_header_t *headers, size_t num_headers);

/**
 * @brief Queues response body data
 *
 * The data is copied. To limit memory usage, write chunks from on_writable.
 *
 * @return 0 on success, -1 on error
 */
int cft_stream_write(cft_stream_t *s, const void *data, size_t len);

/**
 * @brief Ends the response once the queued data has been sent
 *
 * @return 0 on success, -1 on error
 */
int cft_stream_end(cft_stream_t *s);

/** @brief Response bytes queued but not yet passed to the connection */
size_t cft_stream_buffered(const cft_stream_t *s);

/**
 * @brief Sends a complete response
 *
 * @param s  Stream
 * @param status  HTTP status
 * @param content_type  Content-Type, or NULL
 * @param body  Body; copied
 * @param len  Body length
 * @return 0 on success, -1 on error
 */
int cft_stream_respond(cft_stream_t *s, int status, const char *content_type, const void *body, size_t len);

/** @brief Aborts the stream (HTTP/2 RST_STREAM); on_close follows */
void cft_stream_reset(cft_stream_t *s);

/**
 * @brief Pauses or resumes incoming request body data
 *
 * While paused, the edge can still send up to the remaining stream window
 * (cft_tunnel_config_t.stream_window, 64 KiB by default), which on_body
 * receives as usual before the stream stops. Use this when the consumer is
 * slower than the network, e.g. when writing to flash.
 *
 * @param s  Stream
 * @param pause  true to pause, false to resume
 */
void cft_stream_pause_body(cft_stream_t *s, bool pause);

/**
 * @brief Forwards the request to another host, as cloudflared does
 *
 * Services:
 * - http://host[:port], ws://host[:port]: HTTP/1.1 proxy, including WebSockets;
 * - ssh://, rdp://, smb://, tcp://host[:port]: TCP over WebSocket, used with
 *   `cloudflared access` on the client side;
 * - http_status:CODE: an empty response with that status.
 *
 * Unsupported: https:// and wss:// origins, Unix sockets, and cloudflared's
 * built-in services (hello_world, bastion, socks5).
 *
 * Each request opens a separate connection to the origin. The tunnel takes
 * ownership of the stream; do not set handlers on it. If the origin cannot be
 * reached, the visitor receives 502; above max_origin_connections, 503.
 * Resolving a hostname blocks the tunnel thread; IP addresses do not.
 *
 * @param s  Stream, from on_request
 * @param service  Service URL; copied
 * @return 0 on success (a response follows), -1 if the service is not supported
 */
int cft_stream_proxy(cft_stream_t *s, const char *service);

/**
 * @brief Accepts a WebSocket request (req->is_websocket)
 *
 * Messages then arrive in on_ws_message. Pings are answered and the close
 * handshake is handled; on_close follows.
 *
 * @param s  Stream
 * @param subprotocol  Sec-WebSocket-Protocol to confirm, or NULL
 * @return 0 on success, -1 if the request is not a valid WebSocket request
 */
int cft_ws_accept(cft_stream_t *s, const char *subprotocol);

/**
 * @brief Sends a WebSocket message
 *
 * @param s  Stream
 * @param opcode  CFT_WS_TEXT, CFT_WS_BINARY, CFT_WS_PING or CFT_WS_PONG
 * @param data  Payload; copied
 * @param len  Payload length
 * @return 0 on success, -1 on error
 */
int cft_ws_send(cft_stream_t *s, cft_ws_opcode_t opcode, const void *data, size_t len);

/**
 * @brief Closes a WebSocket
 *
 * @param s  Stream
 * @param code  Close status, e.g. 1000
 * @param reason  Reason text, or NULL
 * @return 0 on success, -1 on error
 */
int cft_ws_close(cft_stream_t *s, uint16_t code, const char *reason);

/** @brief Connection events */
typedef enum {
    CFT_EVENT_REGISTERED,          /*!< Connection is registered and serving */
    CFT_EVENT_REGISTRATION_FAILED, /*!< The edge refused the connection */
    CFT_EVENT_CONFIG_UPDATED,      /*!< The edge pushed the remote configuration */
    CFT_EVENT_DRAINING,            /*!< The edge is draining this connection */
    CFT_EVENT_UNREGISTERED,        /*!< Graceful shutdown acknowledged */
    CFT_EVENT_CLOSED,              /*!< Connection closed */
} cft_event_type_t;

/** @brief Connection event */
typedef struct {
    cft_event_type_t type; /*!< Event type */
    uint8_t conn_index;    /*!< Which connection (0..3) */
    union {
        struct {
            char connection_id[37]; /*!< Connection UUID assigned by the edge */
            const char *location;   /*!< Edge data center, e.g. "fra12" */
            bool remotely_managed;  /*!< The tunnel is configured in the dashboard */
        } registered;               /*!< CFT_EVENT_REGISTERED */
        struct {
            const char *cause;      /*!< Reason given by the edge */
            int64_t retry_after_ms; /*!< Requested delay before retrying */
            bool should_retry;      /*!< Whether the caller should retry */
        } failed;                   /*!< CFT_EVENT_REGISTRATION_FAILED */
        struct {
            int32_t version;  /*!< Configuration version */
            const char *json; /*!< The "config" object, not NUL-terminated */
            size_t json_len;  /*!< Length of json */
        } config;             /*!< CFT_EVENT_CONFIG_UPDATED */
        struct {
            const char *reason; /*!< Why the connection closed */
        } closed;               /*!< CFT_EVENT_CLOSED */
    };                          /*!< Event data, by type */
} cft_event_t;

/** @brief Called for connection events on the tunnel's thread */
typedef void (*cft_event_fn)(void *arg, const cft_event_t *ev);

/**
 * @brief TLS client over a non-blocking socket
 *
 * Implemented by each platform; called from the tunnel's thread.
 */
typedef struct {
    /** Starts connecting; returns a connection handle or NULL. May block for DNS. */
    void *(*open)(void *ctx, const char *host, uint16_t port, const char *sni, const char *ca_pem);
    /** Advances the connection and handshake: 1 done, 0 in progress, -1 failed */
    int (*connect)(void *conn);
    /** Socket to wait on */
    int (*get_fd)(void *conn);
    /** Returns >0 bytes read, 0 if nothing is available now, <0 if closed or failed */
    long (*read)(void *conn, uint8_t *buf, size_t len);
    /** Returns >=0 bytes written (0: try later), <0 if failed. After a short write, pass the rest again. */
    long (*write)(void *conn, const uint8_t *buf, size_t len);
    /** Decrypted bytes buffered in the transport, readable without waiting */
    size_t (*pending)(void *conn);
    /** Local address of the socket; returns its length (4 or 16) or 0 */
    size_t (*local_ip)(void *conn, uint8_t ip[16]);
    /** Closes the connection and frees the handle */
    void (*close)(void *conn);
    /** Passed to open() */
    void *ctx;
} cft_transport_ops_t;

/** @brief Tunnel configuration */
typedef struct {
    const char *token;               /*!< Tunnel token; copied */
    cft_request_fn on_request;       /*!< Called for each request */
    void *request_arg;               /*!< Passed to on_request */
    cft_event_fn on_event;           /*!< Optional */
    void *event_arg;                 /*!< Passed to on_event */
    uint8_t num_connections;         /*!< 1..4; 0 means 1 */
    const uint8_t *connector_id;     /*!< 16 bytes, or NULL for a random one. Keep it stable across restarts:
                                          the edge then immediately replaces connections from the previous boot. */
    const char *edge_hosts[2];       /*!< Edge addresses, one per region; NULL for Cloudflare's */
    uint16_t edge_port;              /*!< 0 means 7844 */
    const char *version;             /*!< Reported to the edge; NULL for the component's */
    const char *arch;                /*!< Reported to the edge; NULL for "unknown" */
    size_t max_request_body;         /*!< Default limit of cft_stream_read_body() and of WebSocket messages; 0 means 16 KiB */
    uint32_t max_concurrent_streams; /*!< Open streams per connection, WebSockets included; more get 503. 0 means 16 */
    bool use_ingress_rules;          /*!< Route requests by the public hostnames configured in the dashboard:
                                          localhost services are passed to on_request (if set), others to
                                          cft_stream_proxy(). Without it, or until the edge sends the
                                          rules, all requests go to on_request. */
    uint8_t max_origin_connections;  /*!< Open connections to other hosts; more get 503. 0 means 4 */
    uint32_t stream_window;          /*!< HTTP/2 receive window per stream: maximum unacknowledged request body
                                          bytes from the edge. Bounds upload throughput (window / round-trip
                                          time) and buffered data for a paused stream. 0 means 64 KiB */
    size_t proxy_buffer_size;        /*!< Data buffered per proxied request in each direction, see
                                          cft_stream_proxy(). 0 means 16 KiB */
} cft_tunnel_config_t;

/** @brief Tunnel handle */
typedef struct cft_tunnel cft_tunnel_t;

/**
 * @brief Creates a tunnel
 *
 * @param cfg  Configuration; copied, except for the strings it points to
 * @param transport  Transport; copied
 * @return The tunnel, or NULL if the token is invalid or memory runs out
 */
cft_tunnel_t *cft_tunnel_new(const cft_tunnel_config_t *cfg, const cft_transport_ops_t *transport);

/**
 * @brief Runs the tunnel on the calling thread
 *
 * Returns after cft_tunnel_request_stop(), once the connections have
 * unregistered or the stop timeout has passed.
 */
void cft_tunnel_run(cft_tunnel_t *tunnel);

/**
 * @brief Asks cft_tunnel_run() to stop; thread-safe
 *
 * @param tunnel  Tunnel
 * @param stop_timeout_ms  How long to wait for the connections to unregister
 */
void cft_tunnel_request_stop(cft_tunnel_t *tunnel, uint32_t stop_timeout_ms);

/**
 * @brief Runs a function on the tunnel's thread; thread-safe
 *
 * Use this to send responses or WebSocket messages from other threads.
 * The callback runs within 100 ms. A stream may close in the meantime:
 * keep track of on_close.
 *
 * @return 0 on success, -1 if out of memory
 */
int cft_tunnel_post(cft_tunnel_t *tunnel, void (*fn)(void *arg), void *arg);

/** @brief Frees a tunnel that is not running */
void cft_tunnel_free(cft_tunnel_t *tunnel);

#ifdef __cplusplus
}
#endif
