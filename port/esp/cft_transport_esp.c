/*
 * cft_transport_ops_t on esp-tls in non-blocking mode.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "cf_tunnel_esp.h"
#include "esp_log.h"
#include "esp_tls.h"

static const char *TAG = "cft_transport";

typedef struct {
    esp_tls_t *tls;
    esp_tls_cfg_t cfg;
    tls_keep_alive_cfg_t keep_alive;
    char *host;
    uint16_t port;
} esp_conn_t;

static void *esp_open(void *ctx, const char *host, uint16_t port, const char *sni, const char *ca_pem)
{
    esp_conn_t *c = calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->tls = esp_tls_init();
    c->host = strdup(host);
    if (!c->tls || !c->host) {
        if (c->tls) {
            esp_tls_conn_destroy(c->tls);
        }
        free(c->host);
        free(c);
        return NULL;
    }
    c->port = port;
    c->keep_alive = (tls_keep_alive_cfg_t) {
        .keep_alive_enable = true,
        .keep_alive_idle = 30,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
    };
    c->cfg = (esp_tls_cfg_t) {
        .cacert_buf = (const unsigned char *)ca_pem,
        .cacert_bytes = strlen(ca_pem) + 1,
        .common_name = sni, /* also used as SNI */
        .tls_version = ESP_TLS_VER_TLS_1_3,
        .non_block = true,
        /* esp-tls select()s on the connecting socket for this long per call
         * (0 would mean forever); the supervisor enforces the real timeout. */
        .timeout_ms = 1,
        .keep_alive_cfg = &c->keep_alive,
    };
    return c;
}

static int esp_connect(void *conn)
{
    esp_conn_t *c = conn;
    int r = esp_tls_conn_new_async(c->host, (int)strlen(c->host), c->port, &c->cfg, c->tls);
    return r < 0 ? -1 : r;
}

static int esp_get_fd(void *conn)
{
    esp_conn_t *c = conn;
    int fd = -1;
    esp_tls_get_conn_sockfd(c->tls, &fd);
    return fd;
}

static long esp_read(void *conn, uint8_t *buf, size_t len)
{
    esp_conn_t *c = conn;
    ssize_t n = esp_tls_conn_read(c->tls, buf, len);
    if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
        return 0;
    }
    if (n == 0) {
        return -1; /* closed by the peer */
    }
    if (n < 0) {
        ESP_LOGD(TAG, "read failed: -0x%x", (unsigned)(-n));
    }
    return n;
}

static long esp_write(void *conn, const uint8_t *buf, size_t len)
{
    esp_conn_t *c = conn;
    ssize_t n = esp_tls_conn_write(c->tls, buf, len);
    if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
        return 0;
    }
    if (n < 0) {
        ESP_LOGD(TAG, "write failed: -0x%x", (unsigned)(-n));
    }
    return n;
}

static size_t esp_pending(void *conn)
{
    esp_conn_t *c = conn;
    ssize_t n = esp_tls_get_bytes_avail(c->tls);
    return n > 0 ? (size_t)n : 0;
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

static void esp_close(void *conn)
{
    esp_conn_t *c = conn;
    esp_tls_conn_destroy(c->tls);
    free(c->host);
    free(c);
}

const cft_transport_ops_t cft_transport_esp_tls = {
    .open = esp_open,
    .connect = esp_connect,
    .get_fd = esp_get_fd,
    .read = esp_read,
    .write = esp_write,
    .pending = esp_pending,
    .local_ip = esp_local_ip,
    .close = esp_close,
};
