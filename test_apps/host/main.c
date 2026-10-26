/*
 * Host (Linux) runner: serves a demo handler through the tunnel, using the
 * same supervisor as the device over OpenSSL.
 *
 *   cft_host --token-file PATH [--connections N] [--connector-id UUID]
 *            [--edge HOST[:PORT]] [--plain] [--handler-delay-ms N] [-v]
 */
#include <getopt.h>
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

static void demo_handler(void *arg, const cft_request_t *req, cft_response_t *resp)
{
    (void)arg;
    if (handler_delay_ms) {
        usleep((useconds_t)handler_delay_ms * 1000);
    }
    char body[4096];
    int n = snprintf(body, sizeof(body),
                     "Hello from the esp-cf-tunnel host runner\n\n%s %s\nhost: %s\nbody: %zu bytes\n\nheaders:\n",
                     req->method, req->path, req->authority, req->body_len);
    for (size_t i = 0; i < req->num_headers && n < (int)sizeof(body) - 1; i++) {
        n += snprintf(body + n, sizeof(body) - (size_t)n, "  %s: %s\n", req->headers[i].name,
                      req->headers[i].value);
    }
    if (n >= (int)sizeof(body)) {
        n = sizeof(body) - 1;
    }
    cft_response_add_header(resp, "Content-Type", "text/plain; charset=utf-8");
    cft_response_add_header(resp, "X-Served-By", "esp-cf-tunnel");
    cft_response_set_body(resp, body, (size_t)n);
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
        .handler = demo_handler,
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
        {"verbose", no_argument, NULL, 'v'},
        {0},
    };
    int opt;
    const cft_transport_ops_t *transport = &cft_transport_openssl;
    while ((opt = getopt_long(argc, argv, "t:n:c:e:pd:m:v", opts, NULL)) != -1) {
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
        case 'v': cft_host_log_level = 3; break;
        default: return 2;
        }
    }
    if (!token_file) {
        fprintf(stderr, "usage: %s --token-file PATH [--connections N] [--connector-id UUID] "
                "[--edge HOST[:PORT]] [--plain] [--handler-delay-ms N] [--max-streams N] [-v]\n", argv[0]);
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
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    cft_tunnel_run(s_tunnel);
    cft_tunnel_free(s_tunnel);
    printf("stopped\n");
    return 0;
}
