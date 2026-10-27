/*
 * One tunnel connection over HTTP/2. The edge is the HTTP/2 client: it opens
 * a control stream (registration RPC), configuration-update streams and one
 * stream per proxied request. We run an nghttp2 server session.
 */
#include <nghttp2/nghttp2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cft_conn.h"
#include "cft_debug.h"
#include "cft_port.h"
#include "cft_rpc.h"
#include "cft_squeue.h"
#include "cft_util.h"

static const char *TAG = "cft_conn";

/* Request headers set by the edge (cloudflared connection/http2.go) */
#define HDR_UPGRADE "cf-cloudflared-proxy-connection-upgrade"
#define HDR_TCP_SRC "cf-cloudflared-proxy-src"
#define UPGRADE_CONTROL "control-stream"
#define UPGRADE_CONFIG "update-configuration"
#define UPGRADE_WEBSOCKET "websocket"
/* Response headers read by the edge (cloudflared connection/header.go) */
#define HDR_RESP_USER_HEADERS "cf-cloudflared-response-headers"
#define HDR_RESP_META "cf-cloudflared-response-meta"
#define META_ORIGIN "{\"src\":\"origin\"}"
#define META_CLOUDFLARED "{\"src\":\"cloudflared\"}"

/* The edge pings every few seconds. If it goes quiet, ping it ourselves and
 * give up when that goes unanswered too. */
#ifndef CFT_IDLE_PING_MS
#define CFT_IDLE_PING_MS 15000
#endif
#ifndef CFT_PING_TIMEOUT_MS
#define CFT_PING_TIMEOUT_MS 10000
#endif
#define REGISTER_TIMEOUT_MS 15000
#define DEFAULT_MAX_BODY (16 * 1024)

static const char *const default_features[] = {
    "serialized_headers",
    "allow_remote_config",
};

typedef struct {
    uint8_t *p;
    size_t len;
    size_t cap;
    size_t off; /* read position */
} buf_t;

typedef enum {
    STREAM_HTTP,
    STREAM_CONTROL,
    STREAM_CONFIG,
    STREAM_UNSUPPORTED,
} stream_kind_t;

/* Incoming WebSocket frame parser (RFC 6455) */
typedef struct {
    uint8_t hdr[14];
    uint8_t hdr_len;
    uint8_t hdr_need;    /* bytes of header expected; 0 = in payload */
    uint8_t opcode;      /* of the current frame */
    bool fin;
    uint8_t mask[4];
    uint64_t payload_left;
    uint64_t payload_pos;
    buf_t msg;           /* message being reassembled */
    uint8_t msg_opcode;  /* 0 = no message in progress */
    uint8_t ctrl[125];   /* payload of a control frame */
    size_t ctrl_len;
    bool close_sent;
    bool failed;
} ws_rx_t;

struct cft_stream {
    struct cft_stream *next; /* cft_conn.streams */
    cft_conn_t *conn;
    int32_t id;
    stream_kind_t kind;
    /* Request. The fields arrive as [kind][name\0][value\0] records in one
     * buffer (kind 0: regular header; pseudo-headers have no name), indexed
     * once the headers are complete: two allocations per request. */
    buf_t hdrbuf;
    size_t num_headers;
    cft_header_t *headers;
    const char *pseudo[5]; /* indexed by kind */
    cft_request_t req;
    bool upgrade_ws; /* the edge marked it as a WebSocket request */
    bool counted;    /* included in cft_conn.num_requests */
    bool body_ended; /* END_STREAM received */
    char *service;   /* ingress service the request matched */
    /* Flow control: received bytes not yet returned to the edge's window */
    bool paused;
    size_t held;
    /* Application */
    cft_stream_handlers_t h;
    void *ctx;
    /* cft_stream_read_body() and the configuration stream */
    bool collect;
    buf_t body;
    size_t body_max;
    void (*body_fn)(cft_stream_t *s, void *ctx, const uint8_t *body, size_t len);
    void *body_ctx;
    /* Response */
    bool resp_started;
    bool resp_end; /* end after `out` drains */
    bool resp_eof_sent;
    bool deferred; /* data provider waits for cft_stream_write() */
    bool want_writable;
    size_t low_water; /* also call on_writable when this little is left queued */
    bool write_failed;
    uint32_t reset_pending; /* RST_STREAM error code still to queue; 0 = none */
    cft_squeue_t out;
    /* WebSocket */
    bool ws;
    bool ws_raw; /* payload of data frames goes to on_body as it arrives */
    ws_rx_t wsr;
};

struct cft_conn {
    cft_conn_config_t cfg;
    nghttp2_session *h2;
    cft_rpc_t *rpc;
    cft_stream_t *control;
    cft_stream_t *streams; /* all open streams; nghttp2_session_del does not report closes */
    uint32_t num_requests; /* open STREAM_HTTP streams */
    uint8_t client_id[16];
    int32_t config_version;
    /* Incoming frame scanner, see fix_goaway() */
    uint32_t preface_left;
    uint8_t fhdr[9];
    uint8_t fhdr_len;
    uint8_t ftype;
    uint32_t payload_left;
    uint32_t payload_pos;
    /* Timers, see cft_conn_tick() */
    int64_t created_ms;   /* 0 until the first tick */
    int64_t last_rx_ms;
    int64_t ping_sent_ms; /* 0 if no PING outstanding */
    bool rx_since_tick;
    bool resets_pending; /* some stream has reset_pending */
#ifdef CFT_DEBUG_STATS
    int64_t window_zero_since; /* the edge's connection window for our data is 0 */
#endif
    bool alive;
    bool registered;
    bool shutting_down;
    char close_reason[128];
};

/* --- buffers --- */

static int buf_append(buf_t *b, const void *data, size_t len)
{
    if (len == 0) {
        return 0;
    }
    if (b->off && b->len + len > b->cap) {
        /* Reuse the space of data already passed on before growing */
        memmove(b->p, b->p + b->off, b->len - b->off);
        b->len -= b->off;
        b->off = 0;
    }
    if (b->len + len > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + len) {
            cap *= 2;
        }
        uint8_t *np = realloc(b->p, cap);
        if (!np) {
            return -1;
        }
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->len, data, len);
    b->len += len;
    return 0;
}

/* Grows the buffer to hold exactly `total` bytes, for data whose size is
 * known: no doubling, which on a fragmented heap may not find a block */
static int buf_reserve(buf_t *b, size_t total)
{
    if (total <= b->cap) {
        return 0;
    }
    uint8_t *np = realloc(b->p, total);
    if (!np) {
        return -1;
    }
    b->p = np;
    b->cap = total;
    return 0;
}

static void buf_free(buf_t *b)
{
    free(b->p);
    memset(b, 0, sizeof(*b));
}

