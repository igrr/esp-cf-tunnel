/*
 * Supervisor: keeps cfg.num_connections connections to the edge, each in a
 * slot with its own connection index. Failed slots reconnect with exponential
 * backoff and alternate regions; when the edge drains a connection, the
 * supervisor opens a replacement in a free slot before closing the draining
 * connection.
 *
 * With use_ingress_rules, requests are routed by the ingress rules the edge
 * pushes, to on_request or to the proxy; the proxy's origin sockets share
 * the select loop through the poller.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

#include "cf_tunnel.h"
#include "cft_conn.h"
#include "cft_ingress.h"
#include "cft_poll.h"
#include "cft_port.h"
#include "cft_proxy.h"

static const char *TAG = "cft_tunnel";

#define DEFAULT_VERSION "esp-cf-tunnel/0.1"
#define MAX_SLOTS 4 /* connection indices 0..3, as in cloudflared */
#define CONNECT_TIMEOUT_MS 15000
#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 60000
#define POLL_MS 100 /* also the latency of cft_tunnel_post() */
#define RX_BUF_SIZE 4096
#define DEFAULT_MAX_ORIGINS 4

typedef enum {
    SLOT_FREE,
    SLOT_WAIT,       /* backing off until `deadline_ms` */
    SLOT_CONNECTING, /* TCP + TLS in progress until `deadline_ms` */
    SLOT_RUNNING,
} slot_state_t;

typedef struct {
    slot_state_t state;
    uint8_t region;
    void *tr;
    cft_conn_t *conn;
    cft_conn_config_t conn_cfg;
    uint8_t local_ip[16];
    int64_t deadline_ms;
    uint8_t attempts;     /* failed attempts since the last registration */
    uint32_t backoff_ms;
    int64_t retry_after_ms;
    bool registered;
    bool draining;
    /* Bytes taken from the connection but not yet accepted by the transport */
    uint8_t *out;
    size_t out_len;
    size_t out_cap;
} slot_t;

typedef struct post {
    struct post *next;
    void (*fn)(void *arg);
    void *arg;
} post_t;

struct cft_tunnel {
    cft_tunnel_config_t cfg;
    cft_transport_ops_t tr;
    cft_token_t token;
    uint8_t connector_id[16];
    slot_t slots[MAX_SLOTS];
    uint8_t *rx;
    atomic_bool stop;
    atomic_uint stop_timeout_ms;
    bool stopping;
    int64_t stop_deadline_ms;
    pthread_mutex_t post_lock;
    post_t *posts;      /* oldest first */
    post_t **posts_tail;
    cft_poller_t *poller;
    cft_ingress_t *ingress;
    int32_t ingress_version;
};

static void apply_config(cft_tunnel_t *t, const cft_event_t *ev)
{
    if (!t->cfg.use_ingress_rules || (t->ingress && ev->config.version <= t->ingress_version)) {
        return;
    }
    cft_ingress_t *ing = cft_ingress_parse(ev->config.json, ev->config.json_len);
    if (!ing) {
        CFT_LOGW(TAG, "configuration version %ld has no usable ingress rules", (long)ev->config.version);
        return;
    }
    cft_ingress_free(t->ingress);
    t->ingress = ing;
    t->ingress_version = ev->config.version;
    CFT_LOGI(TAG, "applied ingress rules of configuration version %ld", (long)ev->config.version);
}

static void route_request(void *arg, cft_stream_t *s, const cft_request_t *req)
{
    cft_tunnel_t *t = arg;
    const cft_ingress_rule_t *rule = t->ingress ? cft_ingress_match(t->ingress, req->authority, req->path) : NULL;
    if (rule && !(t->cfg.on_request && cft_ingress_is_local(rule->service))) {
        if (cft_proxy_start(s, rule->service, rule->host_header) < 0) {
            cft_stream_fail(s, 502);
        }
        return;
    }
    if (rule && cft_stream_set_service(s, rule->service) < 0) {
        cft_stream_fail(s, 500);
        return;
    }
    if (t->cfg.on_request) {
        t->cfg.on_request(t->cfg.request_arg, s, req);
    } else {
        cft_stream_respond(s, 404, NULL, NULL, 0);
    }
}

