/**
 * @file cf_tunnel_esp.h
 * @brief ESP-IDF integration: runs a tunnel in a FreeRTOS task over mbedTLS.
 *
 * Requires CONFIG_MBEDTLS_SSL_PROTO_TLS1_3 (the edge refuses TLS 1.2).
 */
#pragma once

#include "cf_tunnel.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Transport over mbedTLS and lwIP sockets, used by cft_tunnel_start() */
extern const cft_transport_ops_t cft_transport_esp;

/** @brief Handle of a tunnel started by cft_tunnel_start() */
typedef struct cft_tunnel_esp *cft_tunnel_handle_t;

/**
 * @brief Creates a tunnel and runs it in a new task
 *
 * Defaults that differ from cft_tunnel_new():
 * - connector_id NULL: derived from the base MAC address and the tunnel ID,
 *   so the device keeps its connector ID across reboots;
 * - arch NULL: CONFIG_IDF_TARGET.
 *
 * Task stack size and priority come from Kconfig (CONFIG_CF_TUNNEL_TASK_*).
 * Request handlers run in this task.
 *
 * @param cfg  Configuration
 * @param[out] out  Handle of the running tunnel
 * @return ESP_OK, ESP_ERR_INVALID_ARG (e.g. invalid token) or ESP_ERR_NO_MEM
 */
esp_err_t cft_tunnel_start(const cft_tunnel_config_t *cfg, cft_tunnel_handle_t *out);

/**
 * @brief Unregisters from the edge and stops the task
 *
 * @param tunnel  Handle from cft_tunnel_start()
 * @param timeout_ms  How long to wait for the connections to unregister
 * @return ESP_OK, or ESP_ERR_TIMEOUT if the task did not stop
 */
esp_err_t cft_tunnel_stop(cft_tunnel_handle_t tunnel, uint32_t timeout_ms);

/**
 * @brief The tunnel run by the task, e.g. for cft_tunnel_post()
 *
 * @param tunnel  Handle from cft_tunnel_start()
 * @return The tunnel
 */
cft_tunnel_t *cft_tunnel_get(cft_tunnel_handle_t tunnel);

#ifdef __cplusplus
}
#endif
