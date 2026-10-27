/*
 * Device test app: the test handlers (test_apps/common) over Ethernet, for
 * the stress test (test_apps/stress). Built with heap poisoning and stack
 * checks; /stats reports heap figures and checks heap integrity, and
 * /chaos/netdown?ms=N stops the Ethernet driver for N ms.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cf_tunnel_esp.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_heap_task_info.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_dotenv.h"
#include "test_handlers.h"

static const char *TAG = "device_test";

#ifdef CFT_DEBUG_STATS
int cft_debug_stats(char *buf, size_t size); /* esp-cf-tunnel src/cft_debug.c */
#endif

static EventGroupHandle_t s_events;
#define GOT_IP_BIT BIT0

static esp_eth_handle_t s_eth;
static volatile bool s_netdown_active;
static volatile uint32_t s_netdowns;
static volatile bool s_heap_corrupt;

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
    phy_config.phy_addr = CONFIG_TEST_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_TEST_ETH_PHY_RST_GPIO;
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = CONFIG_TEST_ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = CONFIG_TEST_ETH_MDIO_GPIO;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &s_eth));
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(s_eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL));
    ESP_ERROR_CHECK(esp_eth_start(s_eth));
}

static bool heap_ok(void)
{
    bool ok = heap_caps_check_integrity_all(true);
    if (!ok) {
        s_heap_corrupt = true;
    }
    return ok && !s_heap_corrupt;
}

/* Runs on the tunnel task, so the stack figure is the tunnel task's */
static int platform_stats(char *buf, size_t size)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_DEFAULT);
    return snprintf(buf, size,
                    ",\"uptime_s\":%lld,\"heap_free\":%u,\"heap_min_free\":%u,\"largest_block\":%u,"
                    "\"alloc_blocks\":%u,\"heap_ok\":%s,\"tunnel_stack_free\":%u,\"netdowns\":%" PRIu32,
                    esp_timer_get_time() / 1000000, (unsigned)info.total_free_bytes,
                    (unsigned)info.minimum_free_bytes, (unsigned)info.largest_free_block,
                    (unsigned)info.allocated_blocks, heap_ok() ? "true" : "false",
                    (unsigned)uxTaskGetStackHighWaterMark(NULL), s_netdowns);
}

static void netdown_task(void *arg)
{
    uint32_t ms = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(1000)); /* let the response go out */
    ESP_LOGW(TAG, "chaos: Ethernet down for %" PRIu32 " ms", ms);
    esp_eth_stop(s_eth);
    vTaskDelay(pdMS_TO_TICKS(ms));
    esp_eth_start(s_eth);
    ESP_LOGW(TAG, "chaos: Ethernet up");
    s_netdowns++;
    s_netdown_active = false;
    vTaskDelete(NULL);
}

static void chaos(cft_stream_t *s, const cft_request_t *req)
{
    if (strncmp(req->path, "/chaos/netdown", 14) == 0) {
        const char *q = strstr(req->path, "ms=");
        uint32_t ms = q ? (uint32_t)strtoul(q + 3, NULL, 10) : 10000;
        if (s_netdown_active) {
            cft_stream_respond(s, 409, "text/plain", "busy\n", 5);
            return;
        }
        s_netdown_active = true;
        if (xTaskCreate(netdown_task, "netdown", 3072, (void *)(uintptr_t)ms, 5, NULL) != pdPASS) {
            s_netdown_active = false;
            cft_stream_respond(s, 500, NULL, NULL, 0);
            return;
        }
        cft_stream_respond(s, 200, "text/plain", "ok\n", 3);
    } else {
        cft_stream_respond(s, 404, NULL, NULL, 0);
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_dotenv_load());
    const char *token = getenv("TUNNEL_TOKEN");
    if (!token || !*token) {
        ESP_LOGE(TAG, "TUNNEL_TOKEN is not set; add it to .env and flash again");
        return;
    }
    s_events = xEventGroupCreate();
    ethernet_start();
    xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    th_config_t th = {
        .lan_http = getenv("LAN_HTTP"),
        .lan_tcp = getenv("LAN_TCP"),
        .lan_dead = getenv("LAN_DEAD"),
        .poster_threads = CONFIG_TEST_POSTER_THREADS,
        .platform_stats = platform_stats,
        .chaos = chaos,
    };
    th_init(&th);
    cft_tunnel_config_t cfg = {
        .token = token,
        .on_request = th_on_request,
        .on_event = th_on_event,
        .num_connections = CONFIG_TEST_NUM_CONNECTIONS,
        .max_concurrent_streams = CONFIG_TEST_MAX_STREAMS,
        .stream_window = CONFIG_TEST_STREAM_WINDOW_KB * 1024,
        .max_origin_connections = 4,
    };
    cft_tunnel_handle_t tunnel;
    ESP_ERROR_CHECK(cft_tunnel_start(&cfg, &tunnel));
    ESP_ERROR_CHECK(th_start(cft_tunnel_get(tunnel)) == 0 ? ESP_OK : ESP_FAIL);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        multi_heap_info_t info;
        heap_caps_get_info(&info, MALLOC_CAP_DEFAULT);
        ESP_LOGI(TAG, "heap: free %u min %u largest %u blocks %u integrity %s",
                 (unsigned)info.total_free_bytes, (unsigned)info.minimum_free_bytes,
                 (unsigned)info.largest_free_block, (unsigned)info.allocated_blocks,
                 heap_ok() ? "ok" : "CORRUPT");
#ifdef CFT_DEBUG_STATS
        char dbg[256];
        cft_debug_stats(dbg, sizeof(dbg)); /* racy read of counters; fine for a log */
        ESP_LOGI(TAG, "tunnel%s", dbg);
#endif
#if defined(CONFIG_HEAP_TASK_TRACKING) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
        heap_caps_print_all_task_stat_overview(stdout);
#endif
    }
}
