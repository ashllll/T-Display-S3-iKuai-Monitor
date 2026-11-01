// rp5-agent: one read-only HTTP GET per second against the gateway's status
// endpoint. See agent/rp5-agent.py for the data source and its JSON contract.
#include "rp5_monitor.h"
#if __has_include("config.h")
#include "config.h"
#else
#include "config.example.h"
#endif
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"

static const char *TAG = "rp5";
#define HTTP_TIMEOUT_MS 4000
#define STATUS_PATH "/api/v1/status"
#define BUF_SZ 8192
#define FRESH_SEC 10

#ifndef RP5_HOST
#error "RP5_HOST must be defined in config.h / config.example.h"
#endif
#ifndef RP5_PORT
#define RP5_PORT 8799
#endif
#ifndef RP5_TOKEN
#define RP5_TOKEN ""
#endif

static SemaphoreHandle_t s_mux;
static rp5_sys_t s_sys;
static rp5_proxy_t s_proxy;
static rp5_gw_t s_gw;
static rp5_curve_t s_curve;
static volatile uint32_t s_last_ok_ts;
static volatile rp5_link_state_t s_link_state = RP5_LINK_WAIT;
static volatile bool s_network_changed;

typedef struct { char *buf; int len, cap; bool truncated; } resp_t;
static resp_t s_resp;
static esp_http_client_handle_t s_client;

