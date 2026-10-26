/*
 * cft_transport_ops_t over plain TCP, for tests against fake_edge.py.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "transports.h"

typedef struct {
    int fd;
    bool connected;
} plain_conn_t;

static void *plain_open(void *ctx, const char *host, uint16_t port, const char *sni, const char *ca_pem)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        return NULL;
    }
    plain_conn_t *c = calloc(1, sizeof(*c));
    c->fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (c->fd >= 0) {
        fcntl(c->fd, F_SETFL, fcntl(c->fd, F_GETFL) | O_NONBLOCK);
        if (connect(c->fd, res->ai_addr, res->ai_addrlen) != 0 && errno != EINPROGRESS) {
            close(c->fd);
            c->fd = -1;
        }
    }
    freeaddrinfo(res);
    if (c->fd < 0) {
        free(c);
        return NULL;
    }
    return c;
}

static int plain_connect(void *conn)
{
    plain_conn_t *c = conn;
    if (c->connected) {
        return 1;
    }
    struct pollfd pfd = {.fd = c->fd, .events = POLLOUT};
    if (poll(&pfd, 1, 0) <= 0) {
        return 0;
    }
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
        return -1;
    }
    c->connected = true;
    return 1;
}

static int plain_get_fd(void *conn)
{
    return ((plain_conn_t *)conn)->fd;
}

static long plain_read(void *conn, uint8_t *buf, size_t len)
{
    ssize_t n = recv(((plain_conn_t *)conn)->fd, buf, len, 0);
    if (n > 0) {
        return n;
    }
    return (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) ? 0 : -1;
}

static long plain_write(void *conn, const uint8_t *buf, size_t len)
{
    ssize_t n = send(((plain_conn_t *)conn)->fd, buf, len, MSG_NOSIGNAL);
    if (n >= 0) {
        return n;
    }
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
}

static size_t plain_pending(void *conn)
{
    return 0;
}

static size_t plain_local_ip(void *conn, uint8_t ip[16])
{
    return 0;
}

static void plain_close(void *conn)
{
    close(((plain_conn_t *)conn)->fd);
    free(conn);
}

const cft_transport_ops_t cft_transport_plain = {
    .open = plain_open,
    .connect = plain_connect,
    .get_fd = plain_get_fd,
    .read = plain_read,
    .write = plain_write,
    .pending = plain_pending,
    .local_ip = plain_local_ip,
    .close = plain_close,
};
