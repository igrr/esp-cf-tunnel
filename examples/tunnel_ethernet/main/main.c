/*
 * Brings up Ethernet, then serves a status page through a Cloudflare Tunnel.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cf_tunnel_esp.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_dotenv.h"

static const char *TAG = "example";

static EventGroupHandle_t s_events;
#define GOT_IP_BIT BIT0

static uint32_t s_requests;
static UBaseType_t s_tunnel_stack_free = UINT32_MAX;

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *ev = data;
    ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&ev->ip_info.ip));
    xEventGroupSetBits(s_events, GOT_IP_BIT);
}

static void ethernet_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CONFIG_EXAMPLE_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_EXAMPLE_ETH_PHY_RST_GPIO;
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = CONFIG_EXAMPLE_ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = CONFIG_EXAMPLE_ETH_MDIO_GPIO;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth));
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL));
    ESP_ERROR_CHECK(esp_eth_start(eth));
}

static const char *header_value(const cft_request_t *req, const char *name)
{
    for (size_t i = 0; i < req->num_headers; i++) {
        if (strcmp(req->headers[i].name, name) == 0) {
            return req->headers[i].value;
        }
    }
    return "";
}

/* Compares the path part of a request target (before '?') with `path`. */
static bool path_is(const cft_request_t *req, const char *path)
{
    size_t n = strcspn(req->path, "?");
    return n == strlen(path) && strncmp(req->path, path, n) == 0;
}

/* Runs in the tunnel task. */
static void handler(void *arg, const cft_request_t *req, cft_response_t *resp)
{
    s_requests++;
    UBaseType_t stack_free = uxTaskGetStackHighWaterMark(NULL);
    if (stack_free < s_tunnel_stack_free) {
        s_tunnel_stack_free = stack_free;
    }
    char body[1024];
    int n;
    if (path_is(req, "/api/status")) {
        n = snprintf(body, sizeof(body),
                     "{\"uptime_s\":%lld,\"requests\":%" PRIu32 ",\"heap_free\":%u,\"heap_min_free\":%u,"
                     "\"internal_free\":%u,\"internal_min_free\":%u,\"tunnel_stack_free_min\":%u}\n",
                     esp_timer_get_time() / 1000000, s_requests, (unsigned)esp_get_free_heap_size(),
                     (unsigned)esp_get_minimum_free_heap_size(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)s_tunnel_stack_free);
        cft_response_add_header(resp, "Content-Type", "application/json");
    } else if (path_is(req, "/")) {
        n = snprintf(body, sizeof(body),
                     "<!doctype html><html><head><meta charset=\"utf-8\"><title>%s</title></head><body>"
                     "<h1>Hello from %s</h1>"
                     "<p>Served through a Cloudflare Tunnel by the device itself.</p>"
                     "<ul><li>Uptime: %lld s</li><li>Requests served: %" PRIu32 "</li>"
                     "<li>Free heap: %u bytes (minimum %u)</li><li>Your IP: %s (%s)</li></ul>"
                     "<p><a href=\"/api/status\">/api/status</a></p></body></html>\n",
                     CONFIG_IDF_TARGET, CONFIG_IDF_TARGET, esp_timer_get_time() / 1000000, s_requests,
                     (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
                     header_value(req, "cf-connecting-ip"), header_value(req, "cf-ipcountry"));
        cft_response_add_header(resp, "Content-Type", "text/html; charset=utf-8");
    } else {
        cft_response_set_status(resp, 404);
        n = snprintf(body, sizeof(body), "not found\n");
        cft_response_add_header(resp, "Content-Type", "text/plain");
    }
    if (n >= (int)sizeof(body)) {
        n = sizeof(body) - 1;
    }
    cft_response_add_header(resp, "Cache-Control", "no-store");
    cft_response_set_body(resp, body, (size_t)n);
}

void app_main(void)
{
    /* Variables from the project's .env file, flashed to the "dotenv" partition */
    ESP_ERROR_CHECK(nvs_dotenv_load());
    const char *token = getenv("TUNNEL_TOKEN");
    if (!token || !*token) {
        ESP_LOGE(TAG, "TUNNEL_TOKEN is not set; add it to .env (see .env.example) and flash again");
        return;
    }
#ifdef CONFIG_EXAMPLE_DEBUG_TUNNEL
    esp_log_level_set("cft_conn", ESP_LOG_DEBUG);
    esp_log_level_set("cft_rpc", ESP_LOG_DEBUG);
#endif
    s_events = xEventGroupCreate();
    ethernet_start();
    xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    ESP_LOGI(TAG, "heap before tunnel start: internal free %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cft_tunnel_config_t cfg = {
        .token = token,
        .handler = handler,
    };
    cft_tunnel_handle_t tunnel;
    ESP_ERROR_CHECK(cft_tunnel_start(&cfg, &tunnel));

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "heap: free %u (min %u), internal free %u (min %u), largest internal block %u",
                 (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
}
