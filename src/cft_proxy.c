/*
 * Each proxied request gets its own origin connection (no pooling): a
 * non-blocking socket in the tunnel's poller. Both directions are bounded:
 * the request body is paused when the origin reads slowly, and reading from
 * the origin pauses when the response queue is full.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cft_conn.h"
#include "cft_poll.h"
#include "cft_port.h"
#include "cft_proxy.h"
#include "cft_squeue.h"

static const char *TAG = "cft_proxy";

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define CONNECT_TIMEOUT_MS 30000 /* cloudflared's connectTimeout */
#define STALL_TIMEOUT_MS 30000   /* origin takes none of the queued request data */
#define READ_CHUNK 1024
#define DEFAULT_BUFFER (16 * 1024) /* per direction, see proxy_buffer_size */
#define MAX_HEAD (16 * 1024)     /* response status line and headers */
#define MAX_RESP_HEADERS 64

typedef enum {
    P_CONNECTING,
    P_HEAD,        /* reading the response head */
    P_BODY_LENGTH, /* Content-Length body */
    P_BODY_CHUNKED,
    P_BODY_EOF,    /* body ends when the origin closes */
    P_UPGRADED,    /* after 101: bytes both ways */
    P_TCP,         /* TCP over WebSocket */
    P_DONE,
} pstate_t;

typedef enum {
    CH_SIZE,
    CH_DATA,
    CH_DATA_END, /* CRLF after the data */
    CH_TRAILER,
} chunk_state_t;

typedef struct {
    uint8_t *p;
    size_t len;
    size_t cap;
    size_t off;
} xbuf_t;

typedef struct {
    cft_stream_t *s;
    cft_poll_entry_t *pe;
    int fd;
    pstate_t state;
    bool tcp;           /* TCP over WebSocket */
    bool upgrade;       /* WebSocket request to an HTTP origin */
    bool head_request;
    char *host_header;
    /* Request: bytes for the origin */
    cft_squeue_t up;
    cft_squeue_t early;     /* body received before the request head was written */
    bool head_sent;
    bool chunked_up;
    bool body_ended;
    /* Response */
    xbuf_t head;
    uint64_t left;
    chunk_state_t chunk;
    char line[64];
    size_t line_len;
    bool reading;
    bool resp_started;
    size_t up_high;   /* pause the request body above this */
    bool stall_armed; /* deadline set for STALL_TIMEOUT_MS */
    bool shut_wr;     /* SHUT_WR sent to origin after request body ended */
    size_t down_high; /* stop reading the origin above this */
} proxy_t;

/* --- buffers --- */