/* Called by the connection; slot index = conn_index */
static void on_conn_event(void *arg, const cft_event_t *ev)
{
    cft_tunnel_t *t = arg;
    slot_t *s = &t->slots[ev->conn_index];
    switch (ev->type) {
    case CFT_EVENT_REGISTERED:
        s->registered = true;
        s->attempts = 0;
        s->backoff_ms = BACKOFF_MIN_MS;
        CFT_LOGI(TAG, "connection %u registered: %s at %s", ev->conn_index, ev->registered.connection_id,
                 ev->registered.location);
        break;
    case CFT_EVENT_REGISTRATION_FAILED:
        CFT_LOGW(TAG, "connection %u refused: %s", ev->conn_index, ev->failed.cause);
        s->retry_after_ms = ev->failed.retry_after_ms;
        break;
    case CFT_EVENT_DRAINING:
        CFT_LOGI(TAG, "connection %u is being drained by the edge", ev->conn_index);
        s->draining = true;
        break;
    case CFT_EVENT_CLOSED:
        CFT_LOGI(TAG, "connection %u closed: %s", ev->conn_index, ev->closed.reason);
        break;
    case CFT_EVENT_CONFIG_UPDATED:
        apply_config(t, ev);
        break;
    default:
        break;
    }
    if (t->cfg.on_event) {
        t->cfg.on_event(t->cfg.event_arg, ev);
    }
}

static unsigned num_slots_in(cft_tunnel_t *t, bool count_draining)
{
    unsigned n = 0;
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (t->slots[i].state != SLOT_FREE && (count_draining || !t->slots[i].draining)) {
            n++;
        }
    }
    return n;
}

/* Select the region with fewer active slots */
static uint8_t pick_region(cft_tunnel_t *t, int except)
{
    int use[2] = {0, 0};
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (i != except && t->slots[i].state != SLOT_FREE) {
            use[t->slots[i].region]++;
        }
    }
    if (use[0] != use[1]) {
        return use[0] < use[1] ? 0 : 1;
    }
    uint8_t r;
    cft_port_random(&r, 1);
    return r & 1;
}

static void slot_start_connect(cft_tunnel_t *t, int i, int64_t now)
{
    slot_t *s = &t->slots[i];
    const char *host = t->cfg.edge_hosts[s->region];
    CFT_LOGI(TAG, "connection %d: connecting to %s", i, host);
    s->tr = t->tr.open(t->tr.ctx, host, t->cfg.edge_port, CFT_EDGE_SNI, cft_edge_ca_pem_str);
    s->state = SLOT_CONNECTING;
    s->deadline_ms = now + CONNECT_TIMEOUT_MS;
    s->registered = false;
    s->draining = false;
    s->retry_after_ms = 0;
    s->out_len = 0;
}

static void slot_wait(cft_tunnel_t *t, int i, int64_t now)
{
    slot_t *s = &t->slots[i];
    uint32_t jitter;
    cft_port_random(&jitter, sizeof(jitter));
    uint32_t delay = s->backoff_ms + jitter % (s->backoff_ms / 2 + 1);
    if (s->retry_after_ms > delay) {
        delay = (uint32_t)s->retry_after_ms;
    }
    s->backoff_ms = s->backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : s->backoff_ms * 2;
    s->state = SLOT_WAIT;
    s->deadline_ms = now + delay;
    CFT_LOGI(TAG, "connection %d: retrying in %u ms", i, (unsigned)delay);
}

