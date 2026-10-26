/*
 * Host (Linux) runner for the tunnel core: one connection to the edge over
 * OpenSSL, serving a demo handler. Used to develop and check the protocol
 * code before running it on a device.
 *
 *   cft_host --token-file PATH [--edge HOST] [--conn-index N] [-v]
 */
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "cf_tunnel.h"

int cft_host_log_level = 2;

static volatile sig_atomic_t stop_requested;
static bool unregistered;
static bool closed;

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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

static int tcp_connect(const char *host, char *ip_out, size_t ip_out_size)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res;
    char port[8];
    snprintf(port, sizeof(port), "%d", CFT_EDGE_PORT);
    int rv = getaddrinfo(host, port, &hints, &res);
    if (rv != 0) {
        fprintf(stderr, "resolve %s: %s\n", host, gai_strerror(rv));
        return -1;
    }
    int count = 0;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        count++;
    }
    /* Pick one address at random, like cloudflared's edge address pool. */
    int pick = rand() % count;
    struct addrinfo *ai = res;
    while (pick--) {
        ai = ai->ai_next;
    }
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    inet_ntop(AF_INET, &((struct sockaddr_in *)ai->ai_addr)->sin_addr, ip_out, (socklen_t)ip_out_size);
    if (fd < 0 || connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
        perror("connect");
        if (fd >= 0) {
            close(fd);
        }
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static SSL_CTX *make_tls_ctx(void)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    /* What mbedTLS on the device can offer: no post-quantum groups. */
    SSL_CTX_set1_groups_list(ctx, "X25519:P-256");
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    BIO *bio = BIO_new_mem_buf(cft_edge_ca_pem_str, -1);
    X509 *ca = PEM_read_bio_X509(bio, NULL, NULL, NULL);
    BIO_free(bio);
    if (!ca || X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx), ca) != 1) {
        fprintf(stderr, "failed to load edge CA\n");
        exit(1);
    }
    X509_free(ca);
    return ctx;
}

/* --- demo handler --- */

static int handler_delay_ms;

static void demo_handler(void *arg, const cft_request_t *req, cft_response_t *resp)
{
    (void)arg;
    if (handler_delay_ms) {
        usleep((useconds_t)handler_delay_ms * 1000);
    }
    char body[4096];
    int n = snprintf(body, sizeof(body),
                     "Hello from the cf_tunnel host prototype\n\n%s %s\nhost: %s\nbody: %zu bytes\n\nheaders:\n",
                     req->method, req->path, req->authority, req->body_len);
    for (size_t i = 0; i < req->num_headers && n < (int)sizeof(body) - 1; i++) {
        n += snprintf(body + n, sizeof(body) - (size_t)n, "  %s: %s\n", req->headers[i].name,
                      req->headers[i].value);
    }
    if (n >= (int)sizeof(body)) {
        n = sizeof(body) - 1;
    }
    cft_response_add_header(resp, "Content-Type", "text/plain; charset=utf-8");
    cft_response_add_header(resp, "X-Served-By", "cf_tunnel");
    cft_response_set_body(resp, body, (size_t)n);
}

static void on_event(void *arg, const cft_event_t *ev)
{
    (void)arg;
    switch (ev->type) {
    case CFT_EVENT_REGISTERED:
        printf("registered: connection %s at %s (remotely managed: %s)\n", ev->registered.connection_id,
               ev->registered.location, ev->registered.remotely_managed ? "yes" : "no");
        break;
    case CFT_EVENT_REGISTRATION_FAILED:
        printf("registration failed: %s (retry: %s, after %lld ms)\n", ev->failed.cause,
               ev->failed.should_retry ? "yes" : "no", (long long)ev->failed.retry_after_ms);
        break;
    case CFT_EVENT_CONFIG_UPDATED:
        printf("config version %d: %.*s\n", (int)ev->config.version, (int)ev->config.json_len, ev->config.json);
        break;
    case CFT_EVENT_UNREGISTERED:
        printf("unregistered\n");
        unregistered = true;
        break;
    case CFT_EVENT_CLOSED:
        printf("closed: %s\n", ev->closed.reason);
        closed = true;
        break;
    }
    fflush(stdout);
}

static int flush_out(cft_conn_t *conn, SSL *ssl)
{
    for (;;) {
        const uint8_t *data;
        long n = cft_conn_send_data(conn, &data);
        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            return 0;
        }
        size_t written = 0;
        while (written < (size_t)n) {
            int w = SSL_write(ssl, data + written, (int)((size_t)n - written));
            if (w <= 0) {
                fprintf(stderr, "SSL_write failed: %d\n", SSL_get_error(ssl, w));
                return -1;
            }
            written += (size_t)w;
        }
    }
}

