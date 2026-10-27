#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "test_handlers.h"

#ifdef CFT_DEBUG_STATS
int cft_debug_stats(char *buf, size_t size); /* src/cft_debug.c */
#endif

#define TICK_MS 50
#define BIG_CHUNK 4096
#define SLOW_MAX_QUEUED (32 * 1024)

static th_config_t s_cfg;
static cft_tunnel_t *s_tunnel;

/* Counters; the tunnel thread writes them, other threads may read */
static atomic_uint s_streams_total;
static atomic_uint s_streams_open;
static atomic_uint s_ws_messages;
static atomic_uint s_posts_run;
static atomic_uint s_posts_out_of_order;
static atomic_uint s_events_registered;
static atomic_uint s_events_closed;
static atomic_uint s_events_config;

/* --- per-stream state --- */

typedef enum {
    K_PLAIN,
    K_BIG,
    K_SLOW,
    K_LATER,
    K_PAUSE,
    K_SINK,
    K_RESET,
    K_WS,
    K_WS_PUSH,
} kind_t;

typedef struct req {
    struct req *next; /* s_ticking */
    cft_stream_t *s;
    kind_t kind;
    bool ticking;
    size_t left;
    size_t pos;
    size_t rate;
    uint64_t hash;
    int64_t due_ms;
    int interval_ms;
    int count;
    bool paused;
} req_t;

static req_t *s_ticking; /* streams served by the ticker, tunnel thread only */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void tick_add(req_t *r)
{
    r->ticking = true;
    r->next = s_ticking;
    s_ticking = r;
}

static void tick_remove(req_t *r)
{
    if (!r->ticking) {
        return;
    }
    for (req_t **pp = &s_ticking; *pp; pp = &(*pp)->next) {
        if (*pp == r) {
            *pp = r->next;
            break;
        }
    }
    r->ticking = false;
}

static void on_close(cft_stream_t *s, void *ctx, bool completed)
{
    req_t *r = ctx;
    tick_remove(r);
    free(r);
    atomic_fetch_sub(&s_streams_open, 1);
}

/* Takes over the stream: counts it and frees r in on_close */
static req_t *track(cft_stream_t *s, kind_t kind, const cft_stream_handlers_t *h)
{
    req_t *r = calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    r->s = s;
    r->kind = kind;
    cft_stream_handlers_t hh = h ? *h : (cft_stream_handlers_t) {
        0
    };
    hh.on_close = on_close;
    cft_stream_set_handlers(s, &hh, r);
    atomic_fetch_add(&s_streams_open, 1);
    return r;
}

static bool path_is(const cft_request_t *req, const char *path)
{
    size_t n = strcspn(req->path, "?");
    return n == strlen(path) && strncmp(req->path, path, n) == 0;
}

static bool path_starts(const cft_request_t *req, const char *prefix)
{
    return strncmp(req->path, prefix, strlen(prefix)) == 0;
}

static long query_int(const cft_request_t *req, const char *name, long dflt)
{
    const char *q = strchr(req->path, '?');
    size_t nl = strlen(name);
    while (q) {
        q++;
        if (strncmp(q, name, nl) == 0 && q[nl] == '=') {
            return strtol(q + nl + 1, NULL, 10);
        }
        q = strchr(q, '&');
    }
    return dflt;
}

static void fill_pattern(uint8_t *buf, size_t n, size_t pos)
{
    for (size_t i = 0; i < n; i++) {
        buf[i] = (uint8_t)('a' + (pos + i) % 26);
    }
}

/* --- endpoints --- */

static void hello(cft_stream_t *s, const cft_request_t *req)
{
    char body[4096];
    int n = snprintf(body, sizeof(body),
                     "Hello from the esp-cf-tunnel test handlers\n\n%s %s\nhost: %s\nservice: %s\n\nheaders:\n",
                     req->method, req->path, req->authority, req->service ? req->service : "-");
    for (size_t i = 0; i < req->num_headers && n < (int)sizeof(body) - 1; i++) {
        n += snprintf(body + n, sizeof(body) - (size_t)n, "  %s: %s\n", req->headers[i].name,
                      req->headers[i].value);
    }
    if (n >= (int)sizeof(body)) {
        n = sizeof(body) - 1;
    }
    cft_header_t hdrs[] = {
        {"Content-Type", "text/plain; charset=utf-8"},
        {"X-Served-By", "esp-cf-tunnel"},
    };
    cft_stream_send_headers(s, 200, hdrs, 2);
    cft_stream_write(s, body, (size_t)n);
    cft_stream_end(s);
}

/* /echo */
static void echo_body(cft_stream_t *s, void *ctx, const uint8_t *data, size_t len)
{
    cft_stream_write(s, data, len);
}

static void echo_end(cft_stream_t *s, void *ctx)
{
    cft_stream_end(s);
}