static void stream_free(cft_stream_t *s)
{
    free(s->service);
    buf_free(&s->hdrbuf);
    free(s->headers);
    buf_free(&s->body);
    cft_sq_free(&s->out);
    buf_free(&s->wsr.msg);
    CFT_DEBUG_ADD(streams, -1);
    CFT_DEBUG_ADD(held, -s->held);
    free(s);
}

static void stream_unlink(cft_conn_t *c, cft_stream_t *s)
{
    for (cft_stream_t **pp = &c->streams; *pp; pp = &(*pp)->next) {
        if (*pp == s) {
            *pp = s->next;
            return;
        }
    }
}

/* --- events --- */

static void emit(cft_conn_t *c, cft_event_t *ev)
{
    ev->conn_index = c->cfg.conn_index;
    if (c->cfg.on_event) {
        c->cfg.on_event(c->cfg.event_arg, ev);
    }
}

static void conn_fail(cft_conn_t *c, const char *reason)
{
    if (!c->alive) {
        return;
    }
    CFT_LOGW(TAG, "closing connection: %s", reason);
    snprintf(c->close_reason, sizeof(c->close_reason), "%s", reason);
    c->alive = false;
    nghttp2_session_terminate_session(c->h2, NGHTTP2_NO_ERROR);
    cft_event_t ev = {.type = CFT_EVENT_CLOSED};
    ev.closed.reason = c->close_reason;
    emit(c, &ev);
}

/* --- response data --- */

static nghttp2_ssize read_out(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length,
                              uint32_t *data_flags, nghttp2_data_source *source, void *user_data)
{
    cft_stream_t *s = source->ptr;
    if (s->out.pending == 0) {
        if (s->resp_end) {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            s->resp_eof_sent = true;
            return 0;
        }
        s->deferred = true;
        return NGHTTP2_ERR_DEFERRED;
    }
    size_t n = cft_sq_read(&s->out, buf, length);
    if (s->out.pending == 0) {
        if (s->resp_end) {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            s->resp_eof_sent = true;
        } else if (s->kind == STREAM_HTTP) {
            s->want_writable = true;
        }
    } else if (s->low_water && s->out.pending <= s->low_water && !s->resp_end) {
        s->want_writable = true;
    }
    return (nghttp2_ssize)n;
}

static void resume(cft_stream_t *s)
{
    if (s->deferred) {
        s->deferred = false;
        nghttp2_session_resume_data(s->conn->h2, s->id);
    }
}

#define NV(n, v) {(uint8_t *)(n), (uint8_t *)(v), strlen(n), strlen(v), NGHTTP2_NV_FLAG_NONE}

/* "b64(name):b64(value);..." with unpadded standard base64 */
static char *serialize_headers(const cft_header_t *h, size_t n)
{
    size_t len = 1;
    for (size_t i = 0; i < n; i++) {
        len += cft_b64_raw_len(strlen(h[i].name)) + cft_b64_raw_len(strlen(h[i].value)) + 2;
    }
    char *out = malloc(len);
    if (!out) {
        return NULL;
    }
    char *p = out;
    for (size_t i = 0; i < n; i++) {
        if (i) {
            *p++ = ';';
        }
        size_t nl = strlen(h[i].name);
        cft_b64_encode_raw((const uint8_t *)h[i].name, nl, p);
        p += cft_b64_raw_len(nl);
        *p++ = ':';
        size_t vl = strlen(h[i].value);
        cft_b64_encode_raw((const uint8_t *)h[i].value, vl, p);
        p += cft_b64_raw_len(vl);
    }
    *p = '\0';
    return out;
}

/* Ends a stream that cannot continue. Queuing the RST_STREAM needs memory
 * too; if that fails, cft_conn_tick() retries, so the visitor is not left
 * waiting on a stream nothing will be sent on. */
static void stream_abort(cft_stream_t *s, uint32_t error_code)
{
    s->write_failed = true;
    if (nghttp2_submit_rst_stream(s->conn->h2, NGHTTP2_FLAG_NONE, s->id, error_code) != 0) {
        s->reset_pending = error_code;
        s->conn->resets_pending = true;
    }
}

/*
 * Response headers as cloudflared sends them: user headers serialized in one
 * header (so HTTP/2 header rules don't apply to them), content-length also
 * as an HTTP/2 header, and the meta header naming the source.
 */
static int submit_headers(cft_stream_t *s, int status, const cft_header_t *headers, size_t num_headers,
                          const char *meta)
{
    if (s->resp_started) {
        return -1;
    }
    char status_str[8];
    snprintf(status_str, sizeof(status_str), "%d", status == 101 ? 200 : status); /* HTTP/2 has no 101 */
    char *serialized = serialize_headers(headers, num_headers);
    if (!serialized) {
        stream_abort(s, NGHTTP2_INTERNAL_ERROR);
        return -1;
    }
    nghttp2_nv nva[4] = {NV(":status", status_str), NV(HDR_RESP_USER_HEADERS, serialized), NV(HDR_RESP_META, meta)};
    size_t nnv = 3;
    for (size_t i = 0; i < num_headers; i++) {
        if (strcasecmp(headers[i].name, "content-length") == 0) {
            nva[nnv++] = (nghttp2_nv)NV("content-length", headers[i].value);
            break;
        }
    }
    nghttp2_data_provider2 prd = {.source.ptr = s, .read_callback = read_out};
    int rv = nghttp2_submit_response2(s->conn->h2, s->id, nva, nnv, &prd);
    free(serialized);
    if (rv != 0) {
        /* Most likely out of memory; without a response the visitor would wait */
        CFT_LOGW(TAG, "stream %d: cannot send the response, resetting", (int)s->id);
        stream_abort(s, NGHTTP2_INTERNAL_ERROR);
        return -1;
    }
    s->resp_started = true;
    return 0;
}

/* Queues response data. If memory runs out, the response cannot be completed
 * as announced, so the stream is reset rather than left with a gap. */
static int queue_out(cft_stream_t *s, const void *data, size_t len)
{
    if (s->write_failed) {
        return -1;
    }
    if (cft_sq_append(&s->out, data, len) < 0) {
        CFT_LOGW(TAG, "stream %d: out of memory, resetting", (int)s->id);
        stream_abort(s, NGHTTP2_INTERNAL_ERROR);
        return -1;
    }
    return 0;
}

static int respond_full(cft_stream_t *s, int status, const char *content_type, const void *body, size_t len,
                        const char *meta)
{
    char clen[24];
    snprintf(clen, sizeof(clen), "%zu", len);
    cft_header_t hdrs[2] = {{"Content-Length", clen}, {"Content-Type", content_type}};
    if (submit_headers(s, status, hdrs, content_type ? 2 : 1, meta) < 0) {
        return -1;
    }
    if (queue_out(s, body, len) < 0) {
        return -1;
    }
    s->resp_end = true;
    resume(s);
    return 0;
}

/* --- public stream API --- */

