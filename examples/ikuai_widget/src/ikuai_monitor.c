// iKuai 路由器实时监视：HTTPS API + ICMP ping
// - system 端点 1s 轮询：实时上下行速率、CPU/内存/在线数
// - ping 网关 1s 采样：延迟曲线
// - 60 点环形缓存，供 UI 画实时滚动曲线
#include "ikuai_monitor.h"
#if __has_include("ikuai_cert.h")
#include "ikuai_cert.h"
#else
#include "ikuai_cert.example.h"
#endif
#if __has_include("config.h")
#include "config.h"
#else
#include "config.example.h"
#endif
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "apps/ping/ping_sock.h"   // IDF 6.0 无 esp_ping 组件，用 lwip ping_sock
#include "esp_netif.h"

static const char *TAG = "ikuai";

#define API_BASE IKUAI_HOST
#define HTTP_TIMEOUT_MS 5000

static SemaphoreHandle_t s_mux;
static ikuai_sys_t    s_sys;
static ikuai_extra_t  s_extra;
static ikuai_wan_t    s_extra_wan_tmp[2];
static ikuai_client_t s_extra_client_tmp[3];
static ikuai_curve_t  s_curve;
static volatile uint32_t s_last_ok_ts = 0;
static volatile float s_latest_ping_ms = -1;   // ping 回调只存最新值
static volatile ikuai_link_state_t s_link_state = IKUAI_LINK_WAIT;
static volatile bool s_network_changed;

// ─── HTTP（复用 client，IDF 6.0.1 每次 init/cleanup 泄漏 ~5KB）───

typedef struct {
    char *buf;
    int len;
    int cap;
    bool truncated;
} resp_t;
static esp_http_client_handle_t s_client = NULL;
static resp_t s_resp;
static bool s_logged_api_ok;
static bool s_logged_wan_shape;
static bool s_logged_client_shape;
static bool s_logged_ac_shape;
static int s_last_temp_shape = -1;

static esp_err_t on_http_evt(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        resp_t *r = (resp_t *)evt->user_data;
        int n = evt->data_len;
        int room = r->cap - r->len - 1;
        if (room <= 0) {
            r->truncated = true;
            return ESP_OK;
        }
        int copy = n < room ? n : room;
        memcpy(r->buf + r->len, evt->data, copy);
        r->len += copy;
        r->buf[r->len] = 0;
        if (copy != n) r->truncated = true;
    }
    return ESP_OK;
}

static bool api_get(const char *path, char *buf, int cap) {
    char url[200];
    int url_len = snprintf(url, sizeof(url), "%s%s", API_BASE, path);
    if (url_len <= 0 || url_len >= (int)sizeof(url)) {
        ESP_LOGW(TAG, "request URL too long: %s", path);
        s_link_state = IKUAI_LINK_HTTP_ERROR;
        return false;
    }
    if (!s_client) {
        esp_http_client_config_t cfg = {
            .url = API_BASE "/api/v4.0/monitoring/system",
            .method = HTTP_METHOD_GET,
            .timeout_ms = HTTP_TIMEOUT_MS,
            .cert_pem = ikuai_cert_pem,
            .skip_cert_common_name_check = true,
            .event_handler = on_http_evt,
            .user_data = &s_resp,
            .user_agent = "T-Display-S3/1.0",
        };
        s_client = esp_http_client_init(&cfg);
        if (!s_client) {
            s_link_state = IKUAI_LINK_NETWORK_ERROR;
            return false;
        }
        ESP_LOGI(TAG, "http client initialized once");
    }
    s_resp.buf = buf;
    s_resp.len = 0;
    s_resp.cap = cap;
    s_resp.truncated = false;
    esp_http_client_set_url(s_client, url);
    char auth[192];
    int auth_len = snprintf(auth, sizeof(auth), "Bearer %s", IKUAI_TOKEN);
    if (auth_len <= 0 || auth_len >= (int)sizeof(auth)) {
        ESP_LOGW(TAG, "Bearer token is too long");
        s_link_state = IKUAI_LINK_AUTH_ERROR;
        return false;
    }
    esp_http_client_set_header(s_client, "Authorization", auth);
    esp_err_t err = esp_http_client_perform(s_client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "http fail %s: %s", path, esp_err_to_name(err));
        s_link_state = err == ESP_ERR_TIMEOUT ? IKUAI_LINK_TIMEOUT : IKUAI_LINK_NETWORK_ERROR;
        return false;
    }
    int status = esp_http_client_get_status_code(s_client);
    if (status < 200 || status >= 300 || s_resp.truncated) {
        ESP_LOGW(TAG, "http invalid response %s: status=%d bytes=%d truncated=%d",
                 path, status, s_resp.len, s_resp.truncated);
        if (status == 401 || status == 403) s_link_state = IKUAI_LINK_AUTH_ERROR;
        else s_link_state = IKUAI_LINK_HTTP_ERROR;
        return false;
    }
    if (!s_logged_api_ok) {
        s_logged_api_ok = true;
        ESP_LOGI(TAG, "api online: status=%d bytes=%d", status, s_resp.len);
    }
    s_link_state = IKUAI_LINK_ONLINE;
    return s_resp.len > 0;
}