/* Frees the slot's connection and transport, then schedules a retry or releases the slot. */
static void slot_close(cft_tunnel_t *t, int i, int64_t now)
{
    slot_t *s = &t->slots[i];
    bool was_registered = s->registered;
    if (s->conn) {
        cft_conn_free(s->conn);
        s->conn = NULL;
    }
    if (s->tr) {
        t->tr.close(s->tr);
        s->tr = NULL;
    }
    s->out_len = 0;
    if (t->stopping || s->draining) {
        /* A replacement was started when draining began */
        s->state = SLOT_FREE;
        s->draining = false;
        return;
    }
    if (was_registered) {
        /* Previously registered connection dropped: reconnect after BACKOFF_MIN_MS in the same region */
        s->backoff_ms = BACKOFF_MIN_MS;
    } else {
        if (s->attempts < UINT8_MAX) {
            s->attempts++;
        }
        s->region ^= 1;
    }
    slot_wait(t, i, now);
}

static bool slot_connected(cft_tunnel_t *t, int i)
{
    slot_t *s = &t->slots[i];
    s->conn_cfg = (cft_conn_config_t) {
        .token = &t->token,
        .conn_index = (uint8_t)i,
        .client_id = t->connector_id,
        .version = t->cfg.version ? t->cfg.version : DEFAULT_VERSION,
        .arch = t->cfg.arch ? t->cfg.arch : "unknown",
        .num_previous_attempts = s->attempts,
        .on_request = route_request,
        .request_arg = t,
        .on_event = on_conn_event,
        .event_arg = t,
        .max_request_body = t->cfg.max_request_body,
        .max_concurrent_streams = t->cfg.max_concurrent_streams,
        .poller = t->poller,
        .stream_window = t->cfg.stream_window,
        .proxy_buffer_size = t->cfg.proxy_buffer_size,
    };
    s->conn_cfg.local_ip_len = t->tr.local_ip(s->tr, s->local_ip);
    s->conn_cfg.local_ip = s->conn_cfg.local_ip_len ? s->local_ip : NULL;
    s->conn = cft_conn_new(&s->conn_cfg);
    if (!s->conn) {
        CFT_LOGE(TAG, "out of memory");
        return false;
    }
    s->state = SLOT_RUNNING;
    return true;
}

/* Moves data from the connection to the transport. Returns false on error. */
static bool slot_flush(cft_tunnel_t *t, slot_t *s)
{
    for (;;) {
        if (s->out_len) {
            long w = t->tr.write(s->tr, s->out, s->out_len);
            if (w < 0) {
                return false;
            }
            memmove(s->out, s->out + w, s->out_len - (size_t)w);
            s->out_len -= (size_t)w;
            if (s->out_len) {
                return true; /* transport is full; wait for writability */
            }
        }
        const uint8_t *data;
        long n = cft_conn_send_data(s->conn, &data);
        if (n <= 0) {
            return n == 0;
        }
        if (s->out_cap < (size_t)n) {
            uint8_t *nb = realloc(s->out, (size_t)n);
            if (!nb) {
                return false;
            }
            s->out = nb;
            s->out_cap = (size_t)n;
        }
        memcpy(s->out, data, (size_t)n);
        s->out_len = (size_t)n;
    }
}

/* Reads whatever is available and feeds it to the connection. */
static bool slot_read(cft_tunnel_t *t, slot_t *s)
{
    for (int rounds = 0; rounds < 16; rounds++) {
        long n = t->tr.read(s->tr, t->rx, RX_BUF_SIZE);
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            return true;
        }
        cft_conn_recv(s->conn, t->rx, (size_t)n);
        if (!slot_flush(t, s)) {
            return false;
        }
    }
    return true;
}

/* Keeps num_connections non-draining slots in use. */
static void maintain_slots(cft_tunnel_t *t, int64_t now)
{
    unsigned want = t->cfg.num_connections;
    while (!t->stopping && num_slots_in(t, false) < want) {
        int free_idx = -1;
        for (int i = 0; i < MAX_SLOTS; i++) {
            if (t->slots[i].state == SLOT_FREE) {
                free_idx = i;
                break;
            }
        }
        if (free_idx < 0) {
            break; /* all indices busy; a draining slot will free one */
        }
        slot_t *s = &t->slots[free_idx];
        s->attempts = 0;
        s->backoff_ms = BACKOFF_MIN_MS;
        s->region = pick_region(t, free_idx);
        slot_start_connect(t, free_idx, now);
    }
}