const cft_request_t *cft_stream_request(const cft_stream_t *s)
{
    return &s->req;
}

const char *cft_stream_header(const cft_stream_t *s, const char *name)
{
    for (size_t i = 0; i < s->num_headers; i++) {
        if (strcmp(s->headers[i].name, name) == 0) {
            return s->headers[i].value;
        }
    }
    return NULL;
}

void cft_stream_set_handlers(cft_stream_t *s, const cft_stream_handlers_t *handlers, void *ctx)
{
    s->h = *handlers;
    s->ctx = ctx;
}

int cft_stream_read_body(cft_stream_t *s, size_t max_len,
                         void (*fn)(cft_stream_t *s, void *ctx, const uint8_t *body, size_t len), void *ctx)
{
    s->collect = true;
    s->body_max = max_len ? max_len : s->conn->cfg.max_request_body ? s->conn->cfg.max_request_body
                  : DEFAULT_MAX_BODY;
    s->body_fn = fn;
    s->body_ctx = ctx;
    if (s->req.content_length > (int64_t)s->body_max) {
        s->body_fn = NULL;
        return respond_full(s, 413, NULL, NULL, 0, META_ORIGIN);
    }
    return 0;
}

int cft_stream_send_headers(cft_stream_t *s, int status, const cft_header_t *headers, size_t num_headers)
{
    return submit_headers(s, status, headers, num_headers, META_ORIGIN);
}

int cft_stream_write(cft_stream_t *s, const void *data, size_t len)
{
    if (!s->resp_started || s->resp_end) {
        return -1;
    }
    if (queue_out(s, data, len) < 0) {
        return -1;
    }
    resume(s);
    return 0;
}

int cft_stream_end(cft_stream_t *s)
{
    if (!s->resp_started || s->resp_end) {
        return -1;
    }
    s->resp_end = true;
    resume(s);
    return 0;
}

size_t cft_stream_buffered(const cft_stream_t *s)
{
    return s->out.pending;
}

int cft_stream_respond(cft_stream_t *s, int status, const char *content_type, const void *body, size_t len)
{
    return respond_full(s, status, content_type, body, len, META_ORIGIN);
}

void cft_stream_reset(cft_stream_t *s)
{
    stream_abort(s, NGHTTP2_CANCEL);
}

void cft_stream_pause_body(cft_stream_t *s, bool pause)
{
    s->paused = pause;
    if (!pause && s->held) {
        nghttp2_session_consume_stream(s->conn->h2, s->id, s->held);
        CFT_DEBUG_ADD(held, -s->held);
        s->held = 0;
    }
}

/* --- internal stream API (cft_conn.h) --- */

int cft_stream_fail(cft_stream_t *s, int status)
{
    return respond_full(s, status, NULL, NULL, 0, META_CLOUDFLARED);
}

void cft_stream_set_ws_raw(cft_stream_t *s)
{
    s->ws_raw = true;
}

int cft_stream_set_service(cft_stream_t *s, const char *service)
{
    free(s->service);
    s->service = service ? strdup(service) : NULL;
    s->req.service = s->service;
    return service && !s->service ? -1 : 0;
}

const cft_conn_config_t *cft_stream_config(const cft_stream_t *s)
{
    return &s->conn->cfg;
}

void cft_stream_set_low_water(cft_stream_t *s, size_t bytes)
{
    s->low_water = bytes;
}

bool cft_stream_body_ended(const cft_stream_t *s)
{
    return s->body_ended;
}

int cft_stream_describe(const cft_stream_t *s, char *buf, size_t size)
{
    nghttp2_session *h2 = s->conn->h2;
    return snprintf(buf, size, "stream %d: send window %ld (connection %ld), queued %u; receive held %u%s%s",
                    (int)s->id, (long)nghttp2_session_get_stream_remote_window_size(h2, s->id),
                    (long)nghttp2_session_get_remote_window_size(h2), (unsigned)s->out.pending, (unsigned)s->held,
                    s->paused ? ", paused" : "", s->body_ended ? ", visitor done" : "");
}

/* --- WebSocket --- */

static int ws_send_frame(cft_stream_t *s, uint8_t opcode, const void *data, size_t len)
{
    uint8_t hdr[10];
    size_t hl;
    hdr[0] = 0x80 | opcode; /* FIN */
    if (len < 126) {
        hdr[1] = (uint8_t)len;
        hl = 2;
    } else if (len <= 0xffff) {
        hdr[1] = 126;
        hdr[2] = (uint8_t)(len >> 8);
        hdr[3] = (uint8_t)len;
        hl = 4;
    } else {
        hdr[1] = 127;
        for (int i = 0; i < 8; i++) {
            hdr[2 + i] = (uint8_t)((uint64_t)len >> (56 - 8 * i));
        }
        hl = 10;
    }
    if (queue_out(s, hdr, hl) < 0 || queue_out(s, data, len) < 0) {
        return -1;
    }
    resume(s);
    return 0;
}

int cft_ws_accept(cft_stream_t *s, const char *subprotocol)
{
    const char *key = cft_stream_header(s, "sec-websocket-key");
    if (!s->req.is_websocket || !key || s->resp_started) {
        return -1;
    }
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%s%s", key, guid);
    if (n < 0 || n >= (int)sizeof(buf)) {
        return -1;
    }
    uint8_t digest[20];
    cft_sha1((const uint8_t *)buf, (size_t)n, digest);
    char accept[29];
    cft_b64_encode_raw(digest, 20, accept);
    accept[27] = '=';
    accept[28] = '\0';
    cft_header_t hdrs[4] = {
        {"Upgrade", "websocket"},
        {"Connection", "Upgrade"},
        {"Sec-WebSocket-Accept", accept},
        {"Sec-WebSocket-Protocol", subprotocol},
    };
    if (submit_headers(s, 101, hdrs, subprotocol ? 4 : 3, META_ORIGIN) < 0) {
        return -1;
    }
    s->ws = true;
    return 0;
}

int cft_ws_send(cft_stream_t *s, cft_ws_opcode_t opcode, const void *data, size_t len)
{
    if (!s->ws || s->wsr.close_sent || s->resp_end) {
        return -1;
    }
    return ws_send_frame(s, (uint8_t)opcode, data, len);
}

int cft_ws_close(cft_stream_t *s, uint16_t code, const char *reason)
{
    if (!s->ws || s->wsr.close_sent) {
        return -1;
    }
    uint8_t payload[125];
    size_t rl = reason ? strlen(reason) : 0;
    if (rl > sizeof(payload) - 2) {
        rl = sizeof(payload) - 2;
    }
    payload[0] = (uint8_t)(code >> 8);
    payload[1] = (uint8_t)code;
    if (rl) {
        memcpy(payload + 2, reason, rl);
    }
    s->wsr.close_sent = true;
    if (ws_send_frame(s, CFT_WS_CLOSE, payload, rl + 2) < 0) {
        return -1;
    }
    return cft_stream_end(s);
}

