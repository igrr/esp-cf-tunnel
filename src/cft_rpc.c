#include "cft_rpc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cft_capnp.h"
#include "cft_port.h"

static const char *TAG = "cft_rpc";

/*
 * Layouts from rpc.capnp and tunnelrpc.capnp (see tools/capnp_layout.py).
 * D = data words, P = pointer words; data offsets are in bits.
 */
/* Message: D1 P1, union tag @0 */
#define MSG_D 1
#define MSG_P 1
#define MSG_UNIMPLEMENTED 0
#define MSG_ABORT 1
#define MSG_CALL 2
#define MSG_RETURN 3
#define MSG_FINISH 4
#define MSG_RESOLVE 5
#define MSG_RELEASE 6
#define MSG_BOOTSTRAP 8
#define MSG_DISEMBARGO 13
/* Bootstrap: D1 P1, questionId u32 @0 */
/* Call: D3 P3, questionId u32 @0, methodId u16 @32, interfaceId u64 @64,
 * target ptr0, params ptr1, sendResultsTo tag u16 @48 (0 = caller) */
/* MessageTarget: D1 P1, importedCap u32 @0, tag u16 @32 (0 = importedCap) */
/* Payload: D0 P2, content ptr0, capTable ptr1 */
/* Return: D2 P1, answerId u32 @0, releaseParamCaps bool @32 (default true),
 * tag u16 @48 (0 results, 1 exception, 2 canceled), results/exception ptr0 */
#define RET_RESULTS 0
#define RET_EXCEPTION 1
/* Finish: D1 P0, questionId u32 @0, releaseResultCaps bool @32 (default true) */
/* Release: D1 P0, id u32 @0, referenceCount u32 @32 */
/* Exception: D1 P1, reason ptr0, type u16 @32 */
/* CapDescriptor: D1 P1, tag u16 @0 (1 senderHosted, 2 senderPromise), id u32 @32 */

#define REGISTRATION_SERVER_ID 0xf71695ec7fe85497ULL
#define METHOD_REGISTER_CONNECTION 0
#define METHOD_UNREGISTER_CONNECTION 1

/* Accumulated incoming bytes are capped; replies we expect are small. */
#define RX_MAX (64 * 1024)
#define TX_BUF 1024

typedef enum {
    ST_IDLE,
    ST_BOOTSTRAP,
    ST_REGISTERING,
    ST_REGISTERED,
    ST_UNREGISTERING,
    ST_DONE,
    ST_FAILED,
} rpc_state_t;

struct cft_rpc {
    cft_rpc_register_params_t params;
    cft_rpc_send_fn send;
    cft_rpc_event_fn on_event;
    void *arg;

    rpc_state_t state;
    uint32_t next_qid;
    uint32_t q_bootstrap;
    uint32_t q_register;
    uint32_t q_unregister;
    uint32_t import_id;
    bool have_import;

    uint8_t *rx;
    size_t rx_len;
    size_t rx_cap;
};

static void emit_protocol_error(cft_rpc_t *rpc, const char *fmt, const char *detail)
{
    cft_rpc_event_t ev = {.type = CFT_RPC_PROTOCOL_ERROR};
    snprintf(ev.reason, sizeof(ev.reason), fmt, detail ? detail : "");
    CFT_LOGE(TAG, "%s", ev.reason);
    rpc->state = ST_FAILED;
    rpc->on_event(rpc->arg, &ev);
}

static int send_frame(cft_rpc_t *rpc, cft_cpb_t *b)
{
    size_t n = cft_cpb_finish(b);
    if (n == 0) {
        CFT_LOGE(TAG, "message does not fit into %d bytes", TX_BUF);
        return -1;
    }
    return rpc->send(rpc->arg, b->buf, n);
}

