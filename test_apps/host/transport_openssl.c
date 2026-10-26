/*
 * cft_transport_ops_t on OpenSSL over a non-blocking socket (host builds).
 * Offers only the key exchange groups mbedTLS has (no post-quantum), like
 * the device.
 */
#include "transports.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int fd;
    SSL_CTX *ctx;
    SSL *ssl;
    bool tcp_connected;
} ossl_conn_t;

static void ossl_close(void *conn)
{
    ossl_conn_t *c = conn;
    if (c->ssl) {
        SSL_free(c->ssl);
    }
    if (c->ctx) {
        SSL_CTX_free(c->ctx);
    }
    if (c->fd >= 0) {
        close(c->fd);
    }
    free(c);
}

static SSL_CTX *make_ctx(const char *ca_pem)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        return NULL;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set1_groups_list(ctx, "X25519:P-256");
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    BIO *bio = BIO_new_mem_buf(ca_pem, -1);
    X509 *ca = PEM_read_bio_X509(bio, NULL, NULL, NULL);
    BIO_free(bio);
    if (!ca || X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx), ca) != 1) {
        X509_free(ca);
        SSL_CTX_free(ctx);
        return NULL;
    }
    X509_free(ca);
    return ctx;
}

static void *ossl_open(void *ctx, const char *host, uint16_t port, const char *sni, const char *ca_pem)
{
    (void)ctx;
    ossl_conn_t *c = calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->fd = -1;
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        free(c);
        return NULL;
    }
    int count = 0;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        count++;
    }
    struct addrinfo *ai = res;
    for (int pick = rand() % count; pick > 0; pick--) {
        ai = ai->ai_next;
    }
    c->fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (c->fd >= 0) {
        fcntl(c->fd, F_SETFL, fcntl(c->fd, F_GETFL) | O_NONBLOCK);
        if (connect(c->fd, ai->ai_addr, ai->ai_addrlen) != 0 && errno != EINPROGRESS) {
            close(c->fd);
            c->fd = -1;
        }
    }
    freeaddrinfo(res);
    c->ctx = c->fd >= 0 ? make_ctx(ca_pem) : NULL;
    c->ssl = c->ctx ? SSL_new(c->ctx) : NULL;
    if (!c->ssl) {
        ossl_close(c);
        return NULL;
    }
    SSL_set_fd(c->ssl, c->fd);
    SSL_set_tlsext_host_name(c->ssl, sni);
    SSL_set1_host(c->ssl, sni);
    return c;
}

static int ossl_connect(void *conn)
{
    ossl_conn_t *c = conn;
    if (!c->tcp_connected) {
        struct pollfd pfd = {.fd = c->fd, .events = POLLOUT};
        if (poll(&pfd, 1, 0) <= 0) {
            return 0;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            return -1;
        }
        c->tcp_connected = true;
    }
    int r = SSL_connect(c->ssl);
    if (r == 1) {
        return 1;
    }
    int e = SSL_get_error(c->ssl, r);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
        return 0;
    }
    ERR_print_errors_fp(stderr);
    return -1;
}

static int ossl_get_fd(void *conn)
{
    return ((ossl_conn_t *)conn)->fd;
}

static long ossl_read(void *conn, uint8_t *buf, size_t len)
{
    ossl_conn_t *c = conn;
    int n = SSL_read(c->ssl, buf, (int)len);
    if (n > 0) {
        return n;
    }
    int e = SSL_get_error(c->ssl, n);
    return (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) ? 0 : -1;
}

static long ossl_write(void *conn, const uint8_t *buf, size_t len)
{
    ossl_conn_t *c = conn;
    int n = SSL_write(c->ssl, buf, (int)len);
    if (n > 0) {
        return n;
    }
    int e = SSL_get_error(c->ssl, n);
    return (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) ? 0 : -1;
}

static size_t ossl_pending(void *conn)
{
    return (size_t)SSL_pending(((ossl_conn_t *)conn)->ssl);
}

static size_t ossl_local_ip(void *conn, uint8_t ip[16])
{
    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    if (getsockname(((ossl_conn_t *)conn)->fd, (struct sockaddr *)&local, &len) != 0 ||
            local.sin_family != AF_INET) {
        return 0;
    }
    memcpy(ip, &local.sin_addr, 4);
    return 4;
}

const cft_transport_ops_t cft_transport_openssl = {
    .open = ossl_open,
    .connect = ossl_connect,
    .get_fd = ossl_get_fd,
    .read = ossl_read,
    .write = ossl_write,
    .pending = ossl_pending,
    .local_ip = ossl_local_ip,
    .close = ossl_close,
};
