/*
 * ESP-IDF integration: runs a cft_tunnel_t in a FreeRTOS task over esp-tls.
 *
 * Requires CONFIG_MBEDTLS_SSL_PROTO_TLS1_3 (the edge refuses TLS 1.2).
 */
#pragma once

#include "cf_tunnel.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* esp-tls based transport, used by cft_tunnel_start() */
extern const cft_transport_ops_t cft_transport_esp_tls;

typedef struct cft_tunnel_esp *cft_tunnel_handle_t;

/*
 * Starts the tunnel task. Defaults that differ from cft_tunnel_new():
 * - connector_id NULL: derived from the base MAC address and the tunnel ID,
 *   so the device keeps its connector ID across reboots;
 * - arch NULL: CONFIG_IDF_TARGET.
 * Task stack size and priority come from Kconfig (CF_TUNNEL_TASK_*).
 */
esp_err_t cft_tunnel_start(const cft_tunnel_config_t *cfg, cft_tunnel_handle_t *out);

/* Unregisters from the edge (waiting up to timeout_ms) and stops the task. */
esp_err_t cft_tunnel_stop(cft_tunnel_handle_t tunnel, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