static int send_bootstrap(cft_rpc_t *rpc, uint32_t qid)
{
    uint8_t buf[64];
    cft_cpb_t b;
    cft_cpb_init(&b, buf, sizeof(buf));
    cft_cpb_struct_t msg = cft_cpb_root(&b, MSG_D, MSG_P);
    cft_cpb_set_u16(&b, msg, 0, MSG_BOOTSTRAP);
    cft_cpb_struct_t bs = cft_cpb_new_struct(&b, msg, 0, 1, 1);
    cft_cpb_set_u32(&b, bs, 0, qid);
    return send_frame(rpc, &b);
}

static int send_finish(cft_rpc_t *rpc, uint32_t qid, bool release_result_caps)
{
    uint8_t buf[64];
    cft_cpb_t b;
    cft_cpb_init(&b, buf, sizeof(buf));
    cft_cpb_struct_t msg = cft_cpb_root(&b, MSG_D, MSG_P);
    cft_cpb_set_u16(&b, msg, 0, MSG_FINISH);
    cft_cpb_struct_t fin = cft_cpb_new_struct(&b, msg, 0, 1, 0);
    cft_cpb_set_u32(&b, fin, 0, qid);
    /* stored XOR the default (true) */
    cft_cpb_set_bool(&b, fin, 32, !release_result_caps);
    return send_frame(rpc, &b);
}

static int send_release(cft_rpc_t *rpc, uint32_t id, uint32_t count)
{
    uint8_t buf[64];
    cft_cpb_t b;
    cft_cpb_init(&b, buf, sizeof(buf));
    cft_cpb_struct_t msg = cft_cpb_root(&b, MSG_D, MSG_P);
    cft_cpb_set_u16(&b, msg, 0, MSG_RELEASE);
    cft_cpb_struct_t rel = cft_cpb_new_struct(&b, msg, 0, 1, 0);
    cft_cpb_set_u32(&b, rel, 0, id);
    cft_cpb_set_u32(&b, rel, 32, count);
    return send_frame(rpc, &b);
}

/* Answers a Bootstrap or Call from the peer: we export nothing. */
static int send_exception_return(cft_rpc_t *rpc, uint32_t answer_id, const char *reason)
{
    uint8_t buf[256];
    cft_cpb_t b;
    cft_cpb_init(&b, buf, sizeof(buf));
    cft_cpb_struct_t msg = cft_cpb_root(&b, MSG_D, MSG_P);
    cft_cpb_set_u16(&b, msg, 0, MSG_RETURN);
    cft_cpb_struct_t ret = cft_cpb_new_struct(&b, msg, 0, 2, 1);
    cft_cpb_set_u32(&b, ret, 0, answer_id);
    cft_cpb_set_u16(&b, ret, 48, RET_EXCEPTION);
    cft_cpb_struct_t ex = cft_cpb_new_struct(&b, ret, 0, 1, 1);
    cft_cpb_set_text(&b, ex, 0, reason);
    cft_cpb_set_u16(&b, ex, 32, 0 /* failed */);
    return send_frame(rpc, &b);
}

/* Starts a Call to the bootstrap capability; returns the params struct. */
static cft_cpb_struct_t begin_call(cft_rpc_t *rpc, cft_cpb_t *b, uint32_t qid, uint16_t method, uint16_t pdw,
                                   uint16_t ppw)
{
    cft_cpb_struct_t msg = cft_cpb_root(b, MSG_D, MSG_P);
    cft_cpb_set_u16(b, msg, 0, MSG_CALL);
    cft_cpb_struct_t call = cft_cpb_new_struct(b, msg, 0, 3, 3);
    cft_cpb_set_u32(b, call, 0, qid);
    cft_cpb_set_u16(b, call, 32, method);
    cft_cpb_set_u64(b, call, 64, REGISTRATION_SERVER_ID);
    cft_cpb_struct_t target = cft_cpb_new_struct(b, call, 0, 1, 1);
    cft_cpb_set_u32(b, target, 0, rpc->import_id);
    cft_cpb_set_u16(b, target, 32, 0 /* importedCap */);
    cft_cpb_struct_t payload = cft_cpb_new_struct(b, call, 1, 0, 2);
    return cft_cpb_new_struct(b, payload, 0, pdw, ppw);
}

