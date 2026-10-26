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

#include "cf_tunnel.h"
#include "cft_port.h"
#include "cft_rpc.h"
#include "cft_util.h"

static const char *TAG = "cft_conn";

/* Request headers set by the edge (cloudflared connection/http2.go). */
#define HDR_UPGRADE "cf-cloudflared-proxy-connection-upgrade"
#define HDR_TCP_SRC "cf-cloudflared-proxy-src"
#define UPGRADE_CONTROL "control-stream"
#define UPGRADE_CONFIG "update-configuration"
#define UPGRADE_WEBSOCKET "websocket"
/* Response headers read by the edge (cloudflared connection/header.go). */
#define HDR_RESP_USER_HEADERS "cf-cloudflared-response-headers"
#define HDR_RESP_META "cf-cloudflared-response-meta"
#define META_ORIGIN "{\"src\":\"origin\"}"
#define META_CLOUDFLARED "{\"src\":\"cloudflared\"}"

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

typedef struct stream {
    struct stream *next; /* cft_conn.streams */
    int32_t id;
    stream_kind_t kind;
    char *method;
    char *path;
    char *authority;
    char *scheme;
    cft_header_t *headers;
    size_t num_headers;
    size_t cap_headers;
    buf_t body;
    bool body_too_large;
    bool counted;    /* included in cft_conn.num_requests */
    bool overloaded; /* over the request limit: answered with 503 */
    bool dispatched;
    buf_t out;
} stream_t;

struct cft_response {
    int status;
    cft_header_t *headers;
    size_t num_headers;
    size_t cap_headers;
    buf_t body;
};

struct cft_conn {
    cft_conn_config_t cfg;
    nghttp2_session *h2;
    cft_rpc_t *rpc;
    stream_t *control;
    stream_t *streams; /* all open streams; nghttp2_session_del does not report closes */
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
    bool alive;
    bool registered;
    bool shutting_down;
    char close_reason[128];
};

/* --- buffers --- */

static int buf_append(buf_t *b, const void *data, size_t len)
{
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

static void buf_free(buf_t *b)
{
    free(b->p);
    memset(b, 0, sizeof(*b));
}

static int headers_append(cft_header_t **arr, size_t *n, size_t *cap, const char *name, size_t nlen,
                          const char *value, size_t vlen)
{
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 8;
        cft_header_t *na = realloc(*arr, nc * sizeof(*na));
        if (!na) {
            return -1;
        }
        *arr = na;
        *cap = nc;
    }
    char *nm = malloc(nlen + 1);
    char *vl = malloc(vlen + 1);
    if (!nm || !vl) {
        free(nm);
        free(vl);
        return -1;
    }
    memcpy(nm, name, nlen);
    nm[nlen] = '\0';
    memcpy(vl, value, vlen);
    vl[vlen] = '\0';
    (*arr)[*n].name = nm;
    (*arr)[*n].value = vl;
    (*n)++;
    return 0;
}

static void headers_free(cft_header_t *arr, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free((char *)arr[i].name);
        free((char *)arr[i].value);
    }
    free(arr);
}

static char *dupn(const uint8_t *s, size_t n)
{
    char *d = malloc(n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = '\0';
    }
    return d;
}

static void stream_free(stream_t *s)
{
    free(s->method);
    free(s->path);
    free(s->authority);
    free(s->scheme);
    headers_free(s->headers, s->num_headers);
    buf_free(&s->body);
    buf_free(&s->out);
    free(s);
}

/* --- response API --- */

void cft_response_set_status(cft_response_t *resp, int status)
{
    resp->status = status;
}

int cft_response_add_header(cft_response_t *resp, const char *name, const char *value)
{
    return headers_append(&resp->headers, &resp->num_headers, &resp->cap_headers, name, strlen(name), value,
                          strlen(value));
}

int cft_response_set_body(cft_response_t *resp, const void *data, size_t len)
{
    resp->body.len = 0;
    return buf_append(&resp->body, data, len);
}

/* --- events --- */