/* /upload */
static void upload_done(cft_stream_t *s, void *ctx, const uint8_t *body, size_t len)
{
    char msg[64];
    int n = snprintf(msg, sizeof(msg), "received %zu bytes\n", len);
    cft_stream_respond(s, 200, "text/plain", msg, (size_t)n);
}

/* /sink */
static void sink_body(cft_stream_t *s, void *ctx, const uint8_t *data, size_t len)
{
    req_t *r = ctx;
    for (size_t i = 0; i < len; i++) {
        r->hash = (r->hash ^ data[i]) * 0x100000001b3ULL;
    }
    r->pos += len;
}

static void sink_end(cft_stream_t *s, void *ctx)
{
    req_t *r = ctx;
    char msg[64];
    int n = snprintf(msg, sizeof(msg), "%zu %016llx\n", r->pos, (unsigned long long)r->hash);
    cft_stream_respond(s, 200, "text/plain", msg, (size_t)n);
}

/* /big and /reset */
static void big_fill(cft_stream_t *s, void *ctx)
{
    req_t *r = ctx;
    if (r->left == 0) {
        if (r->kind == K_RESET) {
            cft_stream_reset(s);
        } else {
            cft_stream_end(s); /* size 0 */
        }
        return;
    }
    uint8_t chunk[BIG_CHUNK];
    size_t n = r->left < sizeof(chunk) ? r->left : sizeof(chunk);
    fill_pattern(chunk, n, r->pos);
    r->pos += n;
    r->left -= n;
    cft_stream_write(s, chunk, n);
    if (r->left == 0 && r->kind == K_BIG) {
        cft_stream_end(s);
    }
}

static void big(cft_stream_t *s, const cft_request_t *req, kind_t kind)
{
    cft_stream_handlers_t h = {.on_writable = big_fill};
    req_t *r = track(s, kind, &h);
    if (!r) {
        cft_stream_respond(s, 500, NULL, NULL, 0);
        return;
    }
    r->left = (size_t)query_int(req, kind == K_RESET ? "after" : "size", 100000);
    char clen[24];
    snprintf(clen, sizeof(clen), "%zu", r->left);
    cft_header_t hdrs[] = {{"Content-Type", "application/octet-stream"}, {"Content-Length", clen}};
    /* /reset announces more than it sends */
    if (kind == K_RESET) {
        snprintf(clen, sizeof(clen), "%zu", r->left + 1000);
    }
    cft_stream_send_headers(s, 200, hdrs, 2);
    big_fill(s, r);
}

/* /slow, /later, /pause, /ws/push: served by tick() */
static void tick_one(req_t *r, int64_t now)
{
    switch (r->kind) {
    case K_SLOW: {
        if (cft_stream_buffered(r->s) > SLOW_MAX_QUEUED) {
            return;
        }
        uint8_t chunk[BIG_CHUNK];
        size_t n = r->left < r->rate ? r->left : r->rate;
        n = n < sizeof(chunk) ? n : sizeof(chunk);
        fill_pattern(chunk, n, r->pos);
        r->pos += n;
        r->left -= n;
        cft_stream_write(r->s, chunk, n);
        if (r->left == 0) {
            tick_remove(r);
            cft_stream_end(r->s);
        }
        break;
    }
    case K_LATER:
        if (now >= r->due_ms) {
            tick_remove(r);
            cft_stream_respond(r->s, 200, "text/plain", "later\n", 6);
        }
        break;
    case K_PAUSE:
        if (r->paused && now >= r->due_ms) {
            tick_remove(r);
            r->paused = false;
            cft_stream_pause_body(r->s, false);
        }
        break;
    case K_WS_PUSH:
        if (now >= r->due_ms) {
            if (r->pos == (size_t)r->count) {
                tick_remove(r);
                cft_ws_close(r->s, 1000, "done");
                break;
            }
            char msg[32];
            int n = snprintf(msg, sizeof(msg), "push %zu", r->pos++);
            cft_ws_send(r->s, CFT_WS_TEXT, msg, (size_t)n);
            r->due_ms = now + r->interval_ms;
        }
        break;
    default:
        break;
    }
}

static void tick(void *arg)
{
    (void)arg;
    int64_t now = now_ms();
    /* tick_one may remove r from the list */
    req_t *next;
    for (req_t *r = s_ticking; r; r = next) {
        next = r->next;
        tick_one(r, now);
    }
}

static void pause_body(cft_stream_t *s, void *ctx, const uint8_t *data, size_t len)
{
    req_t *r = ctx;
    r->pos += len;
    if (r->count == 0) {
        r->count = 1;
        r->paused = true;
        r->due_ms = now_ms() + 1000;
        cft_stream_pause_body(s, true);
        tick_add(r);
    }
}