static int send_register(cft_rpc_t *rpc)
{
    const cft_rpc_register_params_t *p = &rpc->params;
    uint8_t buf[TX_BUF];
    cft_cpb_t b;
    cft_cpb_init(&b, buf, sizeof(buf));
    rpc->q_register = rpc->next_qid++;
    /* registerConnection$Params: D1 P3, auth ptr0, tunnelId ptr1,
     * connIndex u8 @0, options ptr2 */
    cft_cpb_struct_t params = begin_call(rpc, &b, rpc->q_register, METHOD_REGISTER_CONNECTION, 1, 3);
    /* TunnelAuth: D0 P2, accountTag ptr0, tunnelSecret ptr1 */
    cft_cpb_struct_t auth = cft_cpb_new_struct(&b, params, 0, 0, 2);
    cft_cpb_set_text(&b, auth, 0, p->account_tag);
    cft_cpb_set_data(&b, auth, 1, p->tunnel_secret, p->tunnel_secret_len);
    cft_cpb_set_data(&b, params, 1, p->tunnel_id, 16);
    cft_cpb_set_u8(&b, params, 0, p->conn_index);
    /* ConnectionOptions: D1 P2, client ptr0, originLocalIp ptr1,
     * replaceExisting bool @0, compressionQuality u8 @8,
     * numPreviousAttempts u8 @16 */
    cft_cpb_struct_t opts = cft_cpb_new_struct(&b, params, 2, 1, 2);
    /* ClientInfo: D0 P4, clientId, features, version, arch */
    cft_cpb_struct_t client = cft_cpb_new_struct(&b, opts, 0, 0, 4);
    cft_cpb_set_data(&b, client, 0, p->client_id, 16);
    cft_cpb_set_text_list(&b, client, 1, p->features, p->num_features);
    cft_cpb_set_text(&b, client, 2, p->version);
    cft_cpb_set_text(&b, client, 3, p->arch);
    if (p->origin_local_ip && p->origin_local_ip_len) {
        cft_cpb_set_data(&b, opts, 1, p->origin_local_ip, p->origin_local_ip_len);
    }
    cft_cpb_set_bool(&b, opts, 0, p->replace_existing);
    cft_cpb_set_u8(&b, opts, 16, p->num_previous_attempts);
    return send_frame(rpc, &b);
}

int cft_rpc_unregister(cft_rpc_t *rpc)
{
    if (rpc->state != ST_REGISTERED) {
        return -1;
    }
    uint8_t buf[128];
    cft_cpb_t b;
    cft_cpb_init(&b, buf, sizeof(buf));
    rpc->q_unregister = rpc->next_qid++;
    begin_call(rpc, &b, rpc->q_unregister, METHOD_UNREGISTER_CONNECTION, 0, 0);
    rpc->state = ST_UNREGISTERING;
    return send_frame(rpc, &b);
}

static void copy_text(cft_cpr_struct_t s, unsigned ptr, char *out, size_t out_size)
{
    const uint8_t *d;
    size_t n;
    out[0] = '\0';
    if (cft_cpr_blob(s, ptr, true, &d, &n)) {
        if (n >= out_size) {
            n = out_size - 1;
        }
        memcpy(out, d, n);
        out[n] = '\0';
    }
}

static void exception_reason(cft_cpr_struct_t ret, char *out, size_t out_size)
{
    copy_text(cft_cpr_struct(ret, 0), 0, out, out_size);
}

static int handle_bootstrap_return(cft_rpc_t *rpc, cft_cpr_struct_t ret)
{
    uint16_t which = cft_cpr_u16(ret, 48);
    if (which != RET_RESULTS) {
        char reason[96];
        exception_reason(ret, reason, sizeof(reason));
        emit_protocol_error(rpc, "bootstrap failed: %s", reason);
        return -1;
    }
    cft_cpr_struct_t payload = cft_cpr_struct(ret, 0);
    int32_t cap = cft_cpr_cap(payload, 0);
    cft_cpr_list_t table = cft_cpr_list(payload, 1);
    cft_cpr_struct_t desc = cft_cpr_list_struct(table, cap < 0 ? 0 : (uint32_t)cap);
    uint16_t kind = cft_cpr_u16(desc, 0);
    if (cap < 0 || (kind != 1 && kind != 2)) {
        emit_protocol_error(rpc, "bootstrap returned no usable capability%s", NULL);
        return -1;
    }
    rpc->import_id = cft_cpr_u32(desc, 32);
    rpc->have_import = true;
    /* Keep the capability: later calls (unregister) target it. */
    if (send_finish(rpc, rpc->q_bootstrap, false) < 0) {
        return -1;
    }
    rpc->state = ST_REGISTERING;
    return send_register(rpc);
}