static int xbuf_append(xbuf_t *b, const void *data, size_t len)
{
    if (len == 0) {
        return 0;
    }
    if (b->off && b->off == b->len) {
        b->off = b->len = 0;
    }
    if (b->len + len > b->cap) {
        size_t cap = b->cap ? b->cap : 512;
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

static void xbuf_free(xbuf_t *b)
{
    free(b->p);
    memset(b, 0, sizeof(*b));
}

/* --- origin connection --- */

/* Closes the origin connection; the stream remains valid until on_close */
static void origin_close(proxy_t *p, bool stream_alive)
{
    if (p->pe) {
        cft_poll_remove(p->pe);
        p->pe = NULL;
    }
    if (p->fd >= 0) {
        close(p->fd);
        p->fd = -1;
    }
    cft_sq_free(&p->up);
    cft_sq_free(&p->early);
    xbuf_free(&p->head);
    if (stream_alive) {
        /* Resume flow control so any remaining request body drains and is discarded */
        cft_stream_pause_body(p->s, false);
    }
}

static void update_want(proxy_t *p)
{
    if (!p->pe) {
        return;
    }
    unsigned ev = 0;
    if (p->state == P_CONNECTING || p->up.pending) {
        ev |= CFT_POLL_WRITE;
    }
    if (p->state != P_CONNECTING && p->reading) {
        ev |= CFT_POLL_READ;
    }
    cft_poll_want(p->pe, ev);
}

/* The origin failed or closed the connection before the response completed */
static void origin_error(proxy_t *p, const char *why)
{
    CFT_LOGW(TAG, "%s: %s", cft_stream_request(p->s)->service, why);
    if (!p->resp_started) {
        cft_stream_fail(p->s, 502);
    } else if (p->tcp) {
        cft_ws_close(p->s, 1011, NULL);
    } else {
        cft_stream_reset(p->s);
    }
    p->state = P_DONE;
    origin_close(p, true);
}

/* The response is complete */
static void finish(proxy_t *p)
{
    if (p->tcp) {
        cft_ws_close(p->s, 1000, NULL);
    } else {
        cft_stream_end(p->s);
    }
    p->state = P_DONE;
    origin_close(p, true);
}

/* After the client closes its write side on a byte stream (TCP over
 * WebSocket or upgraded HTTP) and all queued bytes are sent to the origin,
 * call shutdown(SHUT_WR) so the origin receives EOF and closes the
 * connection. */
static void maybe_half_close(proxy_t *p)
{
    if (p->body_ended && !p->shut_wr && p->fd >= 0 && p->up.pending == 0 &&
            (p->state == P_TCP || p->state == P_UPGRADED)) {
        shutdown(p->fd, SHUT_WR);
        p->shut_wr = true;
    }
}

static void flush_up(proxy_t *p)
{
    bool progressed = false;
    while (p->fd >= 0 && p->up.pending) {
        cft_seg_t *sg = p->up.head;
        ssize_t n = send(p->fd, sg->data + sg->off, sg->len - sg->off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                break;
            }
            origin_error(p, strerror(errno));
            return;
        }
        cft_sq_consume(&p->up, (size_t)n);
        progressed |= n > 0;
    }
    if (p->fd < 0) {
        return;
    }
    if (p->up.pending == 0) {
        cft_stream_pause_body(p->s, false);
        cft_poll_deadline(p->pe, 0);
        p->stall_armed = false;
        maybe_half_close(p);
    } else if (progressed || !p->stall_armed) {
        /* Abort the transfer if the origin accepts no data for STALL_TIMEOUT_MS */
        cft_poll_deadline(p->pe, cft_port_now_ms() + STALL_TIMEOUT_MS);
        p->stall_armed = true;
    }
    update_want(p);
}

/* --- request --- */

static bool hop_by_hop(const char *name)
{
    static const char *const names[] = {
        "connection", "keep-alive", "proxy-connection", "transfer-encoding", "te", "trailer", "upgrade",
        "host", "content-length", "expect", "http2-settings", "sec-websocket-version",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcasecmp(name, names[i]) == 0) {
            return true;
        }
    }
    return false;
}

static int add_body(proxy_t *p, const uint8_t *data, size_t len)
{
    if (len == 0) {
        return 0;
    }
    if (!p->chunked_up) {
        return cft_sq_append(&p->up, data, len);
    }
    if (cft_sq_printf(&p->up, "%zx\r\n", len) < 0 || cft_sq_append(&p->up, data, len) < 0) {
        return -1;
    }
    return cft_sq_append(&p->up, "\r\n", 2);
}