// ─── 极简 JSON 提取 ─────────────────────────────────────────────────

// Parse the first scalar in a JSON array, accepting both [53] and ["53"].
// An empty array, non-numeric text, or an implausible temperature is no data,
// not a real 0°C measurement.
static bool array_first_num(const char *s, const char *key,
                            double min_value, double max_value, double *out) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\":[", key);
    const char *p = strstr(s, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p == ']' || *p == '\0') return false;

    bool quoted = *p == '"';
    if (quoted) p++;
    char *endp;
    double v = strtod(p, &endp);
    if (endp == p) return false;
    if (quoted) {
        if (*endp != '"') return false;
    } else {
        while (*endp == ' ' || *endp == '\t' || *endp == '\r' || *endp == '\n') endp++;
        if (*endp != ',' && *endp != ']') return false;
    }
    if (!(v >= min_value && v <= max_value)) return false;
    *out = v;
    return true;
}

// Return a search boundary that never points beyond the terminating NUL.
// The API buffer is always NUL-terminated by on_http_evt().
static const char *json_window_end(const char *start, size_t max_len) {
    return start + strnlen(start, max_len);
}

static const char *json_array_end(const char *array_start) {
    const char *p = strchr(array_start, '[');
    if (!p) return NULL;
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (; *p; p++) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (*p == '\\') escaped = true;
            else if (*p == '"') quoted = false;
            continue;
        }
        if (*p == '"') quoted = true;
        else if (*p == '[') depth++;
        else if (*p == ']' && --depth == 0) return p;
    }
    return NULL;
}

static bool kv_num_range(const char *start, const char *end, const char *key, double *out) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(start, pat);
    if (!p || p > end) return false;
    p += strlen(pat);
    if (p > end) return false;
    while (p <= end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (p > end) return false;
    char *endp;
    double v = strtod(p, &endp);
    if (endp == p || endp > end) return false;
    while (endp <= end && (*endp == ' ' || *endp == '\t' ||
                           *endp == '\r' || *endp == '\n')) endp++;
    if (endp <= end && *endp != ',' && *endp != '}' && *endp != ']') return false;
    *out = v;
    return true;
}

static bool kv_u32_range(const char *start, const char *end, const char *key,
                         uint32_t *out) {
    double value;
    if (!kv_num_range(start, end, key, &value) ||
        value < 0.0 || value > (double)UINT32_MAX) return false;
    *out = (uint32_t)value;
    return true;
}

static bool kv_u64_range(const char *start, const char *end, const char *key,
                         uint64_t *out) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(start, pat);
    if (!p || p > end) return false;
    p += strlen(pat);
    while (p <= end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (p > end || *p == '-') return false;
    char *endp;
    unsigned long long v = strtoull(p, &endp, 10);
    if (endp == p || endp > end) return false;
    while (endp <= end && (*endp == ' ' || *endp == '\t' ||
                           *endp == '\r' || *endp == '\n')) endp++;
    if (endp <= end && *endp != ',' && *endp != '}' && *endp != ']') return false;
    *out = (uint64_t)v;
    return true;
}