static void emit(cft_conn_t *c, const cft_event_t *ev)
{
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

/* --- submitting responses --- */

static nghttp2_ssize read_out_buf(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length,
                                  uint32_t *data_flags, nghttp2_data_source *source, void *user_data)
{
    (void)session;
    (void)stream_id;
    (void)user_data;
    stream_t *s = source->ptr;
    size_t avail = s->out.len - s->out.off;
    if (avail == 0) {
        if (s->kind == STREAM_CONTROL) {
            return NGHTTP2_ERR_DEFERRED;
        }
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }
    size_t n = avail < length ? avail : length;
    memcpy(buf, s->out.p + s->out.off, n);
    s->out.off += n;
    if (s->out.off == s->out.len) {
        s->out.off = 0;
        s->out.len = 0;
        if (s->kind != STREAM_CONTROL) {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        }
    }
    return (nghttp2_ssize)n;
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

/* Sends status + headers; the body is whatever is in s->out. */
static int submit_response(cft_conn_t *c, stream_t *s, int status, const cft_header_t *user_headers,
                           size_t num_user_headers, const char *meta)
{
    char status_str[8];
    char clen[24];
    snprintf(status_str, sizeof(status_str), "%d", status);
    snprintf(clen, sizeof(clen), "%zu", s->out.len);

    /* Content-Length is meaningful to the edge, so it is sent both as an
     * HTTP/2 header and among the serialized user headers. */
    cft_header_t *uh = malloc((num_user_headers + 1) * sizeof(*uh));
    if (!uh) {
        return -1;
    }
    size_t nuh = 0;
    bool have_clen = false;
    for (size_t i = 0; i < num_user_headers; i++) {
        if (strcasecmp(user_headers[i].name, "content-length") == 0) {
            have_clen = true;
        }
        uh[nuh++] = user_headers[i];
    }
    if (!have_clen) {
        uh[nuh].name = "Content-Length";
        uh[nuh].value = clen;
        nuh++;
    }
    char *serialized = serialize_headers(uh, nuh);
    free(uh);
    if (!serialized) {
        return -1;
    }

    nghttp2_nv nva[] = {
        NV(":status", status_str),
        NV("content-length", clen),
        NV(HDR_RESP_USER_HEADERS, serialized),
        NV(HDR_RESP_META, meta),
    };
    nghttp2_data_provider2 prd = {.source.ptr = s, .read_callback = read_out_buf};
    int rv = nghttp2_submit_response2(c->h2, s->id, nva, sizeof(nva) / sizeof(nva[0]), &prd);
    free(serialized);
    return rv;
}

static int submit_simple(cft_conn_t *c, stream_t *s, int status, const char *meta)
{
    s->out.len = 0;
    return submit_response(c, s, status, NULL, 0, meta);
}

/* --- control stream / RPC --- */

static int rpc_send(void *arg, const uint8_t *data, size_t len)
{
    cft_conn_t *c = arg;
    if (!c->control || buf_append(&c->control->out, data, len) < 0) {
        return -1;
    }
    nghttp2_session_resume_data(c->h2, c->control->id);
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

static void start_control_stream(cft_conn_t *c, stream_t *s)
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
    nghttp2_data_provider2 prd = {.source.ptr = s, .read_callback = read_out_buf};
    if (nghttp2_submit_response2(c->h2, s->id, nva, 1, &prd) != 0 || cft_rpc_start(c->rpc) < 0) {
        conn_fail(c, "failed to start control stream");
    }
}

/* --- request dispatch --- */

static void handle_config(cft_conn_t *c, stream_t *s)
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
    s->out.len = 0;
    buf_append(&s->out, reply, (size_t)n);
    nghttp2_nv nva[] = {NV(":status", "200"), NV("content-type", "application/json")};
    nghttp2_data_provider2 prd = {.source.ptr = s, .read_callback = read_out_buf};
    nghttp2_submit_response2(c->h2, s->id, nva, 2, &prd);
}

static void handle_http(cft_conn_t *c, stream_t *s)
{
    if (s->overloaded) {
        CFT_LOGW(TAG, "too many concurrent requests, answering 503");
        submit_simple(c, s, 503, META_ORIGIN);
        return;
    }
    if (s->body_too_large) {
        submit_simple(c, s, 413, META_ORIGIN);
        return;
    }
    cft_request_t req = {
        .method = s->method ? s->method : "",
        .path = s->path ? s->path : "/",
        .authority = s->authority ? s->authority : "",
        .scheme = s->scheme ? s->scheme : "https",
        .headers = s->headers,
        .num_headers = s->num_headers,
        .body = s->body.p,
        .body_len = s->body.len,
    };
    cft_response_t resp = {.status = 200};
    if (c->cfg.handler) {
        c->cfg.handler(c->cfg.handler_arg, &req, &resp);
    } else {
        resp.status = 404;
    }
    buf_free(&s->out);
    s->out = resp.body; /* take ownership */
    s->out.off = 0;
    if (submit_response(c, s, resp.status, resp.headers, resp.num_headers, META_ORIGIN) != 0) {
        nghttp2_submit_rst_stream(c->h2, NGHTTP2_FLAG_NONE, s->id, NGHTTP2_INTERNAL_ERROR);
    }
    headers_free(resp.headers, resp.num_headers);
}

static void dispatch(cft_conn_t *c, stream_t *s)
{
    if (s->dispatched) {
        return;
    }
    s->dispatched = true;
    switch (s->kind) {
    case STREAM_CONFIG:
        handle_config(c, s);
        break;
    case STREAM_HTTP:
        CFT_LOGI(TAG, "%s %s%s", s->method ? s->method : "?", s->authority ? s->authority : "",
                 s->path ? s->path : "");
        handle_http(c, s);
        break;
    case STREAM_UNSUPPORTED:
        submit_simple(c, s, 502, META_CLOUDFLARED);
        break;
    case STREAM_CONTROL:
        break;
    }
}

/* --- nghttp2 callbacks --- */

static int on_begin_headers(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }
    stream_t *s = calloc(1, sizeof(*s));
    if (!s) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    cft_conn_t *c = user_data;
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
    (void)flags;
    (void)user_data;
    stream_t *s = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
    if (!s || frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }
    char **pseudo = NULL;
    if (eq(name, namelen, ":method")) {
        pseudo = &s->method;
    } else if (eq(name, namelen, ":path")) {
        pseudo = &s->path;
    } else if (eq(name, namelen, ":authority")) {
        pseudo = &s->authority;
    } else if (eq(name, namelen, ":scheme")) {
        pseudo = &s->scheme;
    }
    if (pseudo) {
        free(*pseudo);
        *pseudo = dupn(value, valuelen);
        return 0;
    }
    if (eq(name, namelen, HDR_UPGRADE)) {
        if (eq(value, valuelen, UPGRADE_CONTROL)) {
            s->kind = STREAM_CONTROL;
        } else if (eq(value, valuelen, UPGRADE_CONFIG)) {
            s->kind = STREAM_CONFIG;
        } else {
            /* websocket: not supported yet */
            s->kind = STREAM_UNSUPPORTED;
        }
        return 0;
    }
    if (eq(name, namelen, HDR_TCP_SRC)) {
        s->kind = STREAM_UNSUPPORTED;
        return 0;
    }
    if (headers_append(&s->headers, &s->num_headers, &s->cap_headers, (const char *)name, namelen,
                       (const char *)value, valuelen) < 0) {
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    return 0;
}

static int on_frame_recv(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    cft_conn_t *c = user_data;
    stream_t *s = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
    switch (frame->hd.type) {
    case NGHTTP2_HEADERS:
        if (!s || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
            break;
        }
        CFT_LOGD(TAG, "stream %d: kind %d %s %s%s", (int)s->id, s->kind, s->method ? s->method : "",
                 s->authority ? s->authority : "", s->path ? s->path : "");
        if (s->kind == STREAM_CONTROL) {
            start_control_stream(c, s);
            break;
        }
        if (s->kind == STREAM_HTTP) {
            uint32_t limit = c->cfg.max_concurrent_streams ? c->cfg.max_concurrent_streams : 16;
            s->counted = true;
            if (++c->num_requests > limit) {
                s->overloaded = true;
            }
        }
        if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
            dispatch(c, s);
        }
        break;
    case NGHTTP2_DATA:
        if (s && s->kind != STREAM_CONTROL && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
            dispatch(c, s);
        }
        break;
    case NGHTTP2_RST_STREAM:
        CFT_LOGD(TAG, "edge reset stream %d (error %u)", (int)frame->hd.stream_id, frame->rst_stream.error_code);
        break;
    case NGHTTP2_GOAWAY:
        CFT_LOGI(TAG, "edge sent GOAWAY (error %u%s%.*s)", frame->goaway.error_code,
                 frame->goaway.opaque_data_len ? ": " : "", (int)frame->goaway.opaque_data_len,
                 frame->goaway.opaque_data);
        if (!c->shutting_down) {
            /* Edge is draining this connection (e.g. maintenance): unregister
             * so the caller can reconnect elsewhere. */
            cft_conn_shutdown(c);
        }
        break;
    default:
        break;
    }
    return 0;
}