cft_tunnel_t *cft_tunnel_new(const cft_tunnel_config_t *cfg, const cft_transport_ops_t *transport)
{
    if (!cfg || !cfg->token || !transport) {
        return NULL;
    }
    cft_tunnel_t *t = calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }
    if (!cft_token_parse(cfg->token, &t->token)) {
        CFT_LOGE(TAG, "invalid tunnel token");
        free(t);
        return NULL;
    }
    t->cfg = *cfg;
    t->cfg.token = NULL;
    if (t->cfg.num_connections == 0) {
        t->cfg.num_connections = 1;
    }
    if (t->cfg.num_connections > MAX_SLOTS) {
        t->cfg.num_connections = MAX_SLOTS;
    }
    t->tr = *transport;
    if (!cfg->edge_hosts[0]) {
        t->cfg.edge_hosts[0] = cfg->edge_hosts[1] ? cfg->edge_hosts[1] : CFT_EDGE_REGION1;
        t->cfg.edge_hosts[1] = cfg->edge_hosts[1] ? cfg->edge_hosts[1] : CFT_EDGE_REGION2;
    } else if (!cfg->edge_hosts[1]) {
        t->cfg.edge_hosts[1] = cfg->edge_hosts[0];
    }
    if (!t->cfg.edge_port) {
        t->cfg.edge_port = CFT_EDGE_PORT;
    }
    if (cfg->connector_id) {
        memcpy(t->connector_id, cfg->connector_id, 16);
    } else {
        cft_port_random(t->connector_id, 16);
        t->connector_id[6] = (t->connector_id[6] & 0x0f) | 0x40; /* UUID v4 */
        t->connector_id[8] = (t->connector_id[8] & 0x3f) | 0x80;
    }
    t->rx = malloc(RX_BUF_SIZE);
    t->poller = cft_poller_new(cfg->max_origin_connections ? cfg->max_origin_connections : DEFAULT_MAX_ORIGINS);
    if (!t->rx || !t->poller) {
        free(t->rx);
        cft_poller_free(t->poller);
        free(t);
        return NULL;
    }
    pthread_mutex_init(&t->post_lock, NULL);
    t->posts_tail = &t->posts;
    atomic_init(&t->stop, false);
    atomic_init(&t->stop_timeout_ms, 5000);
    return t;
}

void cft_tunnel_request_stop(cft_tunnel_t *t, uint32_t stop_timeout_ms)
{
    atomic_store(&t->stop_timeout_ms, stop_timeout_ms);
    atomic_store(&t->stop, true);
}

int cft_tunnel_post(cft_tunnel_t *t, void (*fn)(void *arg), void *arg)
{
    post_t *p = malloc(sizeof(*p));
    if (!p) {
        return -1;
    }
    p->next = NULL;
    p->fn = fn;
    p->arg = arg;
    pthread_mutex_lock(&t->post_lock);
    *t->posts_tail = p;
    t->posts_tail = &p->next;
    pthread_mutex_unlock(&t->post_lock);
    return 0;
}

static void run_posts(cft_tunnel_t *t)
{
    pthread_mutex_lock(&t->post_lock);
    post_t *p = t->posts;
    t->posts = NULL;
    t->posts_tail = &t->posts;
    pthread_mutex_unlock(&t->post_lock);
    while (p) {
        post_t *next = p->next;
        p->fn(p->arg);
        free(p);
        p = next;
    }
}

