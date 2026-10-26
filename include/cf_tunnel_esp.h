/*
 * ESP-IDF runner: keeps one tunnel connection to the Cloudflare edge alive
 * in a FreeRTOS task, reconnecting with backoff.
 *
 * Requires CONFIG_MBEDTLS_SSL_PROTO_TLS1_3 (the edge refuses TLS 1.2).
 */
#pragma once

#include "cf_tunnel.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *token;               /* tunnel token; copied */
    cft_handler_fn handler;          /* called from the tunnel task */
    void *handler_arg;
    cft_event_fn on_event;           /* optional, called from the tunnel task */
    void *event_arg;
    const char *version;             /* reported to the edge; NULL = component default */
    size_t max_request_body;         /* 0 = 16 KiB */
    uint32_t max_concurrent_streams; /* requests in flight; more get 503. 0 = 16 */
    uint32_t task_stack_size;        /* 0 = 8 KiB */
    unsigned task_priority;          /* 0 = 5 */
} cft_tunnel_config_t;

typedef struct cft_tunnel *cft_tunnel_handle_t;

esp_err_t cft_tunnel_start(const cft_tunnel_config_t *cfg, cft_tunnel_handle_t *out);

/* Unregisters from the edge (waiting up to timeout_ms) and stops the task. */
esp_err_t cft_tunnel_stop(cft_tunnel_handle_t tunnel, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