static int write_request_head(proxy_t *p)
{
    const cft_request_t *req = cft_stream_request(p->s);
    cft_squeue_t *b = &p->up;
    if (cft_sq_printf(b, "%s ", req->method) < 0 || cft_sq_append(b, req->path, strlen(req->path)) < 0 ||
            cft_sq_append(b, " HTTP/1.1\r\nHost: ", 17) < 0) {
        return -1;
    }
    const char *host = p->host_header ? p->host_header : req->authority;
    if (cft_sq_append(b, host, strlen(host)) < 0 || cft_sq_append(b, "\r\n", 2) < 0) {
        return -1;
    }
    if (p->host_header && cft_sq_printf(b, "X-Forwarded-Host: %s\r\n", req->authority) < 0) {
        return -1;
    }
    for (size_t i = 0; i < req->num_headers; i++) {
        const cft_header_t *h = &req->headers[i];
        if (hop_by_hop(h->name)) {
            continue;
        }
        if (cft_sq_append(b, h->name, strlen(h->name)) < 0 || cft_sq_append(b, ": ", 2) < 0 ||
                cft_sq_append(b, h->value, strlen(h->value)) < 0 || cft_sq_append(b, "\r\n", 2) < 0) {
            return -1;
        }
    }
    int rv;
    if (p->upgrade) {
        rv = cft_sq_printf(b, "Connection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\n\r\n");
    } else if (req->content_length >= 0) {
        rv = cft_sq_printf(b, "Connection: close\r\nContent-Length: %lld\r\n\r\n", (long long)req->content_length);
    } else if (p->body_ended && p->early.pending == 0) {
        rv = cft_sq_printf(b, "Connection: close\r\n\r\n");
    } else {
        p->chunked_up = true;
        rv = cft_sq_printf(b, "Connection: close\r\nTransfer-Encoding: chunked\r\n\r\n");
    }
    if (rv < 0) {
        return -1;
    }
    for (cft_seg_t *sg = p->early.head; sg; sg = sg->next) {
        if (add_body(p, sg->data + sg->off, sg->len - sg->off) < 0) {
            return -1;
        }
    }
    cft_sq_free(&p->early);
    if (p->body_ended && p->chunked_up && cft_sq_append(b, "0\r\n\r\n", 5) < 0) {
        return -1;
    }
    p->head_sent = true;
    return 0;
}

/* --- response --- */

static void send_down(proxy_t *p, const uint8_t *data, size_t len)
{
    if (len == 0) {
        return;
    }
    if (p->tcp) {
        cft_ws_send(p->s, CFT_WS_BINARY, data, len);
    } else {
        cft_stream_write(p->s, data, len);
    }
}

static bool forwarded_response_header(const char *name, bool upgrade)
{
    if (upgrade && (strcasecmp(name, "upgrade") == 0 || strcasecmp(name, "connection") == 0)) {
        return true;
    }
    return !hop_by_hop(name) || strcasecmp(name, "content-length") == 0;
}

/* "chunked" is the last transfer coding when present (RFC 9112 6.1) */
static bool ends_with_chunked(const char *v)
{
    size_t n = strlen(v);
    return n >= 7 && strcasecmp(v + n - 7, "chunked") == 0;
}

/* Content-Length as Go's http client requires it: digits only, no overflow.
 * Anything else means the origin is broken or hostile. */
static bool parse_content_length(const char *v, int64_t *out)
{
    if (!v || !v[0]) {
        return false;
    }
    errno = 0;
    char *end;
    long long n = strtoll(v, &end, 10);
    if (*end != '\0' || errno == ERANGE || n < 0) {
        return false;
    }
    *out = (int64_t)n;
    return true;
}

static char *trim(char *s, char *end)
{
    while (s < end && (*s == ' ' || *s == '\t')) {
        s++;
    }
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) {
        end--;
    }
    *end = '\0';
    return s;
}

/* Parses the head in p->head (NUL-terminated, without the blank line) and
 * starts the response. Returns false if it was an interim (1xx) response. */