static int on_data_chunk(nghttp2_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len,
                         void *user_data)
{
    (void)flags;
    cft_conn_t *c = user_data;
    stream_t *s = nghttp2_session_get_stream_user_data(session, stream_id);
    if (!s) {
        return 0;
    }
    if (s->kind == STREAM_CONTROL) {
        if (s == c->control && c->rpc && cft_rpc_feed(c->rpc, data, len) < 0) {
            conn_fail(c, "control stream RPC error");
        }
        return 0;
    }
    size_t limit = c->cfg.max_request_body ? c->cfg.max_request_body : 16 * 1024;
    if (s->overloaded || s->body_too_large || s->body.len + len > limit) {
        s->body_too_large = true;
        return 0;
    }
    if (buf_append(&s->body, data, len) < 0) {
        s->body_too_large = true;
    }
    return 0;
}

static int on_stream_close(nghttp2_session *session, int32_t stream_id, uint32_t error_code, void *user_data)
{
    cft_conn_t *c = user_data;
    stream_t *s = nghttp2_session_get_stream_user_data(session, stream_id);
    if (!s) {
        return 0;
    }
    if (s->counted) {
        c->num_requests--;
    }
    if (s == c->control) {
        c->control = NULL;
        conn_fail(c, "control stream closed by edge");
    } else if (error_code != NGHTTP2_NO_ERROR) {
        CFT_LOGD(TAG, "stream %d reset (error %u)", (int)stream_id, error_code);
    }
    nghttp2_session_set_stream_user_data(session, stream_id, NULL);
    for (stream_t **pp = &c->streams; *pp; pp = &(*pp)->next) {
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    }
    stream_free(s);
    return 0;
}