static void ws_fail(cft_stream_t *s, uint16_t code, const char *why)
{
    CFT_LOGW(TAG, "stream %d: WebSocket error: %s", (int)s->id, why);
    s->wsr.failed = true;
    cft_ws_close(s, code, why);
}

static void ws_frame_done(cft_stream_t *s)
{
    ws_rx_t *w = &s->wsr;
    switch (w->opcode) {
    case CFT_WS_PING:
        if (!w->close_sent) {
            ws_send_frame(s, CFT_WS_PONG, w->ctrl, w->ctrl_len);
        }
        break;
    case CFT_WS_PONG:
        break;
    case CFT_WS_CLOSE:
        if (!w->close_sent) {
            uint16_t code = w->ctrl_len >= 2 ? (uint16_t)((w->ctrl[0] << 8) | w->ctrl[1]) : 1000;
            cft_ws_close(s, code, NULL);
        }
        break;
    default:
        if (w->fin && s->h.on_ws_message && !s->ws_raw) {
            s->h.on_ws_message(s, s->ctx, (cft_ws_opcode_t)w->msg_opcode, w->msg.p, w->msg.len);
        }
        if (w->fin) {
            buf_free(&w->msg); /* an idle WebSocket holds no buffer */
            w->msg_opcode = 0;
        }
        break;
    }
}

static void ws_feed(cft_stream_t *s, const uint8_t *data, size_t len)
{
    ws_rx_t *w = &s->wsr;
    size_t max = s->conn->cfg.max_request_body ? s->conn->cfg.max_request_body : DEFAULT_MAX_BODY;
    while (len && !w->failed) {
        if (w->hdr_need == 0 && w->payload_left == 0 && w->hdr_len == 0) {
            w->hdr_need = 2;
        }
        if (w->hdr_need) {
            w->hdr[w->hdr_len++] = *data++;
            len--;
            if (w->hdr_len == 2) {
                uint8_t l7 = w->hdr[1] & 0x7f;
                w->hdr_need = (uint8_t)(2 + (l7 == 126 ? 2 : l7 == 127 ? 8 : 0) + ((w->hdr[1] & 0x80) ? 4 : 0));
            }
            if (w->hdr_len < w->hdr_need) {
                continue;
            }
            /* Header complete */
            w->fin = w->hdr[0] & 0x80;
            w->opcode = w->hdr[0] & 0x0f;
            uint8_t l7 = w->hdr[1] & 0x7f;
            size_t pos = 2;
            uint64_t plen = l7;
            if (l7 == 126) {
                plen = ((uint64_t)w->hdr[2] << 8) | w->hdr[3];
                pos = 4;
            } else if (l7 == 127) {
                plen = 0;
                for (int i = 0; i < 8; i++) {
                    plen = (plen << 8) | w->hdr[2 + i];
                }
                pos = 10;
            }
            w->hdr_need = 0;
            w->hdr_len = 0;
            if (!(w->hdr[1] & 0x80)) {
                ws_fail(s, 1002, "unmasked frame from client");
                return;
            }
            memcpy(w->mask, w->hdr + pos, 4);
            bool control = w->opcode & 0x08;
            if (control && (plen > 125 || !w->fin)) {
                ws_fail(s, 1002, "invalid control frame");
                return;
            }
            if (!control) {
                if ((w->opcode == 0) != (w->msg_opcode != 0)) {
                    ws_fail(s, 1002, "unexpected continuation");
                    return;
                }
                if (w->opcode) {
                    w->msg_opcode = w->opcode;
                }
                if (!s->ws_raw && w->msg.len + plen > max) {
                    ws_fail(s, 1009, "message too big");
                    return;
                }
                if (!s->ws_raw && buf_reserve(&w->msg, w->msg.len + (size_t)plen) < 0) {
                    ws_fail(s, 1011, "out of memory");
                    return;
                }
            }
            w->payload_left = plen;
            w->payload_pos = 0;
            w->ctrl_len = 0;
            if (plen == 0) {
                ws_frame_done(s);
            }
            continue;
        }
        /* Payload */
        size_t k = len < w->payload_left ? len : (size_t)w->payload_left;
        if (s->ws_raw && !(w->opcode & 0x08)) {
            /* Unmask in pieces and pass on, without reassembling messages */
            uint8_t tmp[256];
            for (size_t i = 0; i < k;) {
                size_t m = k - i < sizeof(tmp) ? k - i : sizeof(tmp);
                for (size_t j = 0; j < m; j++) {
                    tmp[j] = data[i + j] ^ w->mask[(w->payload_pos + i + j) & 3];
                }
                if (s->h.on_body) {
                    s->h.on_body(s, s->ctx, tmp, m);
                }
                i += m;
            }
            data += k;
            len -= k;
            w->payload_pos += k;
            w->payload_left -= k;
            if (w->payload_left == 0) {
                ws_frame_done(s);
            }
            continue;
        }
        if (w->opcode & 0x08) {
            for (size_t i = 0; i < k; i++) {
                w->ctrl[w->ctrl_len++] = data[i] ^ w->mask[(w->payload_pos + i) & 3];
            }
        } else {
            /* The frame's size is known: room for it was reserved at its header */
            for (size_t i = 0; i < k; i++) {
                w->msg.p[w->msg.len + i] = data[i] ^ w->mask[(w->payload_pos + i) & 3];
            }
            w->msg.len += k;
        }
        data += k;
        len -= k;
        w->payload_pos += k;
        w->payload_left -= k;
        if (w->payload_left == 0) {
            ws_frame_done(s);
        }
    }
}

/* --- control stream / RPC --- */

static int rpc_send(void *arg, const uint8_t *data, size_t len)
{
    cft_conn_t *c = arg;
    if (!c->control || cft_sq_append(&c->control->out, data, len) < 0) {
        return -1;
    }
    resume(c->control);
    return 0;
}

static void rpc_event(void *arg, const cft_rpc_event_t *rev)
{
    cft_conn_t *c = arg;
    cft_event_t ev = {0};
    switch (rev->type) {
    case CFT_RPC_REGISTERED:
        c->registered = true;
        ev.type = CFT_EVENT_REGISTERED;
        cft_uuid_format(rev->details.uuid, ev.registered.connection_id);
        ev.registered.location = rev->details.location;
        ev.registered.remotely_managed = rev->details.remotely_managed;
        emit(c, &ev);
        if (c->shutting_down) {
            cft_rpc_unregister(c->rpc);
        }
        break;
    case CFT_RPC_REGISTER_FAILED:
        ev.type = CFT_EVENT_REGISTRATION_FAILED;
        ev.failed.cause = rev->error.cause;
        ev.failed.retry_after_ms = rev->error.retry_after_ns / 1000000;
        ev.failed.should_retry = rev->error.should_retry;
        emit(c, &ev);
        conn_fail(c, "registration failed");
        break;
    case CFT_RPC_UNREGISTERED:
        c->registered = false;
        ev.type = CFT_EVENT_UNREGISTERED;
        emit(c, &ev);
        conn_fail(c, "unregistered");
        break;
    case CFT_RPC_PROTOCOL_ERROR:
        conn_fail(c, rev->reason);
        break;
    }
}