static bool start_response(proxy_t *p)
{
    char *text = (char *)p->head.p;
    char *eol = strchr(text, '\n');
    char *line_end = eol ? eol : text + strlen(text);
    int status = 0;
    if (strncmp(text, "HTTP/1.", 7) != 0 || sscanf(text + 8, " %3d", &status) != 1 || status < 100) {
        origin_error(p, "malformed response");
        return true;
    }
    if (status >= 100 && status < 200 && status != 101) {
        return false;
    }
    if (status == 101 && !p->upgrade) {
        origin_error(p, "unexpected 101 response");
        return true;
    }
    cft_header_t hdrs[MAX_RESP_HEADERS];
    size_t n = 0;
    int64_t content_length = -1;
    bool have_cl = false;
    bool chunked = false;
    char *line = eol ? line_end + 1 : line_end;
    while (*line) {
        eol = strchr(line, '\n');
        line_end = eol ? eol : line + strlen(line);
        char *colon = memchr(line, ':', (size_t)(line_end - line));
        if (colon) {
            *colon = '\0';
            char *name = trim(line, colon);
            char *value = trim(colon + 1, line_end);
            bool is_cl = strcasecmp(name, "content-length") == 0;
            if (is_cl) {
                int64_t cl;
                if (!parse_content_length(value, &cl) || (have_cl && cl != content_length)) {
                    origin_error(p, "malformed Content-Length");
                    return true;
                }
                if (!have_cl) {
                    have_cl = true;
                    content_length = cl;
                } else {
                    is_cl = false; /* duplicate: forward it only once */
                }
            } else if (strcasecmp(name, "transfer-encoding") == 0 && ends_with_chunked(value)) {
                chunked = true;
            }
            if (n < MAX_RESP_HEADERS && (is_cl || forwarded_response_header(name, status == 101))) {
                hdrs[n].name = name;
                hdrs[n].value = value;
                n++;
            }
        }
        line = eol ? eol + 1 : line_end;
    }
    if (chunked) {
        /* HTTP/2 frames the response body directly; strip any Content-Length header accompanying chunked encoding */
        size_t k = 0;
        for (size_t i = 0; i < n; i++) {
            if (strcasecmp(hdrs[i].name, "content-length") != 0) {
                hdrs[k++] = hdrs[i];
            }
        }
        n = k;
    }
    if (cft_stream_send_headers(p->s, status, hdrs, n) < 0) {
        origin_error(p, "could not send the response");
        return true;
    }
    p->resp_started = true;
    if (status == 101) {
        p->state = P_UPGRADED;
    } else if (p->head_request || status == 204 || status == 304) {
        finish(p);
    } else if (chunked) {
        p->state = P_BODY_CHUNKED;
        p->chunk = CH_SIZE;
        p->line_len = 0;
    } else if (content_length >= 0) {
        p->left = (uint64_t)content_length;
        p->state = P_BODY_LENGTH;
        if (p->left == 0) {
            finish(p);
        }
    } else {
        p->state = P_BODY_EOF;
    }
    return true;
}

/* Collects a line of the chunked framing; returns true when complete. */
static bool chunk_line(proxy_t *p, uint8_t c)
{
    if (c == '\n') {
        while (p->line_len && p->line[p->line_len - 1] == '\r') {
            p->line_len--;
        }
        p->line[p->line_len] = '\0';
        return true;
    }
    if (p->line_len + 1 < sizeof(p->line)) {
        p->line[p->line_len++] = (char)c;
    }
    return false;
}

static void feed_chunked(proxy_t *p, const uint8_t *data, size_t len)
{
    while (len && p->state == P_BODY_CHUNKED) {
        switch (p->chunk) {
        case CH_DATA: {
            size_t k = len < p->left ? len : (size_t)p->left;
            send_down(p, data, k);
            data += k;
            len -= k;
            p->left -= k;
            if (p->left == 0) {
                p->chunk = CH_DATA_END;
                p->line_len = 0;
            }
            continue;
        }
        case CH_SIZE:
            if (chunk_line(p, *data)) {
                char *end;
                unsigned long long size = strtoull(p->line, &end, 16);
                if (end == p->line) {
                    origin_error(p, "malformed chunked body");
                    return;
                }
                p->line_len = 0;
                p->left = size;
                p->chunk = size ? CH_DATA : CH_TRAILER;
            }
            break;
        case CH_DATA_END:
            if (chunk_line(p, *data)) {
                p->line_len = 0;
                p->chunk = CH_SIZE;
            }
            break;
        case CH_TRAILER:
            if (chunk_line(p, *data)) {
                if (p->line_len == 0) {
                    finish(p);
                    return;
                }
                p->line_len = 0;
            }
            break;
        }
        data++;
        len--;
    }
}

