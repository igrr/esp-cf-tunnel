/*
 * Client side of the tunnel registration RPC (RegistrationServer in
 * cloudflared's tunnelrpc.capnp), spoken over the control stream.
 *
 * Sequence: Bootstrap -> Return(cap) -> Finish, then
 * registerConnection -> Return(ConnectionResponse) -> Finish. On shutdown,
 * unregisterConnection -> Return -> Finish, then Release of the bootstrap cap.
 *
 * No I/O: bytes received on the stream are passed to cft_rpc_feed(), outgoing
 * frames go to the send callback.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *account_tag;
    const uint8_t *tunnel_secret;
    size_t tunnel_secret_len;
    const uint8_t *tunnel_id; /* 16 bytes */
    uint8_t conn_index;
    const uint8_t *client_id; /* 16 bytes */
    const char *const *features;
    size_t num_features;
    const char *version;
    const char *arch;
    const uint8_t *origin_local_ip; /* 4 or 16 bytes, may be NULL */
    size_t origin_local_ip_len;
    bool replace_existing;
    uint8_t num_previous_attempts;
} cft_rpc_register_params_t;

typedef enum {
    CFT_RPC_REGISTERED,       /* details valid */
    CFT_RPC_REGISTER_FAILED,  /* error valid */
    CFT_RPC_UNREGISTERED,
    CFT_RPC_PROTOCOL_ERROR,   /* reason valid; the stream should be closed */
} cft_rpc_event_type_t;

typedef struct {
    cft_rpc_event_type_t type;
    struct {
        uint8_t uuid[16];
        char location[32];
        bool remotely_managed;
    } details;
    struct {
        char cause[128];
        int64_t retry_after_ns;
        bool should_retry;
    } error;
    char reason[128];
} cft_rpc_event_t;

typedef int (*cft_rpc_send_fn)(void *arg, const uint8_t *data, size_t len);
typedef void (*cft_rpc_event_fn)(void *arg, const cft_rpc_event_t *ev);

typedef struct cft_rpc cft_rpc_t;

cft_rpc_t *cft_rpc_new(const cft_rpc_register_params_t *params, cft_rpc_send_fn send, cft_rpc_event_fn on_event,
                       void *arg);
void cft_rpc_free(cft_rpc_t *rpc);

/* Sends Bootstrap; registration continues as replies arrive. */
int cft_rpc_start(cft_rpc_t *rpc);
/* Processes bytes received on the control stream. Returns <0 on a fatal
 * protocol error (an event is also emitted). */
int cft_rpc_feed(cft_rpc_t *rpc, const uint8_t *data, size_t len);
/* Sends unregisterConnection. Only valid after CFT_RPC_REGISTERED. */
int cft_rpc_unregister(cft_rpc_t *rpc);