static void start_control_stream(cft_conn_t *c, cft_stream_t *s)
{
    if (c->control) {
        CFT_LOGW(TAG, "second control stream %d refused", (int)s->id);
        nghttp2_submit_rst_stream(c->h2, NGHTTP2_FLAG_NONE, s->id, NGHTTP2_REFUSED_STREAM);
        return;
    }
    c->control = s;
    const cft_token_t *t = c->cfg.token;
    cft_rpc_register_params_t p = {
        .account_tag = t->account_tag,
        .tunnel_secret = t->secret,
        .tunnel_secret_len = t->secret_len,
        .tunnel_id = t->tunnel_id,
        .conn_index = c->cfg.conn_index,
        .client_id = c->client_id,
        .features = default_features,
        .num_features = sizeof(default_features) / sizeof(default_features[0]),
        .version = c->cfg.version ? c->cfg.version : "unknown",
        .arch = c->cfg.arch ? c->cfg.arch : "unknown",
        .origin_local_ip = c->cfg.local_ip,
        .origin_local_ip_len = c->cfg.local_ip_len,
        .num_previous_attempts = c->cfg.num_previous_attempts,
    };
    c->rpc = cft_rpc_new(&p, rpc_send, rpc_event, c);
    if (!c->rpc) {
        conn_fail(c, "out of memory");
        return;
    }
    /* Plain 200 without the response meta headers, as cloudflared's
     * ResponseWriter does on its first Write. */
    nghttp2_nv nva[] = {NV(":status", "200")};
    nghttp2_data_provider2 prd = {.source.ptr = s, .read_callback = read_out};
    if (nghttp2_submit_response2(c->h2, s->id, nva, 1, &prd) != 0 || cft_rpc_start(c->rpc) < 0) {
        conn_fail(c, "failed to start control stream");
    }
    s->resp_started = true;
}

/* --- request dispatch --- */

static void handle_config(cft_conn_t *c, cft_stream_t *s)
{
    /* Body: {"version": N, "config": {...}}; reply with the version we now
     * have (cloudflared orchestration.UpdateConfig). */
    int64_t version = 0;
    const char *cfg = NULL;
    size_t cfg_len = 0;
    const char *body = (const char *)s->body.p;
    bool ok = cft_json_get_int(body, s->body.len, "version", &version) &&
              cft_json_get_raw(body, s->body.len, "config", &cfg, &cfg_len);
    if (ok && version > c->config_version) {
        c->config_version = (int32_t)version;
        cft_event_t ev = {.type = CFT_EVENT_CONFIG_UPDATED};
        ev.config.version = (int32_t)version;
        ev.config.json = cfg;
        ev.config.json_len = cfg_len;
        emit(c, &ev);
    } else if (!ok) {
        CFT_LOGW(TAG, "malformed configuration update");
    }
    char reply[96];
    int n = snprintf(reply, sizeof(reply), "{\"lastAppliedVersion\":%ld,\"err\":%s}", (long)c->config_version,
                     ok ? "null" : "\"malformed configuration\"");
    /* Plain response, as cloudflared's ResponseWriter writes it */
    nghttp2_nv nva[] = {NV(":status", "200"), NV("content-type", "application/json")};
    cft_sq_append(&s->out, reply, (size_t)n);
    s->resp_end = true;
    s->resp_started = true;
    nghttp2_data_provider2 prd = {.source.ptr = s, .read_callback = read_out};
    nghttp2_submit_response2(c->h2, s->id, nva, 2, &prd);
}

enum { HDR_REGULAR, HDR_METHOD, HDR_PATH, HDR_AUTHORITY, HDR_SCHEME };

static int hdr_store(cft_stream_t *s, uint8_t kind, const uint8_t *name, size_t namelen, const uint8_t *value,
                     size_t valuelen)
{
    static const uint8_t nul = 0;
    buf_t *b = &s->hdrbuf;
    if (buf_append(b, &kind, 1) < 0 ||
            (kind == HDR_REGULAR && (buf_append(b, name, namelen) < 0 || buf_append(b, &nul, 1) < 0)) ||
            buf_append(b, value, valuelen) < 0 || buf_append(b, &nul, 1) < 0) {
        return -1;
    }
    if (kind == HDR_REGULAR) {
        s->num_headers++;
    }
    return 0;
}

/* Indexes the stored fields; returns -1 if out of memory */
static int build_request(cft_stream_t *s)
{
    buf_t *b = &s->hdrbuf;
    if (b->len && b->len < b->cap) {
        uint8_t *p = realloc(b->p, b->len); /* give back the slack */
        if (p) {
            b->p = p;
            b->cap = b->len;
        }
    }
    if (s->num_headers && !(s->headers = malloc(s->num_headers * sizeof(cft_header_t)))) {
        return -1;
    }
    size_t n = 0;
    for (const char *p = (const char *)b->p, *end = p + b->len; p < end;) {
        uint8_t kind = (uint8_t)p[0];
        p++;
        const char *name = NULL;
        if (kind == HDR_REGULAR) {
            name = p;
            p += strlen(p) + 1;
        }
        const char *value = p;
        p += strlen(p) + 1;
        if (kind == HDR_REGULAR) {
            s->headers[n].name = name;
            s->headers[n].value = value;
            n++;
        } else {
            s->pseudo[kind] = value;
        }
    }
    s->req = (cft_request_t) {
        .method = s->pseudo[HDR_METHOD] ? s->pseudo[HDR_METHOD] : "",
        .path = s->pseudo[HDR_PATH] ? s->pseudo[HDR_PATH] : "/",
        .authority = s->pseudo[HDR_AUTHORITY] ? s->pseudo[HDR_AUTHORITY] : "",
        .scheme = s->pseudo[HDR_SCHEME] ? s->pseudo[HDR_SCHEME] : "https",
        .headers = s->headers,
        .num_headers = s->num_headers,
        .content_length = -1,
        .is_websocket = s->upgrade_ws,
        .service = s->service,
    };
    const char *cl = cft_stream_header(s, "content-length");
    if (cl) {
        s->req.content_length = strtoll(cl, NULL, 10);
    }
    return 0;
}