static void pause_end(cft_stream_t *s, void *ctx)
{
    req_t *r = ctx;
    char msg[64];
    int n = snprintf(msg, sizeof(msg), "received %zu bytes\n", r->pos);
    cft_stream_respond(s, 200, "text/plain", msg, (size_t)n);
}

/* /ws */
static void ws_message(cft_stream_t *s, void *ctx, cft_ws_opcode_t op, const uint8_t *data, size_t len)
{
    atomic_fetch_add(&s_ws_messages, 1);
    if (op == CFT_WS_TEXT && len == 5 && memcmp(data, "close", 5) == 0) {
        cft_ws_close(s, 1000, "bye");
        return;
    }
    cft_ws_send(s, op, data, len);
}

/* /stats */
static void stats(cft_stream_t *s)
{
    char body[1024];
    int n = snprintf(body, sizeof(body),
                     "{\"streams_total\":%u,\"streams_open\":%u,\"ws_messages\":%u,\"posts_run\":%u,"
                     "\"posts_out_of_order\":%u,\"registered\":%u,\"closed\":%u,\"config\":%u",
                     atomic_load(&s_streams_total), atomic_load(&s_streams_open), atomic_load(&s_ws_messages),
                     atomic_load(&s_posts_run), atomic_load(&s_posts_out_of_order),
                     atomic_load(&s_events_registered), atomic_load(&s_events_closed),
                     atomic_load(&s_events_config));
#ifdef CFT_DEBUG_STATS
    if (n < (int)sizeof(body)) {
        n += cft_debug_stats(body + n, sizeof(body) - (size_t)n);
    }
#endif
    if (s_cfg.platform_stats && n < (int)sizeof(body)) {
        n += s_cfg.platform_stats(body + n, sizeof(body) - (size_t)n);
    }
    if (n < (int)sizeof(body) - 1) {
        body[n++] = '}';
    }
    cft_stream_respond(s, 200, "application/json", body, (size_t)n);
}

static void proxy(cft_stream_t *s, const char *service)
{
    if (!service) {
        cft_stream_respond(s, 404, "text/plain", "not configured\n", 15);
    } else if (cft_stream_proxy(s, service) < 0) {
        cft_stream_respond(s, 502, NULL, NULL, 0);
    }
}

void th_on_request(void *arg, cft_stream_t *s, const cft_request_t *req)
{
    atomic_fetch_add(&s_streams_total, 1);
    if (s_cfg.handler_delay_ms) {
        usleep((useconds_t)s_cfg.handler_delay_ms * 1000);
    }
    /* Proxied streams belong to the proxy; not tracked */
    if (path_starts(req, "/lan/")) {
        proxy(s, s_cfg.lan_http);
        return;
    }
    if (path_starts(req, "/landead/")) {
        proxy(s, s_cfg.lan_dead);
        return;
    }
    if (req->is_websocket && path_is(req, "/tcp")) {
        proxy(s, s_cfg.lan_tcp);
        return;
    }
    if (path_starts(req, "/chaos/")) {
        track(s, K_PLAIN, NULL);
        if (s_cfg.chaos) {
            s_cfg.chaos(s, req);
        } else {
            cft_stream_respond(s, 404, NULL, NULL, 0);
        }
        return;
    }

    if (req->is_websocket && path_is(req, "/ws")) {
        cft_stream_handlers_t h = {.on_ws_message = ws_message};
        track(s, K_WS, &h);
        cft_ws_accept(s, NULL);
    } else if (req->is_websocket && path_is(req, "/ws/push")) {
        cft_stream_handlers_t h = {.on_ws_message = ws_message};
        req_t *r = track(s, K_WS_PUSH, &h);
        if (r && cft_ws_accept(s, NULL) == 0) {
            r->count = (int)query_int(req, "n", 10);
            r->interval_ms = (int)query_int(req, "ms", 100);
            r->due_ms = now_ms();
            tick_add(r);
        }
    } else if (path_is(req, "/echo")) {
        cft_stream_handlers_t h = {.on_body = echo_body, .on_body_end = echo_end};
        track(s, K_PLAIN, &h);
        cft_header_t hdrs[] = {{"Content-Type", "application/octet-stream"}};
        cft_stream_send_headers(s, 200, hdrs, 1);
    } else if (path_is(req, "/upload")) {
        track(s, K_PLAIN, NULL);
        cft_stream_read_body(s, 0, upload_done, NULL);
    } else if (path_is(req, "/sink")) {
        cft_stream_handlers_t h = {.on_body = sink_body, .on_body_end = sink_end};
        req_t *r = track(s, K_SINK, &h);
        if (r) {
            r->hash = 0xcbf29ce484222325ULL;
        }
    } else if (path_is(req, "/big")) {
        big(s, req, K_BIG);
    } else if (path_is(req, "/reset")) {
        big(s, req, K_RESET);
    } else if (path_is(req, "/slow")) {
        req_t *r = track(s, K_SLOW, NULL);
        if (r) {
            r->left = (size_t)query_int(req, "size", 100000);
            r->rate = (size_t)query_int(req, "rate", 1000);
            r->rate = r->rate ? r->rate : 1;
            char clen[24];
            snprintf(clen, sizeof(clen), "%zu", r->left);
            cft_header_t hdrs[] = {{"Content-Type", "application/octet-stream"}, {"Content-Length", clen}};
            cft_stream_send_headers(s, 200, hdrs, 2);
            tick_add(r);
        }
    } else if (path_is(req, "/later")) {
        req_t *r = track(s, K_LATER, NULL);
        if (r) {
            r->due_ms = now_ms() + query_int(req, "ms", 200);
            tick_add(r);
        }
    } else if (path_is(req, "/pause")) {
        cft_stream_handlers_t h = {.on_body = pause_body, .on_body_end = pause_end};
        track(s, K_PAUSE, &h);
    } else if (path_is(req, "/stats")) {
        track(s, K_PLAIN, NULL);
        stats(s);
    } else {
        track(s, K_PLAIN, NULL);
        hello(s, req);
    }
}

