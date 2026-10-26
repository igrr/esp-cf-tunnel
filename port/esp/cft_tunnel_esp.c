#include "cf_tunnel_esp.h"

#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if !CONFIG_MBEDTLS_SSL_PROTO_TLS1_3
#error "cf_tunnel needs CONFIG_MBEDTLS_SSL_PROTO_TLS1_3: the tunnel edge only accepts TLS 1.3"
#endif

static const char *TAG = "cf_tunnel";

#define DEFAULT_VERSION "cf_tunnel-esp/0.1.0"
#define RX_BUF_SIZE 4096
#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 60000
#define UNREGISTER_WAIT_MS 5000

struct cft_tunnel {
    cft_tunnel_config_t cfg;
    cft_token_t token;
    uint8_t client_id[16];
    TaskHandle_t task;
    SemaphoreHandle_t done;
    volatile bool stop;
    uint32_t stop_timeout_ms;

    /* State of the current connection, updated from events. */
    bool registered;
    bool closed;
    int64_t retry_after_ms;
};

static void on_conn_event(void *arg, const cft_event_t *ev)
{
    struct cft_tunnel *t = arg;
    switch (ev->type) {
    case CFT_EVENT_REGISTERED:
        t->registered = true;
        ESP_LOGI(TAG, "connected: connection %s at %s", ev->registered.connection_id, ev->registered.location);
        break;
    case CFT_EVENT_REGISTRATION_FAILED:
        ESP_LOGW(TAG, "registration refused: %s (retry %s, after %lld ms)", ev->failed.cause,
                 ev->failed.should_retry ? "yes" : "no", (long long)ev->failed.retry_after_ms);
        t->retry_after_ms = ev->failed.retry_after_ms;
        break;
    case CFT_EVENT_CONFIG_UPDATED:
        ESP_LOGI(TAG, "remote configuration version %d", (int)ev->config.version);
        break;
    case CFT_EVENT_UNREGISTERED:
        ESP_LOGI(TAG, "unregistered");
        break;
    case CFT_EVENT_CLOSED:
        t->closed = true;
        break;
    }
    if (t->cfg.on_event) {
        t->cfg.on_event(t->cfg.event_arg, ev);
    }
}

static int flush_out(cft_conn_t *conn, esp_tls_t *tls)
{
    for (;;) {
        const uint8_t *data;
        long n = cft_conn_send_data(conn, &data);
        if (n <= 0) {
            return (int)n;
        }
        size_t written = 0;
        while (written < (size_t)n) {
            ssize_t w = esp_tls_conn_write(tls, data + written, (size_t)n - written);
            if (w == ESP_TLS_ERR_SSL_WANT_READ || w == ESP_TLS_ERR_SSL_WANT_WRITE) {
                continue;
            }
            if (w <= 0) {
                ESP_LOGW(TAG, "write failed: %d", (int)w);
                return -1;
            }
            written += (size_t)w;
        }
    }
}

/* Runs one connection until it closes. */
static void run_connection(struct cft_tunnel *t, const char *host, uint8_t attempt)
{
    t->registered = false;
    t->closed = false;
    t->retry_after_ms = 0;

    esp_tls_cfg_t tls_cfg = {
        .cacert_buf = (const unsigned char *)cft_edge_ca_pem_str,
        .cacert_bytes = strlen(cft_edge_ca_pem_str) + 1,
        .common_name = CFT_EDGE_SNI, /* also used as SNI */
        .tls_version = ESP_TLS_VER_TLS_1_3,
        .timeout_ms = 10000,
        .keep_alive_cfg = &(tls_keep_alive_cfg_t)
        {
            .keep_alive_enable = true,
            .keep_alive_idle = 30,
            .keep_alive_interval = 5,
            .keep_alive_count = 3,
        },
    };
    esp_tls_t *tls = esp_tls_init();
    uint8_t *rx = malloc(RX_BUF_SIZE);
    cft_conn_t *conn = NULL;
    if (!tls || !rx) {
        ESP_LOGE(TAG, "out of memory");
        goto out;
    }
    ESP_LOGI(TAG, "connecting to %s:%d", host, CFT_EDGE_PORT);
    if (esp_tls_conn_new_sync(host, (int)strlen(host), CFT_EDGE_PORT, &tls_cfg, tls) != 1) {
        ESP_LOGW(TAG, "TLS connection to %s failed", host);
        goto out;
    }
    int fd = -1;
    esp_tls_get_conn_sockfd(tls, &fd);
    struct sockaddr_storage local = {0};
    socklen_t local_len = sizeof(local);
    getsockname(fd, (struct sockaddr *)&local, &local_len);
    const uint8_t *local_ip = NULL;
    size_t local_ip_len = 0;
    if (local.ss_family == AF_INET) {
        local_ip = (const uint8_t *) & ((struct sockaddr_in *)&local)->sin_addr;
        local_ip_len = 4;
    }

    cft_conn_config_t cc = {
        .token = &t->token,
        .conn_index = 0,
        .client_id = t->client_id,
        .version = t->cfg.version ? t->cfg.version : DEFAULT_VERSION,
        .arch = CONFIG_IDF_TARGET,
        .local_ip = local_ip,
        .local_ip_len = local_ip_len,
        .num_previous_attempts = attempt,
        .handler = t->cfg.handler,
        .handler_arg = t->cfg.handler_arg,
        .on_event = on_conn_event,
        .event_arg = t,
        .max_request_body = t->cfg.max_request_body,
        .max_concurrent_streams = t->cfg.max_concurrent_streams,
    };
    conn = cft_conn_new(&cc);
    if (!conn) {
        ESP_LOGE(TAG, "out of memory");
        goto out;
    }

    int64_t shutdown_deadline = 0;
    while (!t->closed) {
        if (t->stop && !shutdown_deadline) {
            cft_conn_shutdown(conn);
            shutdown_deadline = esp_timer_get_time() / 1000 + t->stop_timeout_ms;
        }
        if (shutdown_deadline && esp_timer_get_time() / 1000 > shutdown_deadline) {
            ESP_LOGW(TAG, "unregister timed out");
            break;
        }
        if (flush_out(conn, tls) < 0) {
            break;
        }
        if (esp_tls_get_bytes_avail(tls) <= 0) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            struct timeval tv = {.tv_sec = 0, .tv_usec = 500 * 1000};
            int sr = select(fd + 1, &rfds, NULL, NULL, &tv);
            if (sr < 0) {
                ESP_LOGW(TAG, "select failed");
                break;
            }
            if (sr == 0) {
                continue;
            }
        }
        ssize_t n = esp_tls_conn_read(tls, rx, RX_BUF_SIZE);
        if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (n <= 0) {
            ESP_LOGW(TAG, "edge closed the connection (%d)", (int)n);
            break;
        }
        cft_conn_recv(conn, rx, (size_t)n);
    }
    flush_out(conn, tls);