/* Headers complete (END_HEADERS) */
static void on_request_headers(cft_conn_t *c, cft_stream_t *s)
{
    switch (s->kind) {
    case STREAM_CONTROL:
        start_control_stream(c, s);
        return;
    case STREAM_CONFIG:
        s->collect = true;
        s->body_max = 64 * 1024;
        return;
    case STREAM_UNSUPPORTED:
        respond_full(s, 502, NULL, NULL, 0, META_CLOUDFLARED);
        return;
    case STREAM_HTTP:
        break;
    }
    if (build_request(s) < 0) {
        respond_full(s, 503, NULL, NULL, 0, META_ORIGIN);
        return;
    }
    CFT_LOGD(TAG, "%s %s%s%s", s->req.method, s->req.authority, s->req.path, s->req.is_websocket ? " (WebSocket)" : "");
    uint32_t limit = c->cfg.max_concurrent_streams ? c->cfg.max_concurrent_streams : 16;
    s->counted = true;
    if (++c->num_requests > limit) {
        CFT_LOGW(TAG, "too many concurrent requests, answering 503");
        respond_full(s, 503, NULL, NULL, 0, META_ORIGIN);
        return;
    }
#if CFT_MIN_FREE_HEAP > 0
    if (cft_port_free_heap() < CFT_MIN_FREE_HEAP) {
        CFT_LOGW(TAG, "low on memory, answering 503");
        respond_full(s, 503, NULL, NULL, 0, META_ORIGIN);
        return;
    }
#endif
    if (c->cfg.on_request) {
        c->cfg.on_request(c->cfg.request_arg, s, &s->req);
    } else {
        respond_full(s, 404, NULL, NULL, 0, META_ORIGIN);
    }
}

/* END_STREAM received */
static void on_request_end(cft_conn_t *c, cft_stream_t *s)
{
    s->body_ended = true;
    if (s->kind == STREAM_CONFIG) {
        handle_config(c, s);
        return;
    }
    if (s->kind != STREAM_HTTP || s->resp_end) {
        return;
    }
    if (s->collect) {
        if (s->body_fn) {
            s->body_fn(s, s->body_ctx, s->body.p, s->body.len);
        }
        return;
    }
    if (s->h.on_body_end) {
        s->h.on_body_end(s, s->ctx);
    }
}

/* --- nghttp2 callbacks --- */

static int on_begin_headers(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }
    cft_stream_t *s = calloc(1, sizeof(*s));
    if (!s) {
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE; /* resets this stream, not the connection */
    }
    cft_conn_t *c = user_data;
    CFT_DEBUG_ADD(streams, 1);
    s->conn = c;
    s->id = frame->hd.stream_id;
    s->kind = STREAM_HTTP;
    s->next = c->streams;
    c->streams = s;
    nghttp2_session_set_stream_user_data(session, s->id, s);
    return 0;
}

static bool eq(const uint8_t *a, size_t alen, const char *b)
{
    return alen == strlen(b) && memcmp(a, b, alen) == 0;
}

static int on_header(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t namelen,
                     const uint8_t *value, size_t valuelen, uint8_t flags, void *user_data)
{
    cft_stream_t *s = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
    if (!s || frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }
    uint8_t kind = HDR_REGULAR;
    if (eq(name, namelen, ":method")) {
        kind = HDR_METHOD;
    } else if (eq(name, namelen, ":path")) {
        kind = HDR_PATH;
    } else if (eq(name, namelen, ":authority")) {
        kind = HDR_AUTHORITY;
    } else if (eq(name, namelen, ":scheme")) {
        kind = HDR_SCHEME;
    }
    if (kind != HDR_REGULAR) {
        return hdr_store(s, kind, NULL, 0, value, valuelen) < 0 ? NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE : 0;
    }
    if (eq(name, namelen, HDR_UPGRADE)) {
        if (eq(value, valuelen, UPGRADE_CONTROL)) {
            s->kind = STREAM_CONTROL;
        } else if (eq(value, valuelen, UPGRADE_CONFIG)) {
            s->kind = STREAM_CONFIG;
        } else if (eq(value, valuelen, UPGRADE_WEBSOCKET)) {
            s->upgrade_ws = true;
        } else {
            s->kind = STREAM_UNSUPPORTED;
        }
        return 0;
    }
    if (eq(name, namelen, HDR_TCP_SRC)) {
        s->kind = STREAM_UNSUPPORTED; /* WARP routing to private networks */
        return 0;
    }
    if (hdr_store(s, HDR_REGULAR, name, namelen, value, valuelen) < 0) {
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    return 0;
}

static int on_frame_recv(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    cft_conn_t *c = user_data;
    cft_stream_t *s = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
    switch (frame->hd.type) {
    case NGHTTP2_HEADERS:
        if (!s || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
            break;
        }
        on_request_headers(c, s);
        if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
            on_request_end(c, s);
        }
        break;
    case NGHTTP2_DATA:
        if (s && s->kind != STREAM_CONTROL && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
            on_request_end(c, s);
        }
        break;
    case NGHTTP2_RST_STREAM:
        CFT_LOGD(TAG, "edge reset stream %d (error %u)", (int)frame->hd.stream_id,
                 (unsigned)frame->rst_stream.error_code);
        break;
    case NGHTTP2_GOAWAY:
        CFT_LOGI(TAG, "edge sent GOAWAY (error %u)", (unsigned)frame->goaway.error_code);
        if (!c->shutting_down) {
            /* Edge is draining this connection (e.g. maintenance). Let the
             * supervisor open a replacement, and unregister this one. */
            cft_event_t ev = {.type = CFT_EVENT_DRAINING};
            emit(c, &ev);
            cft_conn_shutdown(c);
        }
        break;
    case NGHTTP2_PING:
        if (frame->hd.flags & NGHTTP2_FLAG_ACK) {
            c->ping_sent_ms = 0;
        }
        break;
    default:
        break;
    }
    return 0;
}

static void stream_deliver(cft_conn_t *c, cft_stream_t *s, const uint8_t *data, size_t len);

static int on_data_chunk(nghttp2_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len,
                         void *user_data)
{
    cft_conn_t *c = user_data;
    cft_stream_t *s = nghttp2_session_get_stream_user_data(session, stream_id);
    if (!s) {
        nghttp2_session_consume(session, stream_id, len);
        return 0;
    }
    stream_deliver(c, s, data, len);
    /* The stream may have been reset, but not freed: that happens in
     * on_stream_close, after this callback. */
    if (s->paused) {
        /* A paused stream gets no more window; the edge can send at most
         * what it already had. The connection's window is returned, so the
         * other streams keep flowing. */
        nghttp2_session_consume_connection(session, len);
        s->held += len;
        CFT_DEBUG_ADD(held, len);
    } else {
        nghttp2_session_consume(session, stream_id, len);
    }
    return 0;
}

