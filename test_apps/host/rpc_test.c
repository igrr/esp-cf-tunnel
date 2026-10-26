/*
 * Runs the registration RPC client over plain TCP against
 * mock_registration_server.py: register, then unregister.
 *
 *   rpc_test PORT [--fail]
 * Exit status 0 if the expected events arrived.
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cft_rpc.h"

int cft_host_log_level = 3;

static int fd = -1;
static int registered, failed, unregistered, protocol_error;

static int send_cb(void *arg, const uint8_t *data, size_t len)
{
    (void)arg;
    return write(fd, data, len) == (ssize_t)len ? 0 : -1;
}

static void event_cb(void *arg, const cft_rpc_event_t *ev)
{
    cft_rpc_t *rpc = *(cft_rpc_t **)arg;
    switch (ev->type) {
    case CFT_RPC_REGISTERED:
        printf("REGISTERED location=%s managed=%d uuid[0]=%02x\n", ev->details.location,
               ev->details.remotely_managed, ev->details.uuid[0]);
        registered = 1;
        cft_rpc_unregister(rpc);
        break;
    case CFT_RPC_REGISTER_FAILED:
        printf("FAILED cause=%s retry_after_ns=%lld should_retry=%d\n", ev->error.cause,
               (long long)ev->error.retry_after_ns, ev->error.should_retry);
        failed = 1;
        break;
    case CFT_RPC_UNREGISTERED:
        printf("UNREGISTERED\n");
        unregistered = 1;
        break;
    case CFT_RPC_PROTOCOL_ERROR:
        printf("PROTOCOL_ERROR %s\n", ev->reason);
        protocol_error = 1;
        break;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        return 2;
    }
    bool expect_fail = argc > 2 && strcmp(argv[2], "--fail") == 0;
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[1]))};
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        perror("connect");
        return 1;
    }

    static const uint8_t secret[32] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    static const uint8_t tunnel_id[16] = {0xe9, 0xb6, 0x08, 0x7f, 0xd1, 0x55, 0x4c, 0xca,
                                          0xbb, 0x1b, 0x07, 0xa7, 0x7b, 0xfc, 0x43, 0x06
                                         };
    static const uint8_t client_id[16] = {0xaa, 0xbb};
    static const uint8_t ip[4] = {192, 168, 1, 2};
    const char *features[] = {"serialized_headers", "allow_remote_config"};
    cft_rpc_register_params_t p = {
        .account_tag = expect_fail ? "fail-me" : "acct-tag",
        .tunnel_secret = secret,
        .tunnel_secret_len = sizeof(secret),
        .tunnel_id = tunnel_id,
        .conn_index = 2,
        .client_id = client_id,
        .features = features,
        .num_features = 2,
        .version = "test/1.0",
        .arch = "esp32p4",
        .origin_local_ip = ip,
        .origin_local_ip_len = 4,
        .num_previous_attempts = 3,
    };
    cft_rpc_t *rpc = NULL;
    rpc = cft_rpc_new(&p, send_cb, event_cb, &rpc);
    cft_rpc_start(rpc);

    uint8_t buf[4096];
    while (!unregistered && !failed && !protocol_error) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            printf("EOF\n");
            break;
        }
        /* Feed byte by byte to exercise frame reassembly. */
        for (ssize_t i = 0; i < n; i++) {
            if (cft_rpc_feed(rpc, buf + i, 1) < 0) {
                break;
            }
        }
    }
    cft_rpc_free(rpc);
    close(fd);
    if (expect_fail) {
        return failed ? 0 : 1;
    }
    return registered && unregistered ? 0 : 1;
}