static int handle_register_return(cft_rpc_t *rpc, cft_cpr_struct_t ret)
{
    send_finish(rpc, rpc->q_register, true);
    cft_rpc_event_t ev = {0};
    uint16_t which = cft_cpr_u16(ret, 48);
    if (which != RET_RESULTS) {
        ev.type = CFT_RPC_REGISTER_FAILED;
        exception_reason(ret, ev.error.cause, sizeof(ev.error.cause));
        ev.error.should_retry = true;
        rpc->state = ST_FAILED;
        rpc->on_event(rpc->arg, &ev);
        return 0;
    }
    /* registerConnection$Results: D0 P1, result ptr0 -> ConnectionResponse:
     * D1 P1, tag u16 @0 (0 error, 1 connectionDetails), ptr0 */
    cft_cpr_struct_t results = cft_cpr_struct(cft_cpr_struct(ret, 0), 0);
    cft_cpr_struct_t resp = cft_cpr_struct(results, 0);
    cft_cpr_struct_t inner = cft_cpr_struct(resp, 0);
    if (!resp.msg || !inner.msg) {
        emit_protocol_error(rpc, "malformed registerConnection result%s", NULL);
        return -1;
    }
    if (cft_cpr_u16(resp, 0) == 1) {
        /* ConnectionDetails: D1 P2, uuid ptr0, locationName ptr1,
         * tunnelIsRemotelyManaged bool @0 */
        ev.type = CFT_RPC_REGISTERED;
        const uint8_t *uuid;
        size_t n;
        if (cft_cpr_blob(inner, 0, false, &uuid, &n) && n == 16) {
            memcpy(ev.details.uuid, uuid, 16);
        }
        copy_text(inner, 1, ev.details.location, sizeof(ev.details.location));
        ev.details.remotely_managed = cft_cpr_bool(inner, 0);
        rpc->state = ST_REGISTERED;
    } else {
        /* ConnectionError: D2 P1, cause ptr0, retryAfter i64 @0,
         * shouldRetry bool @64 */
        ev.type = CFT_RPC_REGISTER_FAILED;
        copy_text(inner, 0, ev.error.cause, sizeof(ev.error.cause));
        ev.error.retry_after_ns = (int64_t)cft_cpr_u64(inner, 0);
        ev.error.should_retry = cft_cpr_bool(inner, 64);
        rpc->state = ST_FAILED;
    }
    rpc->on_event(rpc->arg, &ev);
    return 0;
}

static int handle_unregister_return(cft_rpc_t *rpc, cft_cpr_struct_t ret)
{
    send_finish(rpc, rpc->q_unregister, true);
    if (rpc->have_import) {
        send_release(rpc, rpc->import_id, 1);
        rpc->have_import = false;
    }
    if (cft_cpr_u16(ret, 48) != RET_RESULTS) {
        char reason[96];
        exception_reason(ret, reason, sizeof(reason));
        CFT_LOGW(TAG, "unregisterConnection failed: %s", reason);
    }
    rpc->state = ST_DONE;
    cft_rpc_event_t ev = {.type = CFT_RPC_UNREGISTERED};
    rpc->on_event(rpc->arg, &ev);
    return 0;
}