static void stream_deliver(cft_conn_t *c, cft_stream_t *s, const uint8_t *data, size_t len)
{
    if (s->kind == STREAM_CONTROL) {
        if (s == c->control && c->rpc && cft_rpc_feed(c->rpc, data, len) < 0) {
            conn_fail(c, "control stream RPC error");
        }
        return;
    }
    if (s->resp_end && !s->ws) {
        return; /* already answered (413, 503, ...) */
    }
    if (s->ws) {
        ws_feed(s, data, len);
    } else if (s->collect) {
        if (s->body.cap == 0 && s->req.content_length > 0 && (size_t)s->req.content_length <= s->body_max) {
            buf_reserve(&s->body, (size_t)s->req.content_length); /* if it fails, appending tries again */
        }
        if (s->body.len + len > s->body_max) {
            s->body_fn = NULL;
            respond_full(s, 413, NULL, NULL, 0, META_ORIGIN);
        } else if (buf_append(&s->body, data, len) < 0) {
            s->body_fn = NULL;
            respond_full(s, 500, NULL, NULL, 0, META_ORIGIN);
        }
    } else if (s->h.on_body) {
        s->h.on_body(s, s->ctx, data, len);
    }
}

static void close_stream(cft_conn_t *c, cft_stream_t *s, bool completed)
{
    if (s->counted) {
        c->num_requests--;
    }
    if (s->h.on_close) {
        s->h.on_close(s, s->ctx, completed);
    }
    stream_unlink(c, s);
    stream_free(s);
}

static int on_stream_close(nghttp2_session *session, int32_t stream_id, uint32_t error_code, void *user_data)
{
    cft_conn_t *c = user_data;
    cft_stream_t *s = nghttp2_session_get_stream_user_data(session, stream_id);
    if (!s) {
        return 0;
    }
    nghttp2_session_set_stream_user_data(session, stream_id, NULL);
    if (s == c->control) {
        c->control = NULL;
        conn_fail(c, "control stream closed by edge");
    } else if (error_code != NGHTTP2_NO_ERROR) {
        CFT_LOGD(TAG, "stream %d reset (error %u)", (int)stream_id, (unsigned)error_code);
    }
    close_stream(c, s, error_code == NGHTTP2_NO_ERROR && s->resp_eof_sent);
    return 0;
}

static int on_frame_send(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    CFT_LOGD(TAG, "sent frame type %u stream %d flags 0x%02x len %u", frame->hd.type, (int)frame->hd.stream_id,
             frame->hd.flags, (unsigned)frame->hd.length);
    return 0;
}

static int on_invalid_frame(nghttp2_session *session, const nghttp2_frame *frame, int lib_error_code,
                            void *user_data)
{
    CFT_LOGW(TAG, "invalid frame type %u on stream %d: %s", frame->hd.type, (int)frame->hd.stream_id,
             nghttp2_strerror(lib_error_code));
    return 0;
}

#ifdef CFT_DEBUG_STATS
/* nghttp2 allocator that counts what nghttp2 holds */
static void *h2_malloc(size_t size, void *ud)
{
    size_t *p = malloc(size + sizeof(size_t) * 2);
    if (!p) {
        return NULL;
    }
    p[0] = size;
    CFT_DEBUG_ADD(h2_bytes, size);
    CFT_DEBUG_PEAK(h2_bytes, h2_peak);
    return p + 2;
}

static void h2_free(void *ptr, void *ud)
{
    if (ptr) {
        size_t *p = (size_t *)ptr - 2;
        CFT_DEBUG_ADD(h2_bytes, -p[0]);
        free(p);
    }
}

static void *h2_calloc(size_t n, size_t size, void *ud)
{
    void *p = h2_malloc(n * size, ud);
    if (p) {
        memset(p, 0, n * size);
    }
    return p;
}

static void *h2_realloc(void *ptr, size_t size, void *ud)
{
    if (!ptr) {
        return h2_malloc(size, ud);
    }
    size_t old = ((size_t *)ptr - 2)[0];
    void *np = h2_malloc(size, ud);
    if (np) {
        memcpy(np, ptr, old < size ? old : size);
        h2_free(ptr, ud);
    }
    return np;
}

static nghttp2_mem h2_mem = {NULL, h2_malloc, h2_free, h2_calloc, h2_realloc};
#endif

/* --- connection API --- */

cft_conn_t *cft_conn_new(const cft_conn_config_t *cfg)
{
    cft_conn_t *c = calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->cfg = *cfg;
    c->alive = true;
    c->preface_left = NGHTTP2_CLIENT_MAGIC_LEN;
    memcpy(c->client_id, cfg->client_id, 16);

    nghttp2_session_callbacks *cbs;
    if (nghttp2_session_callbacks_new(&cbs) != 0) {
        free(c);
        return NULL;
    }
    nghttp2_session_callbacks_set_on_begin_headers_callback(cbs, on_begin_headers);
    nghttp2_session_callbacks_set_on_header_callback(cbs, on_header);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, on_frame_recv);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, on_data_chunk);
    nghttp2_session_callbacks_set_on_stream_close_callback(cbs, on_stream_close);
    nghttp2_session_callbacks_set_on_invalid_frame_recv_callback(cbs, on_invalid_frame);
    nghttp2_session_callbacks_set_on_frame_send_callback(cbs, on_frame_send);
    nghttp2_option *opt;
    if (nghttp2_option_new(&opt) != 0) {
        nghttp2_session_callbacks_del(cbs);
        free(c);
        return NULL;
    }
    /* Received data is acknowledged once consumed: see cft_stream_pause_body() */
    nghttp2_option_set_no_auto_window_update(opt, 1);
#ifdef CFT_DEBUG_STATS
    int rv = nghttp2_session_server_new3(&c->h2, cbs, c, opt, &h2_mem);
#else
    int rv = nghttp2_session_server_new2(&c->h2, cbs, c, opt);
#endif
    nghttp2_option_del(opt);
    nghttp2_session_callbacks_del(cbs);
    if (rv != 0) {
        free(c);
        return NULL;
    }

    /* No SETTINGS_MAX_CONCURRENT_STREAMS: the edge does not queue requests
     * at the limit but drops the connection with GOAWAY. cloudflared
     * advertises no limit either; excess requests get 503 instead. */
    nghttp2_settings_entry iv[1];
    size_t niv = 0;
    uint32_t window = cfg->stream_window ? cfg->stream_window : NGHTTP2_INITIAL_WINDOW_SIZE;
    if (window > (uint32_t)NGHTTP2_MAX_WINDOW_SIZE) {
        window = NGHTTP2_MAX_WINDOW_SIZE;
    }
    if (window != NGHTTP2_INITIAL_WINDOW_SIZE) {
        iv[niv++] = (nghttp2_settings_entry) {
            NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, window
        };
    }
    nghttp2_submit_settings(c->h2, NGHTTP2_FLAG_NONE, iv, niv);
    /* Received data is returned to the connection window right away (see
     * on_data_chunk); a little more than a stream's window keeps one stream
     * from using it all up while its data is in flight. */
    uint32_t conn_window = window + NGHTTP2_INITIAL_WINDOW_SIZE;
    if (conn_window > (uint32_t)NGHTTP2_MAX_WINDOW_SIZE) {
        conn_window = NGHTTP2_MAX_WINDOW_SIZE;
    }
    nghttp2_session_set_local_window_size(c->h2, NGHTTP2_FLAG_NONE, 0, (int32_t)conn_window);
    return c;
}