static void feed(proxy_t *p, const uint8_t *data, size_t len)
{
    while (len && p->fd >= 0) {
        switch (p->state) {
        case P_HEAD: {
            /* Up to the end of the head, which may be split across reads */
            size_t i = 0;
            bool complete = false;
            while (i < len && !complete) {
                uint8_t c = data[i++];
                if (xbuf_append(&p->head, &c, 1) < 0) {
                    origin_error(p, "out of memory");
                    return;
                }
                size_t hl = p->head.len;
                complete = (hl >= 2 && memcmp(p->head.p + hl - 2, "\n\n", 2) == 0) ||
                           (hl >= 4 && memcmp(p->head.p + hl - 4, "\r\n\r\n", 4) == 0);
            }
            data += i;
            len -= i;
            if (!complete) {
                if (p->head.len > MAX_HEAD) {
                    origin_error(p, "response head too large");
                }
                return;
            }
            p->head.p[p->head.len - 1] = '\0';
            bool started = start_response(p);
            xbuf_free(&p->head);
            (void)started; /* 1xx: stay in P_HEAD for the final response */
            break;
        }
        case P_BODY_LENGTH: {
            size_t k = len < p->left ? len : (size_t)p->left;
            send_down(p, data, k);
            p->left -= k;
            if (p->left == 0) {
                finish(p);
            }
            return;
        }
        case P_BODY_CHUNKED:
            feed_chunked(p, data, len);
            return;
        case P_BODY_EOF:
        case P_UPGRADED:
        case P_TCP:
            send_down(p, data, len);
            return;
        case P_CONNECTING:
        case P_DONE:
            return;
        }
    }
}

static void origin_eof(proxy_t *p)
{
    switch (p->state) {
    case P_BODY_EOF:
    case P_UPGRADED:
    case P_TCP:
        finish(p);
        break;
    default:
        origin_error(p, p->state == P_HEAD ? "closed without a response" : "closed before the end of the body");
        break;
    }
}

static void read_origin(proxy_t *p)
{
    uint8_t buf[READ_CHUNK];
    while (p->fd >= 0 && cft_stream_buffered(p->s) < p->down_high) {
        ssize_t n = recv(p->fd, buf, sizeof(buf), 0);
        if (n == 0) {
            origin_eof(p);
            return;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                break;
            }
            origin_error(p, strerror(errno));
            return;
        }
        feed(p, buf, (size_t)n);
    }
    /* on_writable resumes reading once the send queue drains below low_water */
    p->reading = p->fd >= 0 && cft_stream_buffered(p->s) < p->down_high;
    update_want(p);
}

static void connected(proxy_t *p)
{
    cft_poll_deadline(p->pe, 0);
    p->reading = true;
    if (p->tcp) {
        if (cft_ws_accept(p->s, NULL) < 0) {
            origin_error(p, "WebSocket handshake failed");
            return;
        }
        cft_stream_set_ws_raw(p->s);
        p->resp_started = true;
        p->state = P_TCP;
    } else {
        if (write_request_head(p) < 0) {
            origin_error(p, "out of memory");
            return;
        }
        p->state = P_HEAD;
    }
    flush_up(p);
}

