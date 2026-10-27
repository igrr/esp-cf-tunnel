/*
 * Host (Linux) runner: serves the test handlers (test_apps/common) through
 * the tunnel, using the same supervisor as the device over OpenSSL.
 *
 *   cft_host --token-file PATH [--connections N] [--connector-id UUID]
 *            [--edge HOST[:PORT]] [--plain] [--handler-delay-ms N]
 *            [--max-streams N] [--ingress] [--max-origins N]
 *            [--stream-window N] [--proxy-buffer N] [--posters N]
 *            [--lan-http URL] [--lan-tcp URL] [--lan-dead URL] [-v]
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
#include "test_handlers.h"
#include "transports.h"

int cft_host_log_level = 2;

static cft_tunnel_t *s_tunnel;

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

int main(int argc, char **argv)
{
    const char *token_file = NULL;
    cft_tunnel_config_t cfg = {
        .on_request = th_on_request,
        .on_event = th_on_event,
        .version = "esp-cf-tunnel-host/0.1",
        .arch = "linux_amd64",
    };
    uint8_t connector_id[16];
    th_config_t th = {0};

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
        {"posters", required_argument, NULL, 'P'},
        {"lan-http", required_argument, NULL, 'H'},
        {"lan-tcp", required_argument, NULL, 'T'},
        {"lan-dead", required_argument, NULL, 'D'},
        {"verbose", no_argument, NULL, 'v'},
        {0},
    };
    int opt;
    const cft_transport_ops_t *transport = &cft_transport_openssl;
    while ((opt = getopt_long(argc, argv, "t:n:c:e:pd:m:io:w:b:P:H:T:D:v", opts, NULL)) != -1) {
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
        case 'd': th.handler_delay_ms = atoi(optarg); break;
        case 'm': cfg.max_concurrent_streams = (uint32_t)atoi(optarg); break;
        case 'i': cfg.use_ingress_rules = true; break;
        case 'o': cfg.max_origin_connections = (uint8_t)atoi(optarg); break;
        case 'w': cfg.stream_window = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'b': cfg.proxy_buffer_size = strtoul(optarg, NULL, 0); break;
        case 'P': th.poster_threads = atoi(optarg); break;
        case 'H': th.lan_http = optarg; break;
        case 'T': th.lan_tcp = optarg; break;
        case 'D': th.lan_dead = optarg; break;
        case 'v': cft_host_log_level = 3; break;
        default: return 2;
        }
    }
    if (!token_file) {
        fprintf(stderr, "usage: %s --token-file PATH [--connections N] [--connector-id UUID] "
                "[--edge HOST[:PORT]] [--plain] [--handler-delay-ms N] [--max-streams N] [--ingress] "
                "[--max-origins N] [--stream-window N] [--proxy-buffer N] [--posters N] [--lan-http URL] "
                "[--lan-tcp URL] [--lan-dead URL] [-v]\n", argv[0]);
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
    th_init(&th);
    if (th_start(s_tunnel) < 0) {
        fprintf(stderr, "cannot start threads\n");
        return 1;
    }
    cft_tunnel_run(s_tunnel);
    th_stop();
    cft_tunnel_free(s_tunnel);
    printf("stopped\n");
    return 0;
}
