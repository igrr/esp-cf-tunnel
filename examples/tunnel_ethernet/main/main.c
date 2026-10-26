/*
 * Brings up Ethernet, then serves a status page through a Cloudflare Tunnel.
 * The page opens a WebSocket to the device, which pushes the heap usage every
 * second.
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

static cft_tunnel_handle_t s_tunnel;
static uint32_t s_requests;
static UBaseType_t s_tunnel_stack_free = UINT32_MAX;

/* Open WebSockets; only touched on the tunnel task */
#define MAX_WS 4
static cft_stream_t *s_ws[MAX_WS];

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

/* Compares the path part of a request target (before '?') with `path`. */
static bool path_is(const cft_request_t *req, const char *path)
{
    size_t n = strcspn(req->path, "?");
    return n == strlen(path) && strncmp(req->path, path, n) == 0;
}

static int status_json(char *buf, size_t size)
{
    return snprintf(buf, size,
                    "{\"uptime_s\":%lld,\"requests\":%" PRIu32 ",\"heap_free\":%u,\"heap_min_free\":%u,"
                    "\"internal_free\":%u,\"internal_min_free\":%u,\"tunnel_stack_free_min\":%u}",
                    esp_timer_get_time() / 1000000, s_requests, (unsigned)esp_get_free_heap_size(),
                    (unsigned)esp_get_minimum_free_heap_size(),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                    (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                    (unsigned)s_tunnel_stack_free);
}

static const char index_html[] =
    "<!doctype html><html><head><meta charset=\"utf-8\"><title>" CONFIG_IDF_TARGET "</title></head><body>"
    "<h1>Hello from " CONFIG_IDF_TARGET "</h1>"
    "<p>Served through a Cloudflare Tunnel by the device itself.</p>"
    "<p>Live status over a WebSocket: <code id=\"s\">connecting...</code></p>"
    "<p><a href=\"/api/status\">/api/status</a></p>"
    "<script>"
    "const ws = new WebSocket((location.protocol == 'https:' ? 'wss://' : 'ws://') + location.host + '/ws');"
    "ws.onmessage = e => document.getElementById('s').textContent = e.data;"
    "ws.onclose = () => document.getElementById('s').textContent += ' (closed)';"
    "</script></body></html>\n";

/* --- WebSocket: echoes messages, and gets the status every second --- */

static void ws_message(cft_stream_t *s, void *ctx, cft_ws_opcode_t op, const uint8_t *data, size_t len)
{
    cft_ws_send(s, op, data, len);
}

static void ws_close(cft_stream_t *s, void *ctx, bool completed)
{
    for (int i = 0; i < MAX_WS; i++) {
        if (s_ws[i] == s) {
            s_ws[i] = NULL;
        }
    }
}

static void ws_open(cft_stream_t *s)
{
    for (int i = 0; i < MAX_WS; i++) {
        if (!s_ws[i]) {
            cft_stream_handlers_t h = {.on_ws_message = ws_message, .on_close = ws_close};
            cft_stream_set_handlers(s, &h, NULL);
            if (cft_ws_accept(s, NULL) == 0) {
                s_ws[i] = s;
            }
            return;
        }
    }
    cft_stream_respond(s, 503, "text/plain", "too many WebSockets\n", 20);
}

/* Posted from app_main to the tunnel task */
static void ws_push_status(void *arg)
{
    char json[256];
    int n = status_json(json, sizeof(json));
    for (int i = 0; i < MAX_WS; i++) {
        if (s_ws[i]) {
            cft_ws_send(s_ws[i], CFT_WS_TEXT, json, (size_t)n);
        }
    }
}

/* Runs in the tunnel task when request headers arrive */
static void on_request(void *arg, cft_stream_t *s, const cft_request_t *req)
{
    s_requests++;
    UBaseType_t stack_free = uxTaskGetStackHighWaterMark(NULL);
    if (stack_free < s_tunnel_stack_free) {
        s_tunnel_stack_free = stack_free;
    }
    if (req->is_websocket && path_is(req, "/ws")) {
        ws_open(s);
    } else if (path_is(req, "/api/status")) {
        char json[256];
        int n = status_json(json, sizeof(json));
        cft_stream_respond(s, 200, "application/json", json, (size_t)n);
    } else if (path_is(req, "/")) {
        cft_stream_respond(s, 200, "text/html; charset=utf-8", index_html, sizeof(index_html) - 1);
    } else {
        cft_stream_respond(s, 404, "text/plain", "not found\n", 10);
    }
}

void app_main(void)
{
#ifdef CONFIG_EXAMPLE_DEBUG_TUNNEL
    esp_log_level_set("cft_conn", ESP_LOG_DEBUG);
    esp_log_level_set("cft_rpc", ESP_LOG_DEBUG);
    esp_log_level_set("cft_proxy", ESP_LOG_DEBUG);
#endif
    /* Variables from the project's .env file, flashed to the "dotenv" partition */
    ESP_ERROR_CHECK(nvs_dotenv_load());
    const char *token = getenv("TUNNEL_TOKEN");
    if (!token || !*token) {
        ESP_LOGE(TAG, "TUNNEL_TOKEN is not set; add it to .env (see .env.example) and flash again");
        return;
    }
    s_events = xEventGroupCreate();
    ethernet_start();
    xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    ESP_LOGI(TAG, "heap before tunnel start: internal free %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cft_tunnel_config_t cfg = {
        .token = token,
        .on_request = on_request,
#ifdef CONFIG_EXAMPLE_USE_INGRESS_RULES
        .use_ingress_rules = true,
#endif
    };
    ESP_ERROR_CHECK(cft_tunnel_start(&cfg, &s_tunnel));

    for (int i = 0;; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        cft_tunnel_post(cft_tunnel_get(s_tunnel), ws_push_status, NULL);
        if (i % 30 == 0) {
            ESP_LOGI(TAG, "heap: free %u (min %u), internal free %u (min %u), largest internal block %u",
                     (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        }
    }
}