static void on_origin_event(void *arg, unsigned events)
{
    proxy_t *p = arg;
    if (p->state == P_CONNECTING) {
        if (events & CFT_POLL_TIMEOUT) {
            origin_error(p, "connect timed out");
            return;
        }
        if (!(events & CFT_POLL_WRITE)) {
            return;
        }
        int err = 0;
        socklen_t el = sizeof(err);
        if (getsockopt(p->fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0) {
            err = errno;
        }
        if (err) {
            origin_error(p, strerror(err));
            return;
        }
        connected(p);
        return;
    }
    if (events & CFT_POLL_TIMEOUT) {
        char state[160];
        cft_stream_describe(p->s, state, sizeof(state));
        /* Log socket readiness and peek state before aborting the stalled transfer */
        uint8_t byte;
        ssize_t peek = recv(p->fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        int peek_errno = errno;
        fd_set r, w;
        FD_ZERO(&r);
        FD_SET(p->fd, &r);
        w = r;
        struct timeval tv = {0};
        int ready = select(p->fd + 1, &r, &w, NULL, &tv);
        CFT_LOGW(TAG, "%s; to origin %u, %sreading the origin; poll wants %u, select %d (r%d w%d), peek %d errno %d",
                 state, (unsigned)p->up.pending, p->reading ? "" : "not ", cft_poll_wanted(p->pe), ready,
                 FD_ISSET(p->fd, &r) ? 1 : 0, FD_ISSET(p->fd, &w) ? 1 : 0, (int)peek, peek_errno);
        origin_error(p, "origin stopped taking data");
        return;
    }
    if (events & CFT_POLL_WRITE) {
        flush_up(p);
    }
    if ((events & CFT_POLL_READ) && p->fd >= 0) {
        read_origin(p);
    }
}

/* --- stream callbacks --- */

static void on_body(cft_stream_t *s, void *ctx, const uint8_t *data, size_t len)
{
    proxy_t *p = ctx;
    if (p->fd < 0) {
        return;
    }
    int rv;
    if (p->state == P_CONNECTING) {
        if (p->tcp) {
            return; /* nothing is expected before the WebSocket is accepted */
        }
        rv = cft_sq_append(&p->early, data, len);
    } else if (p->state == P_UPGRADED || p->state == P_TCP) {
        rv = cft_sq_append(&p->up, data, len);
    } else {
        rv = add_body(p, data, len);
    }
    if (rv < 0) {
        origin_error(p, "out of memory");
        return;
    }
    if (p->up.pending + p->early.pending > p->up_high) {
        cft_stream_pause_body(s, true);
    }
    if (p->state != P_CONNECTING) {
        flush_up(p);
    }
}

static void on_body_end(cft_stream_t *s, void *ctx)
{
    proxy_t *p = ctx;
    p->body_ended = true;
    if (p->state == P_TCP || p->state == P_UPGRADED) {
        maybe_half_close(p);
        return;
    }
    if (p->fd >= 0 && p->head_sent && p->chunked_up && p->state != P_UPGRADED) {
        if (cft_sq_append(&p->up, "0\r\n\r\n", 5) < 0) {
            origin_error(p, "out of memory");
            return;
        }
        flush_up(p);
    }
}

static void on_writable(cft_stream_t *s, void *ctx)
{
    proxy_t *p = ctx;
    if (p->fd >= 0 && !p->reading && p->state != P_CONNECTING) {
        p->reading = true;
        update_want(p);
    }
}

static void on_close(cft_stream_t *s, void *ctx, bool completed)
{
    proxy_t *p = ctx;
    origin_close(p, false);
    free(p->host_header);
    free(p);
}

/* --- start --- */

typedef struct {
    const char *scheme;
    uint16_t port;
    bool tcp;
} scheme_t;

static const scheme_t schemes[] = {
    {"http", 80, false}, {"ws", 80, false},
    {"ssh", 22, true},  {"rdp", 3389, true}, {"smb", 445, true}, {"tcp", 7864, true},
};

/* Splits "host[:port]" or "[v6][:port]", ignoring a trailing "/" */
static bool parse_host_port(const char *s, char *host, size_t host_size, uint16_t *port)
{
    const char *end = s + strcspn(s, "/");
    if (end[0] == '/' && end[1] != '\0') {
        return false; /* origins with a path are not supported, as in cloudflared */
    }
    const char *hs = s, *he;
    const char *port_str = NULL;
    if (*s == '[') {
        hs = s + 1;
        he = memchr(hs, ']', (size_t)(end - hs));
        if (!he) {
            return false;
        }
        if (he + 1 < end && he[1] == ':') {
            port_str = he + 2;
        }
    } else {
        he = memchr(s, ':', (size_t)(end - s));
        if (he) {
            port_str = he + 1;
        } else {
            he = end;
        }
    }
    size_t hl = (size_t)(he - hs);
    if (hl == 0 || hl >= host_size) {
        return false;
    }
    memcpy(host, hs, hl);
    host[hl] = '\0';
    if (port_str) {
        char *pe;
        long v = strtol(port_str, &pe, 10);
        if (pe != end || v <= 0 || v > 65535) {
            return false;
        }
        *port = (uint16_t)v;
    }
    return true;
}

static int connect_origin(const char *host, uint16_t port, int *fd_out)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    /* Blocks while resolving names; IP addresses return right away */
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        CFT_LOGW(TAG, "cannot resolve %s", host);
        return -1;
    }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rv = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rv < 0 && errno != EINPROGRESS) {
        CFT_LOGW(TAG, "cannot connect to %s:%s: %s", host, port_str, strerror(errno));
        close(fd);
        return -1;
    }
    *fd_out = fd;
    return 0;
}