void th_on_event(void *arg, const cft_event_t *ev)
{
    switch (ev->type) {
    case CFT_EVENT_REGISTERED:
        atomic_fetch_add(&s_events_registered, 1);
        printf("[%u] registered: connection %s at %s\n", ev->conn_index, ev->registered.connection_id,
               ev->registered.location);
        break;
    case CFT_EVENT_REGISTRATION_FAILED:
        printf("[%u] registration failed: %s\n", ev->conn_index, ev->failed.cause);
        break;
    case CFT_EVENT_CONFIG_UPDATED:
        atomic_fetch_add(&s_events_config, 1);
        printf("[%u] config version %d\n", ev->conn_index, (int)ev->config.version);
        break;
    case CFT_EVENT_DRAINING:
        printf("[%u] draining\n", ev->conn_index);
        break;
    case CFT_EVENT_UNREGISTERED:
        printf("[%u] unregistered\n", ev->conn_index);
        break;
    case CFT_EVENT_CLOSED:
        atomic_fetch_add(&s_events_closed, 1);
        printf("[%u] closed: %s\n", ev->conn_index, ev->closed.reason);
        break;
    }
}

/* --- ticker and posters --- */

static atomic_bool s_run;
static pthread_t s_ticker;
#define MAX_POSTERS 8
static pthread_t s_posters[MAX_POSTERS];
static int s_num_posters;

typedef struct {
    int thread;
    unsigned seq;
} post_msg_t;

static unsigned s_last_seq[MAX_POSTERS]; /* tunnel thread only */

static void post_run(void *arg)
{
    post_msg_t *m = arg;
    if (m->seq != s_last_seq[m->thread] + 1) {
        atomic_fetch_add(&s_posts_out_of_order, 1);
    }
    s_last_seq[m->thread] = m->seq;
    atomic_fetch_add(&s_posts_run, 1);
    free(m);
}

static void *poster(void *arg)
{
    int idx = (int)(intptr_t)arg;
    unsigned seq = 0;
    while (atomic_load(&s_run)) {
        post_msg_t *m = malloc(sizeof(*m));
        if (m) {
            m->thread = idx;
            m->seq = ++seq;
            if (cft_tunnel_post(s_tunnel, post_run, m) < 0) {
                free(m);
                seq--;
            }
        }
        usleep(1000 + (rand() % 20000));
    }
    return NULL;
}

static void *ticker(void *arg)
{
    while (atomic_load(&s_run)) {
        usleep(TICK_MS * 1000);
        cft_tunnel_post(s_tunnel, tick, NULL);
    }
    return NULL;
}

void th_init(const th_config_t *cfg)
{
    s_cfg = *cfg;
}

int th_start(cft_tunnel_t *tunnel)
{
    s_tunnel = tunnel;
    atomic_store(&s_run, true);
    if (pthread_create(&s_ticker, NULL, ticker, NULL) != 0) {
        return -1;
    }
    s_num_posters = s_cfg.poster_threads < MAX_POSTERS ? s_cfg.poster_threads : MAX_POSTERS;
    for (int i = 0; i < s_num_posters; i++) {
        if (pthread_create(&s_posters[i], NULL, poster, (void *)(intptr_t)i) != 0) {
            s_num_posters = i;
            break;
        }
    }
    return 0;
}

void th_stop(void)
{
    atomic_store(&s_run, false);
    pthread_join(s_ticker, NULL);
    for (int i = 0; i < s_num_posters; i++) {
        pthread_join(s_posters[i], NULL);
    }
}