void cft_tunnel_run(cft_tunnel_t *t)
{
    for (;;) {
        run_posts(t);
        int64_t now = cft_port_now_ms();
        if (atomic_load(&t->stop) && !t->stopping) {
            t->stopping = true;
            t->stop_deadline_ms = now + atomic_load(&t->stop_timeout_ms);
            for (int i = 0; i < MAX_SLOTS; i++) {
                slot_t *s = &t->slots[i];
                if (s->state == SLOT_RUNNING) {
                    cft_conn_shutdown(s->conn);
                } else if (s->state != SLOT_FREE) {
                    slot_close(t, i, now);
                }
            }
        }
        if (t->stopping && (num_slots_in(t, true) == 0 || now > t->stop_deadline_ms)) {
            break;
        }
        maintain_slots(t, now);

        /* Advance connects and timers; collect sockets to wait on */
        fd_set rfds, wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        int maxfd = -1;
        int64_t wake = now + POLL_MS;
        for (int i = 0; i < MAX_SLOTS; i++) {
            slot_t *s = &t->slots[i];
            if (s->state == SLOT_WAIT) {
                if (now >= s->deadline_ms) {
                    slot_start_connect(t, i, now);
                } else if (s->deadline_ms < wake) {
                    wake = s->deadline_ms;
                }
            }
            if (s->state == SLOT_CONNECTING) {
                int r = s->tr ? t->tr.connect(s->tr) : -1;
                if (r == 0 && now > s->deadline_ms) {
                    CFT_LOGW(TAG, "connection %d: connect timed out", i);
                    r = -1;
                }
                if (r < 0) {
                    CFT_LOGW(TAG, "connection %d: TLS connection to %s failed", i, t->cfg.edge_hosts[s->region]);
                    slot_close(t, i, now);
                    continue;
                }
                if (r == 1 && !slot_connected(t, i)) {
                    slot_close(t, i, now);
                    continue;
                }
            }
            if (s->state == SLOT_RUNNING) {
                cft_conn_tick(s->conn, now);
                if (!slot_flush(t, s) || (!cft_conn_is_alive(s->conn) && s->out_len == 0)) {
                    slot_close(t, i, now);
                    continue;
                }
                if (t->tr.pending(s->tr)) {
                    wake = now;
                }
            }
            if (s->state == SLOT_CONNECTING || s->state == SLOT_RUNNING) {
                int fd = t->tr.get_fd(s->tr);
                if (fd < 0) {
                    continue;
                }
                FD_SET(fd, &rfds);
                if (s->state == SLOT_CONNECTING || s->out_len) {
                    FD_SET(fd, &wfds);
                }
                if (fd > maxfd) {
                    maxfd = fd;
                }
            }
        }

        cft_poller_prepare(t->poller, &rfds, &wfds, &maxfd, &wake);

        int64_t wait = wake - now;
        if (wait < 0) {
            wait = 0;
        }
        struct timeval tv = {.tv_sec = (long)(wait / 1000), .tv_usec = (long)(wait % 1000) * 1000};
        if (maxfd >= 0) {
            select(maxfd + 1, &rfds, &wfds, NULL, &tv);
        } else {
            select(0, NULL, NULL, NULL, &tv);
        }

        now = cft_port_now_ms();
        /* Dispatch origin sockets first so their output is flushed to the edge in the same iteration */
        cft_poller_dispatch(t->poller, &rfds, &wfds, now);
        for (int i = 0; i < MAX_SLOTS; i++) {
            slot_t *s = &t->slots[i];
            if (s->state != SLOT_RUNNING) {
                continue;
            }
            int fd = t->tr.get_fd(s->tr);
            bool readable = (fd >= 0 && FD_ISSET(fd, &rfds)) || t->tr.pending(s->tr);
            if ((readable && !slot_read(t, s)) || !slot_flush(t, s)) {
                CFT_LOGW(TAG, "connection %d: connection to the edge lost", i);
                slot_close(t, i, now);
            }
        }
    }

    for (int i = 0; i < MAX_SLOTS; i++) {
        t->stopping = true;
        if (t->slots[i].state != SLOT_FREE) {
            slot_close(t, i, cft_port_now_ms());
        }
    }
}

void cft_tunnel_free(cft_tunnel_t *t)
{
    if (!t) {
        return;
    }
    for (int i = 0; i < MAX_SLOTS; i++) {
        free(t->slots[i].out);
    }
    run_posts(t); /* let posters free what they passed */
    pthread_mutex_destroy(&t->post_lock);
    cft_poller_free(t->poller);
    cft_ingress_free(t->ingress);
    memset(&t->token, 0, sizeof(t->token));
    free(t->rx);
    free(t);
}