int cft_proxy_start(cft_stream_t *s, const char *service, const char *host_header)
{
    const cft_request_t *req = cft_stream_request(s);
    if (cft_stream_set_service(s, service) < 0) {
        return cft_stream_fail(s, 500);
    }
    if (strncmp(service, "http_status:", 12) == 0) {
        char *end;
        long code = strtol(service + 12, &end, 10);
        if (*end != '\0' || code < 100 || code > 999) {
            return -1;
        }
        return cft_stream_respond(s, (int)code, NULL, NULL, 0);
    }
    const scheme_t *sc = NULL;
    const char *rest = strstr(service, "://");
    if (rest) {
        for (size_t i = 0; i < sizeof(schemes) / sizeof(schemes[0]); i++) {
            size_t n = strlen(schemes[i].scheme);
            if ((size_t)(rest - service) == n && strncasecmp(service, schemes[i].scheme, n) == 0) {
                sc = &schemes[i];
                break;
            }
        }
    }
    char host[256];
    uint16_t port = sc ? sc->port : 0;
    if (!sc || !parse_host_port(rest + 3, host, sizeof(host), &port)) {
        CFT_LOGW(TAG, "unsupported service %s", service);
        return -1;
    }
    if (sc->tcp && !req->is_websocket) {
        /* Plain HTTP request to a TCP-over-WebSocket service */
        return cft_stream_respond(s, 400, "text/plain", "Use cloudflared access to connect\n", 34);
    }
    const cft_conn_config_t *cfg = cft_stream_config(s);
    cft_poller_t *poller = cfg->poller;
    if (!poller || cft_poller_full(poller)) {
        CFT_LOGW(TAG, "too many origin connections, answering 503");
        return cft_stream_respond(s, 503, NULL, NULL, 0); /* load shedding, as for too many streams */
    }
    proxy_t *p = calloc(1, sizeof(*p));
    if (!p || (host_header && !(p->host_header = strdup(host_header)))) {
        free(p);
        return cft_stream_fail(s, 500);
    }
    p->s = s;
    p->fd = -1;
    size_t buffer = cfg->proxy_buffer_size ? cfg->proxy_buffer_size : DEFAULT_BUFFER;
    p->up_high = buffer;
    /* A read can add READ_CHUNK more; stay within the buffer */
    p->down_high = buffer > 2 * READ_CHUNK ? buffer - READ_CHUNK : READ_CHUNK;
    p->tcp = sc->tcp;
    p->upgrade = !sc->tcp && req->is_websocket;
    p->head_request = strcmp(req->method, "HEAD") == 0;
    p->state = P_CONNECTING;
    CFT_LOGD(TAG, "%s %s%s -> %s", req->method, req->authority, req->path, service);
    if (connect_origin(host, port, &p->fd) < 0 || !(p->pe = cft_poll_add(poller, p->fd, on_origin_event, p))) {
        if (p->fd >= 0) {
            close(p->fd);
        }
        free(p->host_header);
        free(p);
        return cft_stream_fail(s, 502);
    }
    cft_poll_deadline(p->pe, cft_port_now_ms() + CONNECT_TIMEOUT_MS);
    update_want(p);
    cft_stream_handlers_t h = {
        .on_body = on_body,
        .on_body_end = on_body_end,
        .on_writable = on_writable,
        .on_close = on_close,
    };
    cft_stream_set_handlers(s, &h, p);
    /* Resume reading from the origin once half the queue has drained */
    cft_stream_set_low_water(s, p->down_high / 2);
    return 0;
}

int cft_stream_proxy(cft_stream_t *s, const char *service)
{
    return cft_proxy_start(s, service, NULL);
}