void cft_conn_free(cft_conn_t *c)
{
    if (!c) {
        return;
    }
    /* nghttp2_session_del() does not report stream closes; tell the
     * application about the streams that were still open. */
    nghttp2_session_del(c->h2);
    while (c->streams) {
        cft_stream_t *s = c->streams;
        if (s == c->control) {
            c->control = NULL;
        }
        close_stream(c, s, false);
    }
    cft_rpc_free(c->rpc);
    free(c);
}

/*
 * The edge sends GOAWAY with last_stream_id set to one of its own (odd)
 * stream IDs. RFC 9113 says it names a stream initiated by the receiver, so
 * nghttp2 treats it as a connection error, which kills the session during a
 * graceful shutdown. We never initiate streams, so the correct value is 0;
 * rewrite it before nghttp2 sees the frame. Returns a patched copy of data
 * (to be freed) or NULL if nothing changed.
 */
static uint8_t *fix_goaway(cft_conn_t *c, const uint8_t *data, size_t len)
{
    uint8_t *patched = NULL;
    size_t i = 0;
    while (i < len) {
        if (c->preface_left) {
            size_t k = len - i < c->preface_left ? len - i : c->preface_left;
            c->preface_left -= (uint32_t)k;
            i += k;
        } else if (c->payload_left == 0) {
            c->fhdr[c->fhdr_len++] = data[i++];
            if (c->fhdr_len == 9) {
                c->payload_left = ((uint32_t)c->fhdr[0] << 16) | ((uint32_t)c->fhdr[1] << 8) | c->fhdr[2];
                c->ftype = c->fhdr[3];
                c->payload_pos = 0;
                c->fhdr_len = 0;
            }
        } else {
            size_t k = len - i < c->payload_left ? len - i : c->payload_left;
            if (c->ftype == NGHTTP2_GOAWAY) {
                for (size_t j = 0; j < k && c->payload_pos + j < 4; j++) {
                    if (data[i + j] != 0) {
                        if (!patched) {
                            patched = malloc(len);
                            if (!patched) {
                                return NULL;
                            }
                            memcpy(patched, data, len);
                        }
                        patched[i + j] = 0;
                    }
                }
            }
            c->payload_pos += (uint32_t)k;
            c->payload_left -= (uint32_t)k;
            i += k;
        }
    }
    return patched;
}

int cft_conn_recv(cft_conn_t *c, const uint8_t *data, size_t len)
{
    c->rx_since_tick = true;
    uint8_t *patched = fix_goaway(c, data, len);
    nghttp2_ssize n = nghttp2_session_mem_recv2(c->h2, patched ? patched : data, len);
    free(patched);
    if (n < 0) {
        conn_fail(c, nghttp2_strerror((int)n));
        return -1;
    }
    return c->alive ? 0 : -1;
}

/* Calls on_writable for streams whose queued data has been taken; returns
 * true if any was called. */
static bool notify_writable(cft_conn_t *c)
{
    bool called = false;
    for (cft_stream_t *s = c->streams; s; s = s->next) {
        if (s->want_writable) {
            s->want_writable = false;
            if (s->h.on_writable && !s->resp_end) {
                s->h.on_writable(s, s->ctx);
                called = true;
                break; /* the callback may have changed the stream list */
            }
        }
    }
    return called;
}

long cft_conn_send_data(cft_conn_t *c, const uint8_t **data)
{
    for (;;) {
        /* Streams below their low water get more data before it runs out */
        while (notify_writable(c)) {
        }
        nghttp2_ssize n = nghttp2_session_mem_send2(c->h2, data);
        if (n < 0) {
            conn_fail(c, nghttp2_strerror((int)n));
            return -1;
        }
        if (n > 0 || !notify_writable(c)) {
            return (long)n;
        }
    }
}

void cft_conn_tick(cft_conn_t *c, int64_t now_ms)
{
    if (!c->alive) {
        return;
    }
    if (c->created_ms == 0) {
        c->created_ms = now_ms;
        c->last_rx_ms = now_ms;
    }
    if (c->resets_pending) {
        c->resets_pending = false;
        for (cft_stream_t *s = c->streams; s; s = s->next) {
            if (s->reset_pending) {
                if (nghttp2_submit_rst_stream(c->h2, NGHTTP2_FLAG_NONE, s->id, s->reset_pending) == 0) {
                    s->reset_pending = 0;
                } else {
                    c->resets_pending = true;
                }
            }
        }
    }
    if (c->rx_since_tick) {
        c->rx_since_tick = false;
        c->last_rx_ms = now_ms;
        c->ping_sent_ms = 0; /* anything from the edge proves the link works */
    }
    if (!c->registered && !c->shutting_down && now_ms - c->created_ms > REGISTER_TIMEOUT_MS) {
        conn_fail(c, "registration timed out");
        return;
    }
#ifdef CFT_DEBUG_STATS
    long win = (long)nghttp2_session_get_remote_window_size(c->h2);
    if (win < cft_debug.send_window_min || cft_debug.send_window_min == 0) {
        cft_debug.send_window_min = win;
    }
    if (win > 0) {
        c->window_zero_since = 0;
    } else if (!c->window_zero_since) {
        c->window_zero_since = now_ms;
    } else if (now_ms - c->window_zero_since > 5000) {
        CFT_LOGW(TAG, "connection %u: no window from the edge for %lld ms", c->cfg.conn_index,
                 (long long)(now_ms - c->window_zero_since));
        cft_debug.send_window_stalls++;
        c->window_zero_since = now_ms;
    }
#endif
    if (c->ping_sent_ms) {
        if (now_ms - c->ping_sent_ms > CFT_PING_TIMEOUT_MS) {
            conn_fail(c, "edge stopped responding");
        }
    } else if (now_ms - c->last_rx_ms > CFT_IDLE_PING_MS) {
        if (nghttp2_submit_ping(c->h2, NGHTTP2_FLAG_NONE, NULL) == 0) {
            c->ping_sent_ms = now_ms;
        }
    }
}

int cft_conn_shutdown(cft_conn_t *c)
{
    if (c->shutting_down) {
        return 0;
    }
    c->shutting_down = true;
    if (c->registered) {
        return cft_rpc_unregister(c->rpc);
    }
    if (!c->rpc) {
        conn_fail(c, "shut down before registration");
    }
    /* Registration in flight: unregister once it completes (rpc_event). */
    return 0;
}

bool cft_conn_is_alive(const cft_conn_t *c)
{
    return c->alive;
}