out:
    cft_conn_free(conn);
    free(rx);
    if (tls) {
        esp_tls_conn_destroy(tls);
    }
}

/* Sleeps up to ms, waking early on stop. */
static void wait_or_stop(struct cft_tunnel *t, uint32_t ms)
{
    if (!t->stop) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms));
    }
}

static void tunnel_task(void *arg)
{
    struct cft_tunnel *t = arg;
    static const char *const regions[] = {CFT_EDGE_REGION1, CFT_EDGE_REGION2};
    unsigned region = esp_random() & 1;
    uint8_t attempt = 0;
    uint32_t backoff_ms = BACKOFF_MIN_MS;

    while (!t->stop) {
        run_connection(t, regions[region], attempt);
        if (t->stop) {
            break;
        }
        if (t->registered) {
            /* Was up; reconnect promptly. */
            attempt = 0;
            backoff_ms = BACKOFF_MIN_MS;
        } else {
            if (attempt < UINT8_MAX) {
                attempt++;
            }
            region ^= 1;
        }
        uint32_t delay = backoff_ms + esp_random() % (backoff_ms / 2 + 1);
        if (t->retry_after_ms > delay) {
            delay = (uint32_t)t->retry_after_ms;
        }
        ESP_LOGI(TAG, "reconnecting in %u ms", (unsigned)delay);
        wait_or_stop(t, delay);
        if (!t->registered) {
            backoff_ms = backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff_ms * 2;
        }
    }
    xSemaphoreGive(t->done);
    vTaskDelete(NULL);
}

esp_err_t cft_tunnel_start(const cft_tunnel_config_t *cfg, cft_tunnel_handle_t *out)
{
    if (!cfg || !cfg->token || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    struct cft_tunnel *t = calloc(1, sizeof(*t));
    if (!t) {
        return ESP_ERR_NO_MEM;
    }
    if (!cft_token_parse(cfg->token, &t->token)) {
        ESP_LOGE(TAG, "invalid tunnel token");
        free(t);
        return ESP_ERR_INVALID_ARG;
    }
    t->cfg = *cfg;
    t->cfg.token = NULL;
    /* One connector ID for the lifetime of this tunnel, like cloudflared. */
    esp_fill_random(t->client_id, sizeof(t->client_id));
    t->client_id[6] = (t->client_id[6] & 0x0f) | 0x40;
    t->client_id[8] = (t->client_id[8] & 0x3f) | 0x80;
    t->done = xSemaphoreCreateBinary();
    uint32_t stack = cfg->task_stack_size ? cfg->task_stack_size : 8192;
    unsigned prio = cfg->task_priority ? cfg->task_priority : 5;
    if (!t->done || xTaskCreate(tunnel_task, "cf_tunnel", stack, t, prio, &t->task) != pdPASS) {
        if (t->done) {
            vSemaphoreDelete(t->done);
        }
        free(t);
        return ESP_ERR_NO_MEM;
    }
    *out = t;
    return ESP_OK;
}

esp_err_t cft_tunnel_stop(cft_tunnel_handle_t t, uint32_t timeout_ms)
{
    if (!t) {
        return ESP_ERR_INVALID_ARG;
    }
    t->stop_timeout_ms = timeout_ms ? timeout_ms : UNREGISTER_WAIT_MS;
    t->stop = true;
    xTaskNotifyGive(t->task);
    /* The connection loop polls every 500 ms; allow for TLS connect too. */
    if (xSemaphoreTake(t->done, pdMS_TO_TICKS(t->stop_timeout_ms + 15000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    vSemaphoreDelete(t->done);
    memset(&t->token, 0, sizeof(t->token));
    free(t);
    return ESP_OK;
}