static bool kv_str_range(const char *start, const char *end, const char *key,
                         char *out, int cap) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(start, pat);
    if (!p || p > end) return false;
    p += strlen(pat);
    if (p > end) return false;
    int i = 0;
    while (p <= end && *p && *p != '"' && i < cap - 1) out[i++] = *p++;
    out[i] = 0;
    return i > 0 && p <= end;
}

// 解析 system 响应，写入 sys 缓存 + 曲线缓存
static void parse_system(const char *resp) {
    ikuai_sys_t s = { 0 };
    uint32_t direct_down = 0, direct_up = 0;
    bool has_direct_down = false, has_direct_up = false;
    double cpu_temp = 0.0;
    bool cpu_temp_valid = array_first_num(resp, "cputemp", -50.0, 200.0,
                                          &cpu_temp);
    double value;
    s.cpu_valid = array_first_num(resp, "cpu", 0.0, 100.0, &value);
    if (s.cpu_valid) s.cpu_pct = (float)value;
    const char *mem = strstr(resp, "\"memory\":{");
    if (mem) {
        char used[16];
        if (kv_str_range(mem, json_window_end(mem, 300), "used", used, sizeof(used))) {
            char *endp;
            value = strtod(used, &endp);
            s.mem_valid = endp != used && *endp == '\0' && value >= 0.0 && value <= 100.0;
            if (s.mem_valid) s.mem_pct = (float)value;
        }
    }
    const char *ou = strstr(resp, "\"online_user\":{");
    if (ou)
        s.online_valid = kv_u32_range(ou, json_window_end(ou, 200), "count",
                                      &s.online_cnt);
    const char *st = strstr(resp, "\"stream\":{");
    if (st) {
        const char *stream_end = json_window_end(st, 600);
        s.conn_valid = kv_u32_range(st, stream_end, "connect_num", &s.conn_cnt);
        s.tcp_conn_valid = kv_u32_range(st, stream_end, "tcp_connect_num",
                                        &s.tcp_conn_cnt);
        s.udp_conn_valid = kv_u32_range(st, stream_end, "udp_connect_num",
                                        &s.udp_conn_cnt);
        s.icmp_conn_valid = kv_u32_range(st, stream_end, "icmp_connect_num",
                                         &s.icmp_conn_cnt);
        // download/upload 是 iKuai 当前瞬时速率字段；实时展示直接使用它们。
        // total_down/total_up 仅表示累计流量，不能与本次采样混算成另一套速率。
        has_direct_down = kv_u32_range(st, stream_end, "download", &direct_down);
        has_direct_up = kv_u32_range(st, stream_end, "upload", &direct_up);
        bool total_down_valid = kv_u64_range(st, stream_end, "total_down", &s.total_down);
        bool total_up_valid = kv_u64_range(st, stream_end, "total_up", &s.total_up);
        s.totals_valid = total_down_valid && total_up_valid;
    }
    // 空闲 WAN 的速率可以为 0；只有瞬时字段缺失才判定本次解析失败。
    s.ok = st != NULL && has_direct_down && has_direct_up;
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    s.down_bps = has_direct_down ? direct_down : 0;
    s.up_bps = has_direct_up ? direct_up : 0;
    s.ts = (uint32_t)(now_us / 1000000);
    if (s.ok) s_last_ok_ts = s.ts;

    xSemaphoreTake(s_mux, portMAX_DELAY);
    // system 响应附带的健康字段:cputemp[]/uptime/verinfo.verstring（1s 同步）
    {
        s_extra.cpu_temp_valid = cpu_temp_valid;
        s_extra.cpu_temp = cpu_temp_valid ? (float)cpu_temp : 0.0f;
        s_extra.uptime_valid = kv_u32_range(resp, resp + strlen(resp), "uptime",
                                            &s_extra.uptime_sec);
        const char *vi = strstr(resp, "\"verinfo\":{");
        s_extra.version[0] = '\0';
        s_extra.version_valid = vi &&
            kv_str_range(vi, json_window_end(vi, 400), "verstring",
                         s_extra.version, sizeof(s_extra.version));
    }
    s_sys = s;
    if (s.ok) {
        s_extra.system_ts = s.ts;
        s_extra.ok = true;
        // 速率 + ping 统一进曲线（1s 采样，由本函数推进槽位）
        int h = s_curve.head;
        s_curve.down[h] = s.down_bps;
        s_curve.up[h] = s.up_bps;
        s_curve.ping_ms[h] = s_latest_ping_ms;
        if (s_curve.n < IKUAI_CURVE_MAX) s_curve.n++;
        s_curve.head = (h + 1) % IKUAI_CURVE_MAX;
        s_curve.ts = s.ts;
    }
    xSemaphoreGive(s_mux);
    int temp_shape = cpu_temp_valid ? 1 : 0;
    if (temp_shape != s_last_temp_shape) {
        if (cpu_temp_valid)
            ESP_LOGI(TAG, "router temperature ready: %.1f C", cpu_temp);
        else
            ESP_LOGI(TAG, "router temperature unavailable: cputemp is empty or invalid");
        s_last_temp_shape = temp_shape;
    }
}