static uint32_t now_sec(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

// Host must be a bare address: the agent speaks plain HTTP on one port.
static bool config_valid(void) {
    if (!RP5_HOST[0] || strncasecmp(RP5_HOST, "http://", 7) == 0 ||
        strncasecmp(RP5_HOST, "https://", 8) == 0 ||
        strpbrk(RP5_HOST, "/?#@ \r\n"))
        return false;
    // Reject anything that cannot be a bearer token; a stray CR/LF would
    // otherwise be spliced into the request header.
    if (strspn(RP5_TOKEN, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~") !=
        strlen(RP5_TOKEN))
        return false;
    return RP5_PORT > 0 && RP5_PORT < 65536;
}

static esp_err_t on_http_evt(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        resp_t *r = evt->user_data;
        int room = r->cap - r->len - 1;
        int copy = evt->data_len < room ? evt->data_len : room;
        if (copy > 0) {
            memcpy(r->buf + r->len, evt->data, copy);
            r->len += copy;
            r->buf[r->len] = 0;
        }
        if (copy != evt->data_len) r->truncated = true;
    }
    return ESP_OK;
}

// Only the polling task owns this handle, including recovery after Wi-Fi changes.
static void discard_http_client(void) {
    if (s_client) {
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
}

static bool api_get(char *buf, int cap) {
    if (!config_valid()) { s_link_state = RP5_LINK_CONFIG_ERROR; return false; }
    char url[160];
    int n = snprintf(url, sizeof(url), "http://%s:%d%s", RP5_HOST, RP5_PORT, STATUS_PATH);
    if (n <= 0 || n >= (int)sizeof(url)) { s_link_state = RP5_LINK_CONFIG_ERROR; return false; }
    if (!s_client) {
        esp_http_client_config_t cfg = {
            .url = url, .method = HTTP_METHOD_GET, .timeout_ms = HTTP_TIMEOUT_MS,
            .disable_auto_redirect = true,
            .event_handler = on_http_evt, .user_data = &s_resp,
            .user_agent = "T-Display-S3-rp5/1.0",
        };
        s_client = esp_http_client_init(&cfg);
        if (!s_client) { s_link_state = RP5_LINK_NETWORK_ERROR; return false; }
    }
    s_resp = (resp_t){ .buf = buf, .cap = cap };
    buf[0] = 0;
    char auth[96];
    int auth_n = 0;
    if (RP5_TOKEN[0])
        auth_n = snprintf(auth, sizeof(auth), "Bearer %s", RP5_TOKEN);
    if (esp_http_client_set_url(s_client, url) != ESP_OK ||
        esp_http_client_set_timeout_ms(s_client, HTTP_TIMEOUT_MS) != ESP_OK ||
        esp_http_client_set_header(s_client, "Accept", "application/json") != ESP_OK ||
        (RP5_TOKEN[0] && (auth_n <= 0 || auth_n >= (int)sizeof(auth) ||
         esp_http_client_set_header(s_client, "Authorization", auth) != ESP_OK))) {
        discard_http_client();
        s_link_state = RP5_LINK_HTTP_ERROR; return false;
    }
    esp_err_t err = esp_http_client_perform(s_client);
    if (err != ESP_OK) {
        // IDF can leave a timed-out request waiting for its old response headers.
        // Recreate it so the next attempt actually sends a fresh GET.
        discard_http_client();
        s_link_state = err == ESP_ERR_TIMEOUT || err == ESP_ERR_HTTP_EAGAIN ?
                       RP5_LINK_TIMEOUT : RP5_LINK_NETWORK_ERROR;
        ESP_LOGW(TAG, "GET failed: %s", esp_err_to_name(err));
        return false;
    }
    if (s_network_changed) {
        discard_http_client();
        s_link_state = RP5_LINK_NETWORK_ERROR; return false;
    }
    if (!esp_http_client_is_complete_data_received(s_client) || s_resp.truncated) {
        discard_http_client();
        s_link_state = RP5_LINK_HTTP_ERROR;
        ESP_LOGW(TAG, "GET incomplete response");
        return false;
    }
    int status = esp_http_client_get_status_code(s_client);
    if (status != 200 || !s_resp.len) {
        s_link_state = RP5_LINK_HTTP_ERROR;
        ESP_LOGW(TAG, "GET rejected: HTTP %d", status);
        return false;
    }
    return true;
}

// Pure typed JSON helpers, shared by the host fixture tests.
static const cJSON *field(const cJSON *object, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(object, key);
}

static bool number(const cJSON *item, double min, double max, double *out) {
    if (!cJSON_IsNumber(item)) return false;
    double v = item->valuedouble;
    if (!isfinite(v) || v < min || v > max) return false;
    *out = v;
    return true;
}

static bool num_field(const cJSON *object, const char *key, double min, double max,
                      double *out) {
    return number(field(object, key), min, max, out);
}

static bool bool_field(const cJSON *object, const char *key, bool *out) {
    const cJSON *item = field(object, key);
    if (!cJSON_IsBool(item)) return false;
    *out = cJSON_IsTrue(item);
    return true;
}

static bool text_field(const cJSON *object, const char *key, char *out, size_t cap) {
    const cJSON *item = field(object, key);
    if (!cJSON_IsString(item) || !item->valuestring[0]) return false;
    snprintf(out, cap, "%s", item->valuestring);
    return true;
}

static void parse_agent(const cJSON *root, rp5_sys_t *sys) {
    const cJSON *agent = field(root, "agent");
    if (!cJSON_IsObject(agent)) return;
    double v;
    if (num_field(agent, "up_s", 0, 1e9, &v)) {
        sys->agent_valid = true;
        sys->agent_up_s = (uint32_t)v;
    }
    const cJSON *ex = field(agent, "node_exporter");
    if (cJSON_IsObject(ex) && bool_field(ex, "ok", &sys->ex_ok)) {
        sys->ex_valid = true;
        if (sys->ex_ok && num_field(ex, "ms", 0, 60000, &v)) sys->ex_ms = (uint32_t)v;
    }
}

static void parse_pi(const cJSON *root, rp5_sys_t *sys) {
    const cJSON *pi = field(root, "pi");
    if (!cJSON_IsObject(pi)) return;
    bool ok = false;
    if (bool_field(pi, "ok", &ok)) sys->ok = ok;
    double v;
    if (num_field(pi, "cpu_pct", 0, 100, &v)) { sys->cpu_valid = true; sys->cpu_pct = (float)v; }
    if (num_field(pi, "mem_pct", 0, 100, &v)) { sys->mem_valid = true; sys->mem_pct = (float)v; }
    if (num_field(pi, "temp_c", -20, 150, &v)) { sys->temp_valid = true; sys->temp_c = (float)v; }
    if (num_field(pi, "load1", 0, 4096, &v)) {
        sys->load_valid = true; sys->load1 = (float)v;
        num_field(pi, "load5", 0, 4096, &v); sys->load5 = (float)v;
        num_field(pi, "load15", 0, 4096, &v); sys->load15 = (float)v;
    }
    if (num_field(pi, "uptime_s", 0, 4e9, &v)) { sys->uptime_valid = true; sys->uptime_s = (uint32_t)v; }
    if (num_field(pi, "disk_pct", 0, 100, &v)) { sys->disk_valid = true; sys->disk_pct = (float)v; }
    if (num_field(pi, "freq_mhz", 0, 100000, &v)) { sys->freq_valid = true; sys->freq_mhz = (uint32_t)v; }
}

static void parse_proxy(const cJSON *root, rp5_proxy_t *proxy) {
    const cJSON *item = field(root, "proxy");
    if (!cJSON_IsObject(item)) return;
    bool ok = false;
    if (bool_field(item, "ok", &ok)) proxy->ok = ok;
    text_field(item, "mode", proxy->mode, sizeof(proxy->mode));
    proxy->node_valid = text_field(item, "node", proxy->node, sizeof(proxy->node));
    double v;
    if (num_field(item, "delay_ms", 1, 600000, &v)) {
        proxy->delay_valid = true; proxy->delay_ms = (uint32_t)v;
    }
    if (num_field(item, "conns", 0, 1e6, &v)) proxy->conns = (uint32_t)v;
    if (num_field(item, "down_bps", 0, 1e12, &v)) proxy->down_bps = (uint32_t)v;
    if (num_field(item, "up_bps", 0, 1e12, &v)) proxy->up_bps = (uint32_t)v;
    if (num_field(item, "down_total", 0, 1e17, &v)) proxy->down_total = (uint64_t)v;
    if (num_field(item, "up_total", 0, 1e17, &v)) proxy->up_total = (uint64_t)v;
}

static void parse_gw(const cJSON *root, rp5_gw_t *gw) {
    const cJSON *item = field(root, "gw");
    if (!cJSON_IsObject(item)) return;
    bool ok = false;
    if (bool_field(item, "ok", &ok)) gw->ok = ok;
    double v;
    if (num_field(item, "wan_rx", 0, 1e12, &v)) gw->wan_rx = (uint32_t)v;
    if (num_field(item, "wan_tx", 0, 1e12, &v)) gw->wan_tx = (uint32_t)v;
    if (num_field(item, "lan_rx", 0, 1e12, &v)) gw->lan_rx = (uint32_t)v;
    if (num_field(item, "lan_tx", 0, 1e12, &v)) gw->lan_tx = (uint32_t)v;
    if (num_field(item, "px_rx", 0, 1e12, &v)) gw->px_rx = (uint32_t)v;
    if (num_field(item, "px_tx", 0, 1e12, &v)) gw->px_tx = (uint32_t)v;
    if (num_field(item, "leases", 0, 1e6, &v)) {
        gw->leases_valid = true; gw->leases = (uint32_t)v;
    }
}

static bool parse_status(const char *json, rp5_sys_t *sys, rp5_proxy_t *proxy, rp5_gw_t *gw) {
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    memset(sys, 0, sizeof(*sys));
    memset(proxy, 0, sizeof(*proxy));
    memset(gw, 0, sizeof(*gw));
    parse_agent(root, sys);
    parse_pi(root, sys);
    parse_proxy(root, proxy);
    parse_gw(root, gw);
    cJSON_Delete(root);
    return true;
}

static void mark_failed(void) {
    if (!s_mux) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_sys.ok = false;
    s_proxy.ok = false;
    s_gw.ok = false;
    xSemaphoreGive(s_mux);
}

// Commit one successful sample; the chart only advances on real gateway data.
static void commit(rp5_sys_t *sys, rp5_proxy_t *proxy, rp5_gw_t *gw, uint32_t ts) {
    xSemaphoreTake(s_mux, portMAX_DELAY);
    sys->ts = ts;
    proxy->ts = ts;
    gw->ts = ts;
    s_sys = *sys;
    s_proxy = *proxy;
    s_gw = *gw;
    if (gw->ok) {
        int h = s_curve.head;
        s_curve.down[h] = gw->wan_rx;
        s_curve.up[h] = gw->wan_tx;
        s_curve.sample_ts[h] = ts;
        s_curve.head = (h + 1) % RP5_CURVE_MAX;
        if (s_curve.n < RP5_CURVE_MAX) s_curve.n++;
        s_curve.ts = ts;
        s_curve.ok = true;
    }
    xSemaphoreGive(s_mux);
    s_last_ok_ts = ts;
}

// Only GET requests, one per second, with exponential backoff on failure.
static void poll_task(void *arg) {
    char *buf = heap_caps_malloc(BUF_SZ, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { s_link_state = RP5_LINK_NETWORK_ERROR; vTaskDelete(NULL); return; }
    int64_t next = 0;
    unsigned failures = 0;
    for (;;) {
        if (s_network_changed) {
            discard_http_client();
            xSemaphoreTake(s_mux, portMAX_DELAY);
            memset(&s_sys, 0, sizeof(s_sys));
            memset(&s_proxy, 0, sizeof(s_proxy));
            memset(&s_gw, 0, sizeof(s_gw));
            memset(&s_curve, 0, sizeof(s_curve));
            s_network_changed = false;
            xSemaphoreGive(s_mux);
            s_last_ok_ts = 0;
            next = 0;
            failures = 0;
        }
        int64_t now = esp_timer_get_time() / 1000;
        if (now < next) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        rp5_sys_t sys;
        rp5_proxy_t proxy;
        rp5_gw_t gw;
        if (!api_get(buf, BUF_SZ) || !parse_status(buf, &sys, &proxy, &gw)) {
            mark_failed();
            if (s_link_state == RP5_LINK_ONLINE) s_link_state = RP5_LINK_HTTP_ERROR;
            failures = failures < 4 ? failures + 1 : 4;
            next = esp_timer_get_time() / 1000 + (2000LL << failures);
            continue;
        }
        s_link_state = RP5_LINK_ONLINE;
        failures = 0;
        commit(&sys, &proxy, &gw, now_sec());
        next = esp_timer_get_time() / 1000 + 1000;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ─── 对外接口 ───────────────────────────────────────────────────────

static void *json_alloc(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void rp5_monitor_start(void) {
    cJSON_Hooks hooks = { .malloc_fn = json_alloc, .free_fn = free };
    cJSON_InitHooks(&hooks);
    s_mux = xSemaphoreCreateMutex();
    if (!s_mux) return;
    if (xTaskCreate(poll_task, "rp5", 6144, NULL, 4, NULL) != pdPASS)
        s_link_state = RP5_LINK_NETWORK_ERROR;
    ESP_LOGI(TAG, "rp5 monitor started");
}

void rp5_monitor_network_changed(void) {
    if (!s_mux) { s_network_changed = true; return; }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_network_changed = true;
    memset(&s_sys, 0, sizeof(s_sys));
    memset(&s_proxy, 0, sizeof(s_proxy));
    memset(&s_gw, 0, sizeof(s_gw));
    memset(&s_curve, 0, sizeof(s_curve));
    xSemaphoreGive(s_mux);
}

bool rp5_recently_ok(void) {
    if (!s_last_ok_ts) return false;
    return now_sec() - s_last_ok_ts < FRESH_SEC;
}

bool rp5_get_sys(rp5_sys_t *out) {
    if (!s_mux) return false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_sys;
    xSemaphoreGive(s_mux);
    return out->ok && rp5_recently_ok();
}

bool rp5_get_proxy(rp5_proxy_t *out) {
    if (!s_mux) return false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_proxy;
    xSemaphoreGive(s_mux);
    return out->ok && rp5_recently_ok();
}

bool rp5_get_gw(rp5_gw_t *out) {
    if (!s_mux) return false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_gw;
    xSemaphoreGive(s_mux);
    return out->ok && rp5_recently_ok();
}

bool rp5_get_curve(rp5_curve_t *out) {
    if (!s_mux) return false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_curve;
    xSemaphoreGive(s_mux);
    return out->n > 0;
}

rp5_link_state_t rp5_get_link_state(void) {
    return s_link_state;
}