static int on_frame_send(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    (void)session;
    (void)user_data;
    CFT_LOGD(TAG, "sent frame type %u stream %d flags 0x%02x len %u", frame->hd.type, (int)frame->hd.stream_id,
             frame->hd.flags, (unsigned)frame->hd.length);
    return 0;
}

static int on_invalid_frame(nghttp2_session *session, const nghttp2_frame *frame, int lib_error_code,
                            void *user_data)
{
    (void)session;
    (void)user_data;
    CFT_LOGW(TAG, "invalid frame type %u on stream %d: %s", frame->hd.type, (int)frame->hd.stream_id,
             nghttp2_strerror(lib_error_code));
    if (frame->hd.type == NGHTTP2_GOAWAY) {
        CFT_LOGW(TAG, "GOAWAY last_stream_id=%d error=%u debug=%.*s", (int)frame->goaway.last_stream_id,
                 frame->goaway.error_code, (int)frame->goaway.opaque_data_len, frame->goaway.opaque_data);
    }
    return 0;
}

/* --- public API --- */

cft_conn_t *cft_conn_new(const cft_conn_config_t *cfg)
{
    cft_conn_t *c = calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->cfg = *cfg;
    c->alive = true;
    c->preface_left = NGHTTP2_CLIENT_MAGIC_LEN;
    if (cfg->client_id) {
        memcpy(c->client_id, cfg->client_id, 16);
    } else {
        cft_port_random(c->client_id, 16);
        c->client_id[6] = (c->client_id[6] & 0x0f) | 0x40; /* UUID v4 */
        c->client_id[8] = (c->client_id[8] & 0x3f) | 0x80;
    }

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
    int rv = nghttp2_session_server_new(&c->h2, cbs, c);
    nghttp2_session_callbacks_del(cbs);
    if (rv != 0) {
        free(c);
        return NULL;
    }

    /* No SETTINGS_MAX_CONCURRENT_STREAMS: the edge does not queue requests
     * at the limit but drops the connection with GOAWAY. cloudflared
     * advertises no limit either; excess requests get 503 instead. */
    nghttp2_submit_settings(c->h2, NGHTTP2_FLAG_NONE, NULL, 0);
    return c;
}

void cft_conn_free(cft_conn_t *c)
{
    if (!c) {
        return;
    }
    nghttp2_session_del(c->h2);
    while (c->streams) {
        stream_t *s = c->streams;
        c->streams = s->next;
        stream_free(s);
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
    uint8_t *patched = fix_goaway(c, data, len);
    nghttp2_ssize n = nghttp2_session_mem_recv2(c->h2, patched ? patched : data, len);
    free(patched);
    if (n < 0) {
        conn_fail(c, nghttp2_strerror((int)n));
        return -1;
    }
    return c->alive ? 0 : -1;
}

long cft_conn_send_data(cft_conn_t *c, const uint8_t **data)
{
    nghttp2_ssize n = nghttp2_session_mem_send2(c->h2, data);
    if (n < 0) {
        conn_fail(c, nghttp2_strerror((int)n));
        return -1;
    }
    return (long)n;
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