// ─── 扩展端点解析（极简 strstr 提取，与现有风格一致）────────────────

static const char *next_flat_object(const char *cursor, const char *section_end,
                                    const char **object_end) {
    const char *start = strchr(cursor, '{');
    if (!start || (section_end && start >= section_end)) return NULL;
    const char *end = strchr(start, '}');
    if (!end || (section_end && end > section_end)) return NULL;
    *object_end = end;
    return start;
}

// WAN 线路:/monitoring/interfaces-status → iface_check[]
static void parse_wan(const char *resp) {
    const char *arr = strstr(resp, "\"iface_check\":[");
    if (!arr) return;
    const char *checks_end = json_array_end(arr);
    if (!checks_end) return;
    memset(s_extra_wan_tmp, 0, sizeof(s_extra_wan_tmp));
    int cnt = 0;
    const char *streams = strstr(checks_end, "\"iface_stream\":[");
    const char *p = arr;
    const char *obj_end;
    const char *obj;
    while (cnt < 2 &&
           (obj = next_flat_object(p, checks_end, &obj_end)) != NULL) {
        ikuai_wan_t *w = &s_extra_wan_tmp[cnt];
        if (!kv_str_range(obj, obj_end, "interface", w->name, sizeof(w->name))) {
            p = obj_end + 1;
            continue;
        }
        kv_str_range(obj, obj_end, "ip_addr", w->ip, sizeof(w->ip));
        kv_str_range(obj, obj_end, "gateway", w->gateway, sizeof(w->gateway));
        char res[12] = "";
        w->status_valid = kv_str_range(obj, obj_end, "result", res, sizeof(res));
        w->ok = strcmp(res, "success") == 0;
        cnt++;
        p = obj_end + 1;
    }

    // iface_stream[] 与 iface_check[] 使用 interface 字段关联；这里只合并
    // 已识别的两条 WAN，不把 LAN/虚拟接口误塞进页面。
    int merged_streams = 0;
    p = streams;
    const char *streams_end = streams ? json_array_end(streams) : NULL;
    while (p && streams_end &&
           (obj = next_flat_object(p, streams_end, &obj_end)) != NULL) {
        char name[12] = "";
        if (!kv_str_range(obj, obj_end, "interface", name, sizeof(name))) {
            p = obj_end + 1;
            continue;
        }
        ikuai_wan_t *w = NULL;
        for (int i = 0; i < cnt; i++) {
            if (strcmp(s_extra_wan_tmp[i].name, name) == 0) {
                w = &s_extra_wan_tmp[i];
                break;
            }
        }
        if (w) {
            char conn[16] = "";
            if (kv_str_range(obj, obj_end, "connect_num", conn, sizeof(conn))) {
                char *endp;
                unsigned long parsed = strtoul(conn, &endp, 10);
                w->conn_valid = endp != conn && *endp == '\0' && parsed <= UINT32_MAX;
                if (w->conn_valid) w->conn_cnt = (uint32_t)parsed;
            } else {
                w->conn_valid = kv_u32_range(obj, obj_end, "connect_num",
                                             &w->conn_cnt);
            }
            bool down_valid = kv_u32_range(obj, obj_end, "download", &w->down_bps);
            bool up_valid = kv_u32_range(obj, obj_end, "upload", &w->up_bps);
            w->rate_valid = down_valid && up_valid;
            kv_u64_range(obj, obj_end, "total_down", &w->total_down);
            kv_u64_range(obj, obj_end, "total_up", &w->total_up);
            bool down_packets_valid = kv_u64_range(obj, obj_end, "downpacked",
                                                    &w->down_packets);
            bool up_packets_valid = kv_u64_range(obj, obj_end, "uppacked",
                                                  &w->up_packets);
            bool down_dropped_valid = kv_u64_range(obj, obj_end, "downdropped",
                                                    &w->down_dropped);
            bool up_dropped_valid = kv_u64_range(obj, obj_end, "updropped",
                                                  &w->up_dropped);
            w->drop_valid = down_packets_valid && up_packets_valid &&
                            down_dropped_valid && up_dropped_valid;
            merged_streams++;
        }
        p = obj_end + 1;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_extra.wan_cnt = cnt;
    for (int i = 0; i < cnt; i++) s_extra.wan[i] = s_extra_wan_tmp[i];
    s_extra.ok = true;
    s_extra.wan_ts = (uint32_t)(esp_timer_get_time() / 1000000);
    xSemaphoreGive(s_mux);
    if (!s_logged_wan_shape) {
        ESP_LOGI(TAG, "WAN telemetry ready: lines=%d streams=%d", cnt,
                 merged_streams);
        s_logged_wan_shape = true;
    }
}

// 终端 Top3:/monitoring/clients-online?order_by=download&order=desc&limit=3
static void parse_clients(const char *resp) {
    const char *arr = strstr(resp, "\"data\":[");
    if (!arr) return;
    const char *data_end = json_array_end(arr);
    if (!data_end) return;
    memset(s_extra_client_tmp, 0, sizeof(s_extra_client_tmp));
    int cnt = 0;
    const char *p = arr;
    const char *obj_end;
    const char *obj;
    while (cnt < 3 &&
           (obj = next_flat_object(p, data_end, &obj_end)) != NULL) {
        ikuai_client_t *c = &s_extra_client_tmp[cnt];
        kv_str_range(obj, obj_end, "ip_addr", c->ip, sizeof(c->ip));
        // 名称优先级:termname > comment > hostname > ip
        if (!kv_str_range(obj, obj_end, "termname", c->name, sizeof(c->name)) &&
            !kv_str_range(obj, obj_end, "comment", c->name, sizeof(c->name)) &&
            !kv_str_range(obj, obj_end, "hostname", c->name, sizeof(c->name)))
            snprintf(c->name, sizeof(c->name), "%s", c->ip[0] ? c->ip : "CLIENT");
        c->down_valid = kv_u32_range(obj, obj_end, "download", &c->down_bps);
        c->up_valid = kv_u32_range(obj, obj_end, "upload", &c->up_bps);
        double signal;
        if (kv_num_range(obj, obj_end, "signal", &signal) &&
            signal >= -200.0 && signal <= 100.0) c->signal_dbm = (int)signal;
        kv_str_range(obj, obj_end, "interface", c->interface, sizeof(c->interface));
        kv_str_range(obj, obj_end, "frequencies", c->frequency, sizeof(c->frequency));
        cnt++;
        p = obj_end + 1;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_extra.client_cnt = cnt;
    for (int i = 0; i < cnt; i++) s_extra.client[i] = s_extra_client_tmp[i];
    s_extra.ok = true;
    s_extra.clients_ts = (uint32_t)(esp_timer_get_time() / 1000000);
    xSemaphoreGive(s_mux);
    if (!s_logged_client_shape) {
        ESP_LOGI(TAG,
                 "client telemetry ready: rows=%d first_down=%u first_up=%u valid=%d/%d",
                 cnt, cnt > 0 ? (unsigned)s_extra_client_tmp[0].down_bps : 0,
                 cnt > 0 ? (unsigned)s_extra_client_tmp[0].up_bps : 0,
                 cnt > 0 && s_extra_client_tmp[0].down_valid,
                 cnt > 0 && s_extra_client_tmp[0].up_valid);
        s_logged_client_shape = true;
    }
}

static void mark_system_sample_failed(void) {
    if (!s_mux) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_sys.ok = false;
    xSemaphoreGive(s_mux);
}

// AC 状态:/network/ac/services + /monitoring/wireless-statistics
static void parse_ac_status(const char *resp) {
    double v;
    const char *p = strstr(resp, "\"ac_status\":");
    if (!p || !kv_num_range(p, json_window_end(p, 30), "ac_status", &v)) return;
    int ac_on = (int)v;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    int was_on = s_extra.ac_on;
    s_extra.ac_on = ac_on;
    s_extra.ok = true;
    // AC 关闭时开关状态就是完整结果；开启时等待 statistics 成功后
    // 再提交时间戳，避免旧 AP/终端数据被重新标成 LIVE。
    if (!ac_on) {
        s_extra.ap_online = s_extra.ap_count = 0;
        s_extra.clt_2g = s_extra.clt_5g = 0;
        s_extra.clt_active = s_extra.clt_inactive = s_extra.clt_peak = 0;
        s_extra.ac_ts = (uint32_t)(esp_timer_get_time() / 1000000);
    } else if (!was_on) {
        s_extra.ac_ts = 0;
    }
    xSemaphoreGive(s_mux);
    if (!ac_on && !s_logged_ac_shape) {
        ESP_LOGI(TAG, "wireless telemetry ready: ac=off");
        s_logged_ac_shape = true;
    }
}

static void parse_ac_stat(const char *resp) {
    double ap_count = 0, ap_online = 0, c2 = 0, c5 = 0;
    double active = 0, inactive = 0, peak = 0;
    const char *ap = strstr(resp, "\"ap_status\":{");
    bool ap_valid = false, clients_valid = false;
    if (ap) {
        const char *ap_end = json_window_end(ap, 200);
        ap_valid = kv_num_range(ap, ap_end, "ap_count", &ap_count) &&
                   kv_num_range(ap, ap_end, "ap_online", &ap_online);
    }
    const char *cl = strstr(resp, "\"clt_status\":{");
    if (cl) {
        const char *cl_end = json_window_end(cl, 500);
        clients_valid = kv_num_range(cl, cl_end, "clt_count_2g", &c2) &&
                        kv_num_range(cl, cl_end, "clt_count_5g", &c5) &&
                        kv_num_range(cl, cl_end, "clt_active", &active) &&
                        kv_num_range(cl, cl_end, "clt_inactive", &inactive) &&
                        kv_num_range(cl, cl_end, "clt_max_online", &peak);
    }
    if (!ap_valid || !clients_valid) {
        ESP_LOGW(TAG, "wireless statistics incomplete; keeping previous sample stale");
        return;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_extra.ap_count = (int)ap_count; s_extra.ap_online = (int)ap_online;
    s_extra.clt_2g = (int)c2;         s_extra.clt_5g = (int)c5;
    s_extra.clt_active = (int)active;
    s_extra.clt_inactive = (int)inactive;
    s_extra.clt_peak = (int)peak;
    s_extra.ac_ts = (uint32_t)(esp_timer_get_time() / 1000000);
    s_extra.ok = true;
    xSemaphoreGive(s_mux);
    if (!s_logged_ac_shape) {
        ESP_LOGI(TAG,
                 "wireless telemetry ready: ap=%d/%d clients_2g=%d clients_5g=%d active=%d idle=%d peak=%d",
                 (int)ap_online, (int)ap_count, (int)c2, (int)c5,
                 (int)active, (int)inactive, (int)peak);
        s_logged_ac_shape = true;
    }
}

// ─── Ping（ICMP 网关，1s 采样）──────────────────────────────────────

static esp_ping_handle_t s_ping = NULL;

static void on_ping_success(esp_ping_handle_t hdl, void *args) {
    uint32_t t = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &t, sizeof(t));
    s_latest_ping_ms = (float)t;
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args) {
    s_latest_ping_ms = -1;
}

static bool ping_start(void) {
    ip_addr_t gw;
    esp_netif_ip_info_t ipi;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta || esp_netif_get_ip_info(sta, &ipi) != ESP_OK ||
        ipi.ip.addr == 0 || ipi.gw.addr == 0) return false;
    ip_addr_copy_from_ip4(gw, ipi.gw);     // 用实际网关
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = gw;
    cfg.count = ESP_PING_COUNT_INFINITE;   // 持续 ping（1s 间隔实时曲线）
    cfg.interval_ms = 1000;                // 1s 采样
    cfg.timeout_ms = 800;
    esp_ping_callbacks_t cbs = {
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
    };
    if (esp_ping_new_session(&cfg, &cbs, &s_ping) == ESP_OK) {
        esp_ping_start(s_ping);
        ESP_LOGI(TAG, "ping started (1s interval)");
        return true;
    }
    return false;
}

// ─── 轮询任务 ───────────────────────────────────────────────────────

static void poll_task(void *arg) {
    enum { BUF_SZ = 8192 };
    // The response body is CPU-only scratch data. A normal 8 KB malloc is
    // forced into internal RAM by CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384,
    // so request PSRAM explicitly and leave internal RAM for Wi-Fi/TLS.
    char *buf = (char *)heap_caps_malloc(BUF_SZ,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "PSRAM response buffer allocation failed (%u bytes)",
                 (unsigned)BUF_SZ);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "response buffer allocated in PSRAM: %u bytes",
             (unsigned)BUF_SZ);
    uint32_t next = 0, next_ext = 0;
    int ext_slot = 0;
    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
        if (s_network_changed) {
            s_network_changed = false;
            if (s_ping) {
                esp_ping_stop(s_ping);
                esp_ping_delete_session(s_ping);
                s_ping = NULL;
            }
            mark_system_sample_failed();
        }
        if (!s_ping) ping_start();
        if (now >= next) {
            next = now + 1;   // 1s 实时采样
            if (api_get("/api/v4.0/monitoring/system", buf, BUF_SZ))
                parse_system(buf);
            else
                mark_system_sample_failed();
        }
        if (now >= next_ext) {
            next_ext = now + 3;   // 每 3s 发一类扩展请求；同一端点约 9s 更新
            switch (ext_slot) {
            case 0:
                if (api_get("/api/v4.0/monitoring/interfaces-status", buf, BUF_SZ))
                    parse_wan(buf);
                break;
            case 1:
                if (api_get("/api/v4.0/monitoring/clients-online?page=1&limit=3&order_by=download&order=desc", buf, BUF_SZ))
                    parse_clients(buf);
                break;
            case 2:
                if (api_get("/api/v4.0/network/ac/services", buf, BUF_SZ)) {
                    parse_ac_status(buf);
                    xSemaphoreTake(s_mux, portMAX_DELAY);
                    int ac = s_extra.ac_on;
                    xSemaphoreGive(s_mux);
                    if (ac && api_get("/api/v4.0/monitoring/wireless-statistics", buf, BUF_SZ))
                        parse_ac_stat(buf);
                }
                break;
            }
            ext_slot = (ext_slot + 1) % 3;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ─── 对外接口 ───────────────────────────────────────────────────────

void ikuai_monitor_start(void) {
    s_mux = xSemaphoreCreateMutex();
    if (!s_mux) return;
    xTaskCreate(poll_task, "ikuai", 8192, NULL, 4, NULL);
    ESP_LOGI(TAG, "ikuai monitor started");
}

void ikuai_monitor_network_changed(void) {
    s_network_changed = true;
}

bool ikuai_get_sys(ikuai_sys_t *out) {
    if (!s_mux) return false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_sys;
    xSemaphoreGive(s_mux);
    return out->ok && ikuai_recently_ok();
}

bool ikuai_get_curve(ikuai_curve_t *out) {
    if (!s_mux) return false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_curve;
    xSemaphoreGive(s_mux);
    return out->n > 0;
}

bool ikuai_get_extra(ikuai_extra_t *out) {
    if (!s_mux) return false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_extra;
    xSemaphoreGive(s_mux);
    return out->ok;
}

bool ikuai_recently_ok(void) {
    if (!s_last_ok_ts) return false;
    return (uint32_t)(esp_timer_get_time() / 1000000) - s_last_ok_ts < 10;
}

ikuai_link_state_t ikuai_get_link_state(void) {
    return s_link_state;
}
