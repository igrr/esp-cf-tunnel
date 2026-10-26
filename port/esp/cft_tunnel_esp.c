#include "cf_tunnel_esp.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "cft_conn.h"

#if !CONFIG_MBEDTLS_SSL_PROTO_TLS1_3
#error "esp-cf-tunnel needs CONFIG_MBEDTLS_SSL_PROTO_TLS1_3: the tunnel edge only accepts TLS 1.3"
#endif

static const char *TAG = "cf_tunnel";

struct cft_tunnel_esp {
    cft_tunnel_t *tunnel;
    SemaphoreHandle_t done;
};

static void tunnel_task(void *arg)
{
    struct cft_tunnel_esp *h = arg;
    cft_tunnel_run(h->tunnel);
    xSemaphoreGive(h->done);
    vTaskDelete(NULL);
}

/* Stable per device and tunnel: the tunnel ID with the base MAC mixed in,
 * formatted as a UUID. */
static void derive_connector_id(const char *token, uint8_t out[16])
{
    cft_token_t t;
    uint8_t mac[6] = {0};
    memset(out, 0, 16);
    if (cft_token_parse(token, &t)) {
        memcpy(out, t.tunnel_id, 16);
        memset(&t, 0, sizeof(t));
    }
    esp_read_mac(mac, ESP_MAC_BASE);
    for (int i = 0; i < 6; i++) {
        out[10 + i] ^= mac[i];
    }
    out[6] = (out[6] & 0x0f) | 0x80; /* UUID v8: custom */
    out[8] = (out[8] & 0x3f) | 0x80;
}

esp_err_t cft_tunnel_start(const cft_tunnel_config_t *cfg, cft_tunnel_handle_t *out)
{
    if (!cfg || !cfg->token || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    struct cft_tunnel_esp *h = calloc(1, sizeof(*h));
    if (!h) {
        return ESP_ERR_NO_MEM;
    }
    cft_tunnel_config_t c = *cfg;
    uint8_t connector_id[16];
    if (!c.connector_id) {
        derive_connector_id(cfg->token, connector_id);
        c.connector_id = connector_id;
    }
    if (!c.arch) {
        c.arch = CONFIG_IDF_TARGET;
    }
    h->tunnel = cft_tunnel_new(&c, &cft_transport_esp_tls);
    if (!h->tunnel) {
        ESP_LOGE(TAG, "invalid tunnel token or out of memory");
        free(h);
        return ESP_ERR_INVALID_ARG;
    }
    h->done = xSemaphoreCreateBinary();
    if (!h->done || xTaskCreate(tunnel_task, "cf_tunnel", CONFIG_CF_TUNNEL_TASK_STACK_SIZE, h,
                                CONFIG_CF_TUNNEL_TASK_PRIORITY, NULL) != pdPASS) {
        if (h->done) {
            vSemaphoreDelete(h->done);
        }
        cft_tunnel_free(h->tunnel);
        free(h);
        return ESP_ERR_NO_MEM;
    }
    *out = h;
    return ESP_OK;
}

esp_err_t cft_tunnel_stop(cft_tunnel_handle_t h, uint32_t timeout_ms)
{
    if (!h) {
        return ESP_ERR_INVALID_ARG;
    }
    cft_tunnel_request_stop(h->tunnel, timeout_ms);
    /* The loop wakes at least every 250 ms; a connect may take a while longer */
    if (xSemaphoreTake(h->done, pdMS_TO_TICKS(timeout_ms + 20000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    vSemaphoreDelete(h->done);
    cft_tunnel_free(h->tunnel);
    free(h);
    return ESP_OK;
}

cft_tunnel_t *cft_tunnel_get(cft_tunnel_handle_t h)
{
    return h ? h->tunnel : NULL;
}