static int handle_message(cft_rpc_t *rpc, const cft_cpr_msg_t *m)
{
    cft_cpr_struct_t msg = cft_cpr_root(m);
    uint16_t which = cft_cpr_u16(msg, 0);
    cft_cpr_struct_t body = cft_cpr_struct(msg, 0);
    switch (which) {
    case MSG_RETURN: {
        uint32_t id = cft_cpr_u32(body, 0);
        if (rpc->state == ST_BOOTSTRAP && id == rpc->q_bootstrap) {
            return handle_bootstrap_return(rpc, body);
        }
        if (rpc->state == ST_REGISTERING && id == rpc->q_register) {
            return handle_register_return(rpc, body);
        }
        if (rpc->state == ST_UNREGISTERING && id == rpc->q_unregister) {
            return handle_unregister_return(rpc, body);
        }
        CFT_LOGW(TAG, "unexpected Return for question %u", (unsigned)id);
        return 0;
    }
    case MSG_BOOTSTRAP:
        CFT_LOGD(TAG, "peer requested our bootstrap interface; refusing");
        return send_exception_return(rpc, cft_cpr_u32(body, 0), "no bootstrap interface");
    case MSG_CALL:
        CFT_LOGD(TAG, "peer called method %u; refusing", cft_cpr_u16(body, 32));
        return send_exception_return(rpc, cft_cpr_u32(body, 0), "no capabilities exported");
    case MSG_ABORT: {
        char reason[96];
        copy_text(body, 0, reason, sizeof(reason));
        emit_protocol_error(rpc, "peer aborted: %s", reason);
        return -1;
    }
    case MSG_FINISH:
    case MSG_RELEASE:
    case MSG_RESOLVE:
    case MSG_UNIMPLEMENTED:
        return 0;
    default:
        CFT_LOGW(TAG, "ignoring RPC message type %u", which);
        return 0;
    }
}

cft_rpc_t *cft_rpc_new(const cft_rpc_register_params_t *params, cft_rpc_send_fn send, cft_rpc_event_fn on_event,
                       void *arg)
{
    cft_rpc_t *rpc = calloc(1, sizeof(*rpc));
    if (!rpc) {
        return NULL;
    }
    rpc->params = *params;
    rpc->send = send;
    rpc->on_event = on_event;
    rpc->arg = arg;
    return rpc;
}

void cft_rpc_free(cft_rpc_t *rpc)
{
    if (rpc) {
        free(rpc->rx);
        free(rpc);
    }
}

int cft_rpc_start(cft_rpc_t *rpc)
{
    if (rpc->state != ST_IDLE) {
        return -1;
    }
    rpc->q_bootstrap = rpc->next_qid++;
    rpc->state = ST_BOOTSTRAP;
    return send_bootstrap(rpc, rpc->q_bootstrap);
}

int cft_rpc_feed(cft_rpc_t *rpc, const uint8_t *data, size_t len)
{
    if (rpc->state == ST_FAILED) {
        return -1;
    }
    if (len > RX_MAX - rpc->rx_len) {
        emit_protocol_error(rpc, "RPC message too large%s", NULL);
        return -1;
    }
    if (rpc->rx_len + len > rpc->rx_cap) {
        size_t cap = rpc->rx_cap ? rpc->rx_cap : 512;
        while (cap < rpc->rx_len + len) {
            cap *= 2;
        }
        uint8_t *nb = realloc(rpc->rx, cap);
        if (!nb) {
            emit_protocol_error(rpc, "out of memory%s", NULL);
            return -1;
        }
        rpc->rx = nb;
        rpc->rx_cap = cap;
    }
    memcpy(rpc->rx + rpc->rx_len, data, len);
    rpc->rx_len += len;

    for (;;) {
        cft_cpr_msg_t m;
        int n = cft_cpr_parse_frame(rpc->rx, rpc->rx_len, &m);
        if (n == 0) {
            return 0;
        }
        if (n < 0) {
            emit_protocol_error(rpc, "malformed RPC frame%s", NULL);
            return -1;
        }
        int r = handle_message(rpc, &m);
        memmove(rpc->rx, rpc->rx + n, rpc->rx_len - (size_t)n);
        rpc->rx_len -= (size_t)n;
        if (r < 0) {
            return r;
        }
    }
}
