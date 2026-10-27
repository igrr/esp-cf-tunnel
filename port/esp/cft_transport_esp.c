/*
 * cft_transport_ops_t on mbedTLS over a non-blocking lwIP socket.
 *
 * Not esp-tls: its non-blocking connect waits for the TCP connection with
 * select() on fd sets kept in the connection, which a select() timeout
 * clears and nothing refills, so a TCP connect slower than the first call's
 * timeout never completes. Here the supervisor's select loop waits for the
 * socket, and each step (TCP connect, TLS handshake) only does what is ready.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cf_tunnel_esp.h"
#include "esp_log.h"
#include "esp_random.h"
#include "lwip/sockets.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/version.h"
#include "mbedtls/x509_crt.h"
#if MBEDTLS_VERSION_MAJOR >= 4
#include "psa/crypto.h"
#endif

static const char *TAG = "cft_transport";

typedef struct {
    int fd;
    bool tcp_connected;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
} esp_conn_t;

#if MBEDTLS_VERSION_MAJOR < 4
static int rng(void *ctx, unsigned char *buf, size_t len)
{
    esp_fill_random(buf, len);
    return 0;
}
#endif

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    esp_conn_t *c = ctx;
    ssize_t n = send(c->fd, buf, len, 0);
    if (n < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? MBEDTLS_ERR_SSL_WANT_WRITE
               : MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return (int)n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    esp_conn_t *c = ctx;
    ssize_t n = recv(c->fd, buf, len, 0);
    if (n < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? MBEDTLS_ERR_SSL_WANT_READ
               : MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return n == 0 ? MBEDTLS_ERR_NET_CONN_RESET : (int)n;
}

static void esp_close(void *conn)
{
    esp_conn_t *c = conn;
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_x509_crt_free(&c->ca);
    if (c->fd >= 0) {
        close(c->fd);
    }
    free(c);
}

static int tls_setup(esp_conn_t *c, const char *sni, const char *ca_pem)
{
    int ret = mbedtls_x509_crt_parse(&c->ca, (const unsigned char *)ca_pem, strlen(ca_pem) + 1);
    if (ret != 0) {
        ESP_LOGE(TAG, "CA certificate: -0x%04x", (unsigned) - ret);
        return -1;
    }
    ret = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        return -1;
    }
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
    mbedtls_ssl_conf_min_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_3);
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_ssl_conf_rng(&c->conf, rng, NULL);
#endif
    if (mbedtls_ssl_setup(&c->ssl, &c->conf) != 0 || mbedtls_ssl_set_hostname(&c->ssl, sni) != 0) {
        return -1;
    }
    mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);
    return 0;
}

/* Resolves the host (this blocks) and starts a non-blocking TCP connect. */
static int tcp_start(esp_conn_t *c, const char *host, uint16_t port)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "cannot resolve %s", host);
        return -1;
    }
    c->fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (c->fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    fcntl(c->fd, F_SETFL, fcntl(c->fd, F_GETFL, 0) | O_NONBLOCK);
    int on = 1, idle = 30, interval = 5, count = 3;
    setsockopt(c->fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
    setsockopt(c->fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(c->fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(c->fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
    int rv = connect(c->fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rv < 0 && errno != EINPROGRESS) {
        ESP_LOGW(TAG, "connect to %s failed: %d", host, errno);
        return -1;
    }
    return 0;
}

static void *esp_open(void *ctx, const char *host, uint16_t port, const char *sni, const char *ca_pem)
{
#if MBEDTLS_VERSION_MAJOR >= 4
    if (psa_crypto_init() != PSA_SUCCESS) {
        return NULL;
    }
#endif
    esp_conn_t *c = calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->fd = -1;
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_x509_crt_init(&c->ca);
    if (tls_setup(c, sni, ca_pem) < 0 || tcp_start(c, host, port) < 0) {
        esp_close(c);
        return NULL;
    }
    return c;
}

static int esp_connect(void *conn)
{
    esp_conn_t *c = conn;
    if (!c->tcp_connected) {
        fd_set w;
        FD_ZERO(&w);
        FD_SET(c->fd, &w);
        struct timeval tv = {0};
        if (select(c->fd + 1, NULL, &w, NULL, &tv) <= 0) {
            return 0;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
            ESP_LOGW(TAG, "TCP connect failed: %d", err);
            return -1;
        }
        c->tcp_connected = true;
    }
    int ret = mbedtls_ssl_handshake(&c->ssl);
    if (ret == 0) {
        return 1;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 0;
    }
    ESP_LOGW(TAG, "TLS handshake failed: -0x%04x", (unsigned) - ret);
    return -1;
}

static int esp_get_fd(void *conn)
{
    return ((esp_conn_t *)conn)->fd;
}

static long esp_read(void *conn, uint8_t *buf, size_t len)
{
    esp_conn_t *c = conn;
    for (;;) {
        int n = mbedtls_ssl_read(&c->ssl, buf, len);
        if (n > 0) {
            return n;
        }
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return 0;
        }
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
        if (n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            continue; /* TLS 1.3 ticket after the handshake; not used */
        }
#endif
        if (n != 0 && n != MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY && n != MBEDTLS_ERR_NET_CONN_RESET) {
            ESP_LOGD(TAG, "read failed: -0x%04x", (unsigned) - n);
        }
        return -1;
    }
}

static long esp_write(void *conn, const uint8_t *buf, size_t len)
{
    esp_conn_t *c = conn;
    int n = mbedtls_ssl_write(&c->ssl, buf, len);
    if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 0;
    }
    if (n < 0) {
        ESP_LOGD(TAG, "write failed: -0x%04x", (unsigned) - n);
        return -1;
    }
    return n;
}

static size_t esp_pending(void *conn)
{
    return mbedtls_ssl_get_bytes_avail(&((esp_conn_t *)conn)->ssl);
}

static size_t esp_local_ip(void *conn, uint8_t ip[16])
{
    int fd = esp_get_fd(conn);
    struct sockaddr_storage local = {0};
    socklen_t len = sizeof(local);
    if (fd < 0 || getsockname(fd, (struct sockaddr *)&local, &len) != 0) {
        return 0;
    }
    if (local.ss_family == AF_INET) {
        memcpy(ip, &((struct sockaddr_in *)&local)->sin_addr, 4);
        return 4;
    }
#if CONFIG_LWIP_IPV6
    if (local.ss_family == AF_INET6) {
        memcpy(ip, &((struct sockaddr_in6 *)&local)->sin6_addr, 16);
        return 16;
    }
#endif
    return 0;
}

const cft_transport_ops_t cft_transport_esp = {
    .open = esp_open,
    .connect = esp_connect,
    .get_fd = esp_get_fd,
    .read = esp_read,
    .write = esp_write,
    .pending = esp_pending,
    .local_ip = esp_local_ip,
    .close = esp_close,
};
