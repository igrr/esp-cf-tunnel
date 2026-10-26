/*
 * Host (Linux) runner: serves a demo handler through the tunnel, using the
 * same supervisor as the device over OpenSSL.
 *
 *   cft_host --token-file PATH [--connections N] [--connector-id UUID]
 *            [--edge HOST[:PORT]] [--plain] [--handler-delay-ms N]
 *            [--max-streams N] [--ingress] [--max-origins N]
 *            [--stream-window N] [--proxy-buffer N] [-v]
 *
 * With --ingress, requests are routed by the tunnel's public hostnames:
 * services on localhost come here, others are proxied.
 */
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cf_tunnel.h"
#include "transports.h"

int cft_host_log_level = 2;

static cft_tunnel_t *s_tunnel;
static int handler_delay_ms;

static void on_signal(int sig)
{
    (void)sig;
    cft_tunnel_request_stop(s_tunnel, 5000);
}

static char *read_file_trimmed(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        return NULL;
    }
    char *buf = calloc(1, 4096);
    size_t n = fread(buf, 1, 4095, f);
    fclose(f);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) {
        buf[--n] = '\0';
    }
    return buf;
}

static bool parse_uuid(const char *s, uint8_t out[16])
{
    int n = 0;
    for (; *s && n < 32; s++) {
        if (*s == '-') {
            continue;
        }
        unsigned v;
        if (sscanf(s, "%1x", &v) != 1) {
            return false;
        }
        out[n / 2] = (uint8_t)(n % 2 ? out[n / 2] | v : v << 4);
        n++;
    }
    return n == 32;
}

/* --- demo endpoints, also used by test_host.py --- */

static bool path_is(const cft_request_t *req, const char *path)
{
    size_t n = strcspn(req->path, "?");
    return n == strlen(path) && strncmp(req->path, path, n) == 0;
}