int main(int argc, char **argv)
{
    const char *token_file = NULL;
    const char *edge = CFT_EDGE_REGION1;
    int conn_index = 0;
    const char *version = "cf_tunnel-host/0.1";

    static const struct option opts[] = {
        {"token-file", required_argument, NULL, 't'},
        {"edge", required_argument, NULL, 'e'},
        {"conn-index", required_argument, NULL, 'i'},
        {"version", required_argument, NULL, 'V'},
        {"verbose", no_argument, NULL, 'v'},
        {"handler-delay-ms", required_argument, NULL, 'd'},
        {"max-streams", required_argument, NULL, 'm'},
        {0},
    };
    int opt;
    uint32_t max_streams = 0;
    while ((opt = getopt_long(argc, argv, "t:e:i:V:vd:m:", opts, NULL)) != -1) {
        switch (opt) {
        case 't': token_file = optarg; break;
        case 'e': edge = optarg; break;
        case 'i': conn_index = atoi(optarg); break;
        case 'V': version = optarg; break;
        case 'v': cft_host_log_level = 3; break;
        case 'd': handler_delay_ms = atoi(optarg); break;
        case 'm': max_streams = (uint32_t)atoi(optarg); break;
        default: return 2;
        }
    }
    if (!token_file) {
        fprintf(stderr, "usage: %s --token-file PATH [--edge HOST] [--conn-index N] [--version STR] [-v]\n",
                argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    char *token_str = read_file_trimmed(token_file);
    cft_token_t token;
    if (!token_str || !cft_token_parse(token_str, &token)) {
        fprintf(stderr, "invalid token\n");
        return 1;
    }
    memset(token_str, 0, strlen(token_str));
    free(token_str);

    char ip[INET_ADDRSTRLEN] = "";
    int fd = tcp_connect(edge, ip, sizeof(ip));
    if (fd < 0) {
        return 1;
    }
    printf("connected to %s (%s)\n", edge, ip);

    SSL_CTX *ctx = make_tls_ctx();
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, CFT_EDGE_SNI);
    SSL_set1_host(ssl, CFT_EDGE_SNI);
    if (SSL_connect(ssl) != 1) {
        fprintf(stderr, "TLS handshake failed\n");
        ERR_print_errors_fp(stderr);
        return 1;
    }
    printf("TLS: %s %s, group %s\n", SSL_get_version(ssl), SSL_get_cipher(ssl),
           SSL_group_to_name(ssl, SSL_get_negotiated_group(ssl)));

    struct sockaddr_in local;
    socklen_t local_len = sizeof(local);
    getsockname(fd, (struct sockaddr *)&local, &local_len);

    cft_conn_config_t cfg = {
        .token = &token,
        .conn_index = (uint8_t)conn_index,
        .version = version,
        .arch = "linux_amd64",
        .local_ip = (const uint8_t *) &local.sin_addr,
        .local_ip_len = 4,
        .handler = demo_handler,
        .on_event = on_event,
        .max_concurrent_streams = max_streams,
    };
    cft_conn_t *conn = cft_conn_new(&cfg);
    if (!conn || flush_out(conn, ssl) < 0) {
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    int64_t shutdown_deadline = 0;
    uint8_t buf[16384];
    while (!closed) {
        if (stop_requested && !shutdown_deadline) {
            printf("shutting down\n");
            cft_conn_shutdown(conn);
            shutdown_deadline = now_ms() + 5000;
        }
        if (shutdown_deadline && now_ms() > shutdown_deadline) {
            printf("shutdown timed out\n");
            break;
        }
        if (flush_out(conn, ssl) < 0) {
            break;
        }
        if (!SSL_pending(ssl)) {
            struct pollfd pfd = {.fd = fd, .events = POLLIN};
            int pr = poll(&pfd, 1, 500);
            if (pr < 0 && errno != EINTR) {
                perror("poll");
                break;
            }
            if (pr <= 0) {
                continue;
            }
        }
        int n = SSL_read(ssl, buf, sizeof(buf));
        if (n <= 0) {
            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_READ) {
                continue;
            }
            printf("edge closed the connection (SSL error %d)\n", err);
            break;
        }
        cft_conn_recv(conn, buf, (size_t)n);
    }
    flush_out(conn, ssl);
    cft_conn_free(conn);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(fd);
    return unregistered || !closed ? 0 : 1;
}