static void hello(cft_stream_t *s, const cft_request_t *req)
{
    char body[4096];
    int n = snprintf(body, sizeof(body),
                     "Hello from the esp-cf-tunnel host runner\n\n%s %s\nhost: %s\nservice: %s\n\nheaders:\n",
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

/* /echo: the request body, streamed back as it arrives */
static void echo_body(cft_stream_t *s, void *ctx, const uint8_t *data, size_t len)
{
    cft_stream_write(s, data, len);
}

static void echo_end(cft_stream_t *s, void *ctx)
{
    cft_stream_end(s);
}

/* /upload: whole body, then its length */
static void upload_done(cft_stream_t *s, void *ctx, const uint8_t *body, size_t len)
{
    char msg[64];
    int n = snprintf(msg, sizeof(msg), "received %zu bytes\n", len);
    cft_stream_respond(s, 200, "text/plain", msg, (size_t)n);
}

/* /big?size=N: N bytes of a repeating pattern, generated as the edge takes them */
typedef struct {
    size_t left;
    size_t pos;
} big_t;

static void big_fill(cft_stream_t *s, void *ctx)
{
    big_t *b = ctx;
    uint8_t chunk[4096];
    size_t n = b->left < sizeof(chunk) ? b->left : sizeof(chunk);
    for (size_t i = 0; i < n; i++) {
        chunk[i] = (uint8_t)('a' + (b->pos + i) % 26);
    }
    b->pos += n;
    b->left -= n;
    cft_stream_write(s, chunk, n);
    if (b->left == 0) {
        cft_stream_end(s);
    }
}

static void big_close(cft_stream_t *s, void *ctx, bool completed)
{
    free(ctx);
}

static void big(cft_stream_t *s, const cft_request_t *req)
{
    const char *q = strstr(req->path, "size=");
    big_t *b = calloc(1, sizeof(*b));
    b->left = q ? strtoul(q + 5, NULL, 10) : 100000;
    char clen[24];
    snprintf(clen, sizeof(clen), "%zu", b->left);
    cft_header_t hdrs[] = {{"Content-Type", "application/octet-stream"}, {"Content-Length", clen}};
    cft_stream_handlers_t h = {.on_writable = big_fill, .on_close = big_close};
    cft_stream_set_handlers(s, &h, b);
    cft_stream_send_headers(s, 200, hdrs, 2);
    big_fill(s, b);
}

/* /ws: echoes messages; "close" closes */
static void ws_message(cft_stream_t *s, void *ctx, cft_ws_opcode_t op, const uint8_t *data, size_t len)
{
    if (op == CFT_WS_TEXT && len == 5 && memcmp(data, "close", 5) == 0) {
        cft_ws_close(s, 1000, "bye");
        return;
    }
    cft_ws_send(s, op, data, len);
}

/* /later: answered from another thread through cft_tunnel_post(). The
 * context is freed by whichever runs last of the posted function and
 * on_close; both run on the tunnel thread. */
typedef struct {
    cft_stream_t *s; /* NULL once closed */
    bool answered;
} later_t;

static void later_respond(void *arg)
{
    later_t *l = arg;
    if (!l->s) {
        free(l);
        return;
    }
    l->answered = true;
    cft_stream_respond(l->s, 200, "text/plain", "later\n", 6);
}

static void *later_thread(void *arg)
{
    usleep(200 * 1000);
    cft_tunnel_post(s_tunnel, later_respond, arg);
    return NULL;
}

static void later_close(cft_stream_t *s, void *ctx, bool completed)
{
    later_t *l = ctx;
    if (l->answered) {
        free(l);
    } else {
        l->s = NULL;
    }
}

/* /pause: stops the request body at the first chunk for a second, then
 * counts it. Freed like /later. */
typedef struct {
    cft_stream_t *s; /* NULL once closed */
    size_t received;
    bool paused_once;
    bool resumed;
} pause_t;

static void pause_resume(void *arg)
{
    pause_t *p = arg;
    p->resumed = true;
    if (!p->s) {
        free(p);
        return;
    }
    cft_stream_pause_body(p->s, false);
}

static void *pause_thread(void *arg)
{
    usleep(1000 * 1000);
    cft_tunnel_post(s_tunnel, pause_resume, arg);
    return NULL;
}

static void pause_body(cft_stream_t *s, void *ctx, const uint8_t *data, size_t len)
{
    pause_t *p = ctx;
    p->received += len;
    if (!p->paused_once) {
        p->paused_once = true;
        cft_stream_pause_body(s, true);
        pthread_t thread;
        pthread_create(&thread, NULL, pause_thread, p);
        pthread_detach(thread);
    }
}

static void pause_end(cft_stream_t *s, void *ctx)
{
    pause_t *p = ctx;
    char msg[64];
    int n = snprintf(msg, sizeof(msg), "received %zu bytes\n", p->received);
    cft_stream_respond(s, 200, "text/plain", msg, (size_t)n);
}

static void pause_close(cft_stream_t *s, void *ctx, bool completed)
{
    pause_t *p = ctx;
    if (!p->paused_once || p->resumed) {
        free(p);
    } else {
        p->s = NULL;
    }
}

static void on_request(void *arg, cft_stream_t *s, const cft_request_t *req)
{
    if (handler_delay_ms) {
        usleep((useconds_t)handler_delay_ms * 1000);
    }
    if (req->is_websocket && path_is(req, "/ws")) {
        cft_stream_handlers_t h = {.on_ws_message = ws_message};
        cft_stream_set_handlers(s, &h, NULL);
        cft_ws_accept(s, NULL);
    } else if (path_is(req, "/echo")) {
        cft_stream_handlers_t h = {.on_body = echo_body, .on_body_end = echo_end};
        cft_stream_set_handlers(s, &h, NULL);
        cft_header_t hdrs[] = {{"Content-Type", "application/octet-stream"}};
        cft_stream_send_headers(s, 200, hdrs, 1);
    } else if (path_is(req, "/upload")) {
        cft_stream_read_body(s, 0, upload_done, NULL);
    } else if (path_is(req, "/big")) {
        big(s, req);
    } else if (path_is(req, "/pause")) {
        pause_t *p = calloc(1, sizeof(*p));
        p->s = s;
        cft_stream_handlers_t h = {.on_body = pause_body, .on_body_end = pause_end, .on_close = pause_close};
        cft_stream_set_handlers(s, &h, p);
    } else if (path_is(req, "/later")) {
        later_t *l = calloc(1, sizeof(*l));
        l->s = s;
        cft_stream_handlers_t h = {.on_close = later_close};
        cft_stream_set_handlers(s, &h, l);
        pthread_t thread;
        pthread_create(&thread, NULL, later_thread, l);
        pthread_detach(thread);
    } else {
        hello(s, req);
    }
}

static void on_event(void *arg, const cft_event_t *ev)
{
    (void)arg;
    switch (ev->type) {
    case CFT_EVENT_REGISTERED:
        printf("[%u] registered: connection %s at %s\n", ev->conn_index, ev->registered.connection_id,
               ev->registered.location);
        break;
    case CFT_EVENT_REGISTRATION_FAILED:
        printf("[%u] registration failed: %s\n", ev->conn_index, ev->failed.cause);
        break;
    case CFT_EVENT_CONFIG_UPDATED:
        printf("[%u] config version %d\n", ev->conn_index, (int)ev->config.version);
        break;
    case CFT_EVENT_DRAINING:
        printf("[%u] draining\n", ev->conn_index);
        break;
    case CFT_EVENT_UNREGISTERED:
        printf("[%u] unregistered\n", ev->conn_index);
        break;
    case CFT_EVENT_CLOSED:
        printf("[%u] closed: %s\n", ev->conn_index, ev->closed.reason);
        break;
    }
}

int main(int argc, char **argv)
{
    const char *token_file = NULL;
    cft_tunnel_config_t cfg = {
        .on_request = on_request,
        .on_event = on_event,
        .version = "esp-cf-tunnel-host/0.1",
        .arch = "linux_amd64",
    };
    uint8_t connector_id[16];

    static const struct option opts[] = {
        {"token-file", required_argument, NULL, 't'},
        {"connections", required_argument, NULL, 'n'},
        {"connector-id", required_argument, NULL, 'c'},
        {"edge", required_argument, NULL, 'e'},
        {"plain", no_argument, NULL, 'p'},
        {"handler-delay-ms", required_argument, NULL, 'd'},
        {"max-streams", required_argument, NULL, 'm'},
        {"ingress", no_argument, NULL, 'i'},
        {"max-origins", required_argument, NULL, 'o'},
        {"stream-window", required_argument, NULL, 'w'},
        {"proxy-buffer", required_argument, NULL, 'b'},
        {"verbose", no_argument, NULL, 'v'},
        {0},
    };
    int opt;
    const cft_transport_ops_t *transport = &cft_transport_openssl;
    while ((opt = getopt_long(argc, argv, "t:n:c:e:pd:m:io:w:b:v", opts, NULL)) != -1) {
        switch (opt) {
        case 't': token_file = optarg; break;
        case 'n': cfg.num_connections = (uint8_t)atoi(optarg); break;
        case 'c':
            if (!parse_uuid(optarg, connector_id)) {
                fprintf(stderr, "invalid connector ID\n");
                return 2;
            }
            cfg.connector_id = connector_id;
            break;
        case 'e': {
            /* Same host for both regions */
            char *colon = strrchr(optarg, ':');
            if (colon) {
                *colon = '\0';
                cfg.edge_port = (uint16_t)atoi(colon + 1);
            }
            cfg.edge_hosts[0] = cfg.edge_hosts[1] = optarg;
            break;
        }
        case 'p': transport = &cft_transport_plain; break;
        case 'd': handler_delay_ms = atoi(optarg); break;
        case 'm': cfg.max_concurrent_streams = (uint32_t)atoi(optarg); break;
        case 'i': cfg.use_ingress_rules = true; break;
        case 'o': cfg.max_origin_connections = (uint8_t)atoi(optarg); break;
        case 'w': cfg.stream_window = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'b': cfg.proxy_buffer_size = strtoul(optarg, NULL, 0); break;
        case 'v': cft_host_log_level = 3; break;
        default: return 2;
        }
    }
    if (!token_file) {
        fprintf(stderr, "usage: %s --token-file PATH [--connections N] [--connector-id UUID] "
                "[--edge HOST[:PORT]] [--plain] [--handler-delay-ms N] [--max-streams N] [--ingress] "
                "[--max-origins N] [--stream-window N] [--proxy-buffer N] [-v]\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    char *token = read_file_trimmed(token_file);
    if (!token) {
        return 1;
    }
    cfg.token = token;
    s_tunnel = cft_tunnel_new(&cfg, transport);
    memset(token, 0, strlen(token));
    free(token);
    if (!s_tunnel) {
        fprintf(stderr, "invalid token\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    cft_tunnel_run(s_tunnel);
    cft_tunnel_free(s_tunnel);
    printf("stopped\n");
    return 0;
}
