// iKuai 实时监视小屏（酷态科小屏设计语言:黑底 + 大数字 + 胶囊标签 + 无网格曲线）
// 首页只保留 WAN 状态、实时上下行、在线设备、PING 和约 10 秒三色趋势。
// 每个刷新帧固定左移 1px，并用临界阻尼连续跟随新采样，避免低速走格与折线突变。
#include "desktop_widget.h"
#include "lcd_driver.h"
#if __has_include("config.h")
#include "config.h"
#else
#include "config.example.h"
#endif
#include "lvgl.h"
#include "lv_port_disp.h"
#include "ikuai_monitor.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "esp_sntp.h"
#include <time.h>

LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(ui_font_crisp_12);
LV_FONT_DECLARE(ui_font_crisp_36);

#define UI_FONT_META  (&ui_font_crisp_12)
#define UI_FONT_TITLE (&lv_font_montserrat_20)
#define UI_FONT_METRIC (&ui_font_crisp_36)

#ifndef APP_DEMO_MODE
#define APP_DEMO_MODE 0
#endif
#if defined(APP_REQUIRE_LIVE_MODE) && APP_DEMO_MODE
#error "APP_REQUIRE_LIVE_MODE requires APP_DEMO_MODE=0"
#endif
#ifndef APP_TZ
#define APP_TZ "CST-8"
#endif
#ifndef APP_REDUCED_MOTION
#define APP_REDUCED_MOTION 0
#endif
#ifndef APP_NIGHT_START
#define APP_NIGHT_START 23
#define APP_NIGHT_END 7
#define APP_BL_NIGHT_PCT 8
#endif

typedef enum {
    DATA_TRUST_WARMING,
    DATA_TRUST_LIVE,
    DATA_TRUST_STALE,
    DATA_TRUST_TIMEOUT,
    DATA_TRUST_OFFLINE,
    DATA_TRUST_DEMO,
    DATA_TRUST_NO_DATA,
} data_trust_t;

static const char *TAG = "widget";

#define BIT_IP (1 << 0)
static EventGroupHandle_t s_eg;
static char s_ip[16] = "---";
static volatile bool s_wifi_ok = false;
static volatile uint8_t s_wifi_retry_count = 0;
static esp_timer_handle_t s_wifi_retry_timer;
static bool s_night_dim = false;
static lv_timer_t *s_roll_timer;
static data_trust_t s_data_trust = APP_DEMO_MODE ? DATA_TRUST_DEMO : DATA_TRUST_WARMING;
static void refresh_source_label(void);

// ── 酷态科设计令牌：黑底 + 高饱和功能色，颜色只用于编码 ──
#define CLR_BG       0x000000  // 纯黑底(OLED 省电)
#define CLR_PANEL    0x000000  // 不再使用面板底色
#define CLR_BORDER   0x1C222B  // 1px 分隔线
#define CLR_GRID     0x000000  // 曲线区禁用网格线
#define CLR_TEXT     0xF5F7FA  // 主文字白
#define CLR_DIM      0x8A93A3  // 次文字灰
#define CLR_GREEN    0x36D27E  // 状态:在线
#define CLR_DOWN     0x37C4D6  // 数据青 = 下行
#define CLR_UP       0x3FA9F5  // 品牌蓝 = 上行
#define CLR_PING     0xFF7A1A  // 能量橙 = PING/警示
#define CLR_YELLOW   0xFFC800  // 强调黄
#define CLR_RED      0xFF4057  // 状态:离线

#define CURVE_W 298
#define CURVE_H 45
// 全屏曲线页(模式 D)
#define CURVE2_W 300
#define CURVE2_H 108
#define DUAL_CURVE_W 132
#define DUAL_CURVE_H 28
#define CURVE_FPS 15
#define CURVE_SMOOTH_SEC 0.28f
#define ACTIVE_BPS (48 * 1024)

#define RGB565(r,g,b) (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))

// ─── Wi-Fi ───────────────────────────────────────────────────────────

#if !APP_DEMO_MODE
static void wifi_retry_cb(void *arg) {
    (void)arg;
    if (!s_wifi_ok) esp_wifi_connect();
}

static void wifi_schedule_retry(void) {
    if (!s_wifi_retry_timer) return;
    uint8_t attempt = s_wifi_retry_count > 5 ? 5 : s_wifi_retry_count;
    uint64_t delay_ms = 1000ULL << attempt;
    if (delay_ms > 30000) delay_ms = 30000;
    esp_timer_stop(s_wifi_retry_timer);
    esp_timer_start_once(s_wifi_retry_timer, delay_ms * 1000ULL);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_ok = false;
        s_wifi_retry_count = s_wifi_retry_count < 15 ? s_wifi_retry_count + 1 : 15;
        strncpy(s_ip, "---", sizeof(s_ip));
        xEventGroupClearBits(s_eg, BIT_IP);
        ikuai_monitor_network_changed();
        wifi_schedule_retry();
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "wifi disconnected reason=%d retry=%u",
                 d ? d->reason : 0, (unsigned)s_wifi_retry_count);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        esp_ip4addr_ntoa(&e->ip_info.ip, s_ip, sizeof(s_ip));
        s_wifi_ok = true;
        s_wifi_retry_count = 0;
        if (s_wifi_retry_timer) esp_timer_stop(s_wifi_retry_timer);
        xEventGroupSetBits(s_eg, BIT_IP);
        ESP_LOGI(TAG, "got ip %s", s_ip);
        ikuai_monitor_network_changed();
        if (!esp_sntp_enabled()) {          // 网络授时,用于夜间自动降背光
            setenv("TZ", APP_TZ, 1);
            tzset();
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "ntp.aliyun.com");
            esp_sntp_init();
        }
    }
}

static void wifi_start(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wc));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL));
    const esp_timer_create_args_t retry_args = {
        .callback = wifi_retry_cb,
        .name = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&retry_args, &s_wifi_retry_timer));
    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, APP_WIFI_SSID, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, APP_WIFI_PASS, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}
#endif

// ═══════════════════ 流动曲线引擎 ═════════════════════════════════════

static lv_obj_t *canvas_curve;
LV_DRAW_BUF_DEFINE_STATIC(cvs_curve, CURVE_W, CURVE_H, LV_COLOR_FORMAT_RGB565);
static lv_obj_t *canvas_curve2;
LV_DRAW_BUF_DEFINE_STATIC(cvs_curve2, CURVE2_W, CURVE2_H, LV_COLOR_FORMAT_RGB565);
static lv_obj_t *canvas_dual_down, *canvas_dual_up;
LV_DRAW_BUF_DEFINE_STATIC(cvs_dual_down, DUAL_CURVE_W, DUAL_CURVE_H, LV_COLOR_FORMAT_RGB565);
LV_DRAW_BUF_DEFINE_STATIC(cvs_dual_up, DUAL_CURVE_W, DUAL_CURVE_H, LV_COLOR_FORMAT_RGB565);
static void meteor_step(void);

typedef struct {
    float cur;
    float target;
    float velocity;
    uint16_t color;
    uint16_t dim;
    uint16_t fill;   // 渐变填充色(仅主轨道使用)
} curve_track_t;

enum { TRACK_DOWN, TRACK_UP, TRACK_PING, TRACK_COUNT };
static curve_track_t s_track[TRACK_COUNT];
static uint32_t s_peak_down = 1024;
static uint32_t s_peak_up = 1024;
static float s_peak_ping = 50.0f;
static uint16_t s_c_bg;
static uint16_t s_c_grid;
static int s_last_y[TRACK_COUNT] = { CURVE_H - 3, CURVE_H - 3, CURVE_H - 3 };
static int s_last_y2[TRACK_COUNT] = { CURVE2_H - 3, CURVE2_H - 3, CURVE2_H - 3 };
static int s_last_dual_y[2] = { DUAL_CURVE_H - 3, DUAL_CURVE_H - 3 };
static uint32_t s_curve_frames;

static uint16_t mix565(uint16_t bg, uint16_t fg, uint8_t a) {
    if (a == 0) return bg;
    if (a >= 254) return fg;
    int r = (((fg >> 11) & 31) * a + ((bg >> 11) & 31) * (255 - a)) / 255;
    int g = (((fg >> 5) & 63) * a + ((bg >> 5) & 63) * (255 - a)) / 255;
    int b = ((fg & 31) * a + (bg & 31) * (255 - a)) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void curve_clear(void) {
    uint16_t *buf = (uint16_t *)cvs_curve.data;
    for (int y = 0; y < CURVE_H; y++) {
        for (int x = 0; x < CURVE_W; x++) buf[y * CURVE_W + x] = s_c_bg;
    }
}

static int curve_y_h(float value, int h) {
    if (value < 0) value = 0;
    if (value > 1) value = 1;
    return (int)((1.0f - value) * (h - 5) + 2.5f);
}
static int curve_y(float value) { return curve_y_h(value, CURVE_H); }

static void curve_clear_column(uint16_t *buf, int x) {
    for (int y = 0; y < CURVE_H; y++) {
        buf[y * CURVE_W + x] = s_c_bg;
    }
}

static void curve_draw_track(uint16_t *buf, int x, int index, const curve_track_t *track) {
    int y = curve_y(track->cur);
    int y0 = s_last_y[index];
    int lo = y < y0 ? y : y0;
    int hi = y > y0 ? y : y0;
    for (int yy = lo; yy <= hi; yy++) buf[yy * CURVE_W + x] = track->dim;
    buf[y * CURVE_W + x] = track->color;
    if (y + 1 < CURVE_H) buf[(y + 1) * CURVE_W + x] = track->color;
    if (x > 0) buf[y * CURVE_W + x - 1] = track->dim;
    s_last_y[index] = y;
}

// 30号站同款 sparkline:亮线 + 向下渐变填充(仅下行主轨道)
static void curve_draw_track_fill(uint16_t *buf, int x, int index, const curve_track_t *track) {
    curve_draw_track(buf, x, index, track);
    int y = curve_y(track->cur);
    for (int yy = y + 2; yy < CURVE_H; yy++) {
        int depth = yy - y;                        // 越深越淡
        uint16_t px = buf[yy * CURVE_W + x];
        if (px == s_c_bg)                          // 不覆盖其他轨道亮线
            buf[yy * CURVE_W + x] = depth < 8 ? track->fill : mix565(s_c_bg, track->fill, 90);
    }
}

// 临界阻尼平滑：位置和速度都连续，目标每秒跳变时也不会出现折线硬拐角。
static void curve_smooth_step(curve_track_t *track) {
    const float dt = 1.0f / CURVE_FPS;
    const float omega = 2.0f / CURVE_SMOOTH_SEC;
    const float x = omega * dt;
    const float decay = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    const float current = track->cur;
    const float change = current - track->target;
    const float temp = (track->velocity + omega * change) * dt;

    track->velocity = (track->velocity - omega * temp) * decay;
    track->cur = track->target + (change + temp) * decay;

    // 目标反向变化时禁止越过目标，避免视觉上的虚假过冲。
    if (((track->target - current) > 0.0f) == (track->cur > track->target)) {
        track->cur = track->target;
        track->velocity = 0.0f;
    }
    if (track->cur < 0.0f) {
        track->cur = 0.0f;
        track->velocity = 0.0f;
    } else if (track->cur > 1.0f) {
        track->cur = 1.0f;
        track->velocity = 0.0f;
    }
}

static void dual_curve_roll(lv_draw_buf_t *draw_buf, lv_obj_t *canvas,
                            int lane, const curve_track_t *track) {
    uint16_t *buf = (uint16_t *)draw_buf->data;
    for (int y = 0; y < DUAL_CURVE_H; y++) {
        uint16_t *row = buf + y * DUAL_CURVE_W;
        memmove(row, row + 1, (DUAL_CURVE_W - 1) * sizeof(uint16_t));
        row[DUAL_CURVE_W - 1] = s_c_bg;
    }

    int y = curve_y_h(track->cur, DUAL_CURVE_H);
    int y0 = s_last_dual_y[lane];
    int lo = y < y0 ? y : y0;
    int hi = y > y0 ? y : y0;
    for (int yy = lo; yy <= hi; yy++) {
        buf[yy * DUAL_CURVE_W + DUAL_CURVE_W - 1] = track->dim;
    }
    buf[y * DUAL_CURVE_W + DUAL_CURVE_W - 1] = track->color;
    if (y + 1 < DUAL_CURVE_H) {
        buf[(y + 1) * DUAL_CURVE_W + DUAL_CURVE_W - 1] = track->color;
    }
    s_last_dual_y[lane] = y;
    if (canvas && lv_obj_is_visible(canvas)) lv_obj_invalidate(canvas);
}

// 15fps 固定推进 1px：298px 宽对应约 19.9 秒，降低小屏刷新负载并保持连续趋势。
static void curve_roll(void) {
    uint16_t *buf = (uint16_t *)cvs_curve.data;
    for (int i = 0; i < TRACK_COUNT; i++) {
        curve_smooth_step(&s_track[i]);
    }

    // Only mutate and invalidate the curve that is currently on screen.
    // Hidden pages do not need 30 FPS memory copies and cannot leave stale
    // pixels because show_page() invalidates the full display.
    if (lv_obj_is_visible(canvas_curve)) {
        for (int y = 0; y < CURVE_H; y++) {
            uint16_t *row = buf + y * CURVE_W;
            memmove(row, row + 1, (CURVE_W - 1) * sizeof(uint16_t));
        }
        curve_clear_column(buf, CURVE_W - 1);

        for (int i = 0; i < TRACK_COUNT; i++) {
            if (i == TRACK_DOWN) curve_draw_track_fill(buf, CURVE_W - 1, i, &s_track[i]);
            else                 curve_draw_track(buf, CURVE_W - 1, i, &s_track[i]);
        }
        lv_obj_invalidate(canvas_curve);
    }

    // 全屏曲线页同步滚动(同款黑底无网格)
    if (lv_obj_is_visible(canvas_curve2)) {
        uint16_t *buf2 = (uint16_t *)cvs_curve2.data;
        for (int y = 0; y < CURVE2_H; y++) {
            uint16_t *row = buf2 + y * CURVE2_W;
            memmove(row, row + 1, (CURVE2_W - 1) * sizeof(uint16_t));
        }
        for (int y = 0; y < CURVE2_H; y++) buf2[y * CURVE2_W + CURVE2_W - 1] = s_c_bg;
        for (int i = 0; i < TRACK_COUNT; i++) {
            int y = curve_y_h(s_track[i].cur, CURVE2_H);
            int y0 = s_last_y2[i];
            int lo = y < y0 ? y : y0, hi = y > y0 ? y : y0;
            for (int yy = lo; yy <= hi; yy++) buf2[yy * CURVE2_W + CURVE2_W - 1] = s_track[i].dim;
            buf2[y * CURVE2_W + CURVE2_W - 1] = s_track[i].color;
            if (y + 1 < CURVE2_H) buf2[(y + 1) * CURVE2_W + CURVE2_W - 1] = s_track[i].color;
            s_last_y2[i] = y;
        }
        lv_obj_invalidate(canvas_curve2);
    }

    if (lv_obj_is_visible(canvas_dual_down))
        dual_curve_roll(&cvs_dual_down, canvas_dual_down, 0, &s_track[TRACK_DOWN]);
    if (lv_obj_is_visible(canvas_dual_up))
        dual_curve_roll(&cvs_dual_up, canvas_dual_up, 1, &s_track[TRACK_UP]);
    s_curve_frames++;
    meteor_step();
}

// ═══════════════════ UI ══════════════════════════════════════════════

static lv_obj_t *lbl_status, *lbl_online, *lbl_down, *lbl_up;
static lv_obj_t *lbl_down_unit, *lbl_up_unit, *lbl_ping, *lbl_ip;
static lv_obj_t *lbl_ping_bg;
static lv_obj_t *dot_status, *lbl_source;
static lv_obj_t *lbl_main_curve_state, *lbl_curve_state;
static lv_obj_t *lbl_curve_down, *lbl_curve_up, *lbl_curve_ping;

typedef enum {
    RATE_UNIT_B,
    RATE_UNIT_KB,
    RATE_UNIT_MB,
    RATE_UNIT_GB,
    RATE_UNIT_AUTO,
} rate_unit_t;

static rate_unit_t s_down_unit = RATE_UNIT_AUTO;
static rate_unit_t s_up_unit = RATE_UNIT_AUTO;
static rate_unit_t s_peak_unit = RATE_UNIT_AUTO;
static rate_unit_t s_client_unit[3] = { RATE_UNIT_AUTO, RATE_UNIT_AUTO, RATE_UNIT_AUTO };

static rate_unit_t rate_unit_initial(uint32_t bytes_per_sec) {
    if (bytes_per_sec >= 1024U * 1024U * 1024U) return RATE_UNIT_GB;
    if (bytes_per_sec >= 1024U * 1024U) return RATE_UNIT_MB;
    if (bytes_per_sec >= 1024U) return RATE_UNIT_KB;
    return RATE_UNIT_B;
}

static void fmt_rate(uint32_t bytes_per_sec, rate_unit_t *state,
                     char *value, int value_cap, const char **unit) {
    static const uint32_t up_threshold[] = {
        1280U,
        1152U * 1024U,
        1152U * 1024U * 1024U,
    };
    static const uint32_t down_threshold[] = {
        0U,
        768U,
        768U * 1024U,
        768U * 1024U * 1024U,
    };

    rate_unit_t selected = *state;
    if (selected == RATE_UNIT_AUTO) selected = rate_unit_initial(bytes_per_sec);
    while (selected < RATE_UNIT_GB && bytes_per_sec >= up_threshold[selected]) selected++;
    while (selected > RATE_UNIT_B && bytes_per_sec < down_threshold[selected]) selected--;
    *state = selected;

    float divisor = 1.0f;
    switch (selected) {
        case RATE_UNIT_GB: divisor = 1024.0f * 1024.0f * 1024.0f; *unit = "GB/s"; break;
        case RATE_UNIT_MB: divisor = 1024.0f * 1024.0f; *unit = "MB/s"; break;
        case RATE_UNIT_KB: divisor = 1024.0f; *unit = "KB/s"; break;
        default: *unit = "B/s"; break;
    }

    if (selected == RATE_UNIT_B) {
        snprintf(value, value_cap, "%u", (unsigned)bytes_per_sec);
    } else {
        float scaled = bytes_per_sec / divisor;
        snprintf(value, value_cap, scaled >= 100.0f ? "%.0f" : "%.1f", scaled);
    }
}

static lv_obj_t *mk_label(lv_obj_t *parent, int x, int y, const lv_font_t *f, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_letter_space(l, 0, 0);
    lv_label_set_text(l, "");
    return l;
}

// Keep live metrics inside their columns. LVGL labels otherwise grow to the
// width of their text and can cover the next metric when a rate becomes large.
static void constrain_metric_label(lv_obj_t *label, int width) {
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
}

// Dynamic numeric values change width as they move from -- to 0..100.
// Re-anchor the suffix after every value update so %, units, and values stay
// visually grouped instead of relying on the initial placeholder width.
static void align_value_suffix(lv_obj_t *value, lv_obj_t *suffix) {
    if (value && suffix) {
        lv_obj_align_to(suffix, value, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -6);
    }
}

// 胶囊标签:实色功能底 + 全大写黑字,与数字形成"大数字+小标签"咬合
static lv_obj_t *mk_pill(lv_obj_t *parent, int x, int y, const char *text, uint32_t bg, uint32_t fg) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, LV_SIZE_CONTENT, 18);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(o, 7, 0);
    lv_obj_set_style_pad_ver(o, 2, 0);
    lv_obj_t *l = lv_label_create(o);
    lv_obj_set_style_text_font(l, UI_FONT_META, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
    lv_obj_set_style_text_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(l, LV_OPA_COVER, 0);
    lv_label_set_text(l, text);
    return o;
}

static lv_obj_t *mk_block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}


// ═══ 焦点页流星粒子(模式 C 完全体):高速率时流星从两侧向数字汇入 ═══
#define METEOR_W 296
#define METEOR_H 40        // 覆盖大数字垂直中心区(数字视觉中心 y≈66)
#define METEOR_N 6         // 短暂反馈，不持续铺满焦点页
static lv_obj_t *canvas_meteor;
LV_DRAW_BUF_DEFINE_STATIC(cvs_meteor, METEOR_W, METEOR_H, LV_COLOR_FORMAT_RGB565);
static struct { float x, y, vx, vy; } s_mt[METEOR_N];
static bool s_meteor_on = false;
static bool s_last_active;   // 先行声明,定义在下方 ui_update 区
static uint32_t s_meteor_until_ms = 0;
static uint32_t s_meteor_last_bps = 0;
static uint32_t s_meteor_last_trigger_ms = 0;
// No page has been committed until ui_create() selects the first page.
// This makes the initial show_page(0, ...) pass hide every other page.
static int s_page = -1;

static void meteor_reset(int i) {
    // 从左右边缘随机位置出发,汇聚到大数字视觉中心
    s_mt[i].x = (i & 1) ? -4.0f : (float)(METEOR_W + 4);
    s_mt[i].y = (float)(4 + (i * 7919) % (METEOR_H - 8));
    float tx = METEOR_W * 31.0f / 100.0f, ty = METEOR_H / 2.0f;  // 汇聚点=大数字视觉中心
    float dx = tx - s_mt[i].x, dy = ty - s_mt[i].y;
    float sp = 2.2f + (float)((i * 31) % 17) / 10.0f;   // 2.2~3.8 px/帧
    s_mt[i].vx = dx * sp / 150.0f;
    s_mt[i].vy = dy * sp / 150.0f;
}

static void meteor_step(void) {
    uint16_t *buf = (uint16_t *)cvs_meteor.data;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    bool want = !APP_REDUCED_MOTION && !s_night_dim && s_page == 1 && s_last_active &&
                (int32_t)(s_meteor_until_ms - now) > 0;
    if (want != s_meteor_on) {
        s_meteor_on = want;
        if (!want) { for (int i = 0; i < METEOR_W * METEOR_H; i++) buf[i] = s_c_bg; lv_obj_invalidate(canvas_meteor); return; }
        for (int i = 0; i < METEOR_N; i++) meteor_reset(i);
    }
    if (!want) return;
    for (int i = 0; i < METEOR_W * METEOR_H; i++) buf[i] = s_c_bg;
    uint16_t c1 = s_track[TRACK_DOWN].color, c2 = s_track[TRACK_UP].color;
    uint16_t t1 = mix565(s_c_bg, c1, 110), t2 = mix565(s_c_bg, c1, 45);
    for (int i = 0; i < METEOR_N; i++) {
        s_mt[i].x += s_mt[i].vx; s_mt[i].y += s_mt[i].vy;
        int x = (int)s_mt[i].x, y = (int)s_mt[i].y;
        // 到达中心后消散重生
        if (x > (int)(METEOR_W * 31 / 100) - 6 && x < (int)(METEOR_W * 31 / 100) + 6) { meteor_reset(i); continue; }
        if (x >= 0 && x < METEOR_W && y >= 0 && y < METEOR_H) {
            buf[y * METEOR_W + x] = (i % 3) ? c1 : c2;                 // 头部亮点
            int tx = x - (int)(s_mt[i].vx * 2.0f);                     // 拖尾
            if (tx >= 0 && tx < METEOR_W) buf[y * METEOR_W + tx] = t1;
            tx = x - (int)(s_mt[i].vx * 4.0f);
            if (tx >= 0 && tx < METEOR_W) buf[y * METEOR_W + tx] = t2;
        }
    }
    lv_obj_invalidate(canvas_meteor);
}

static void meteor_note_rate(uint32_t bps) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    bool crossed_active = s_meteor_last_bps < ACTIVE_BPS && bps >= ACTIVE_BPS;
    uint32_t base = s_meteor_last_bps > ACTIVE_BPS ? s_meteor_last_bps : ACTIVE_BPS;
    uint32_t delta = bps > s_meteor_last_bps ? bps - s_meteor_last_bps : s_meteor_last_bps - bps;
    if (!APP_REDUCED_MOTION && bps >= ACTIVE_BPS &&
        (crossed_active || delta > base / 2) &&
        (uint32_t)(now - s_meteor_last_trigger_ms) >= 1500) {
        s_meteor_until_ms = now + 700;
        s_meteor_last_trigger_ms = now;
    }
    s_meteor_last_bps = bps;
}


// ═══════════════════ 十页系统(GPIO0 上一页 / GPIO14 下一页) ═══════════════════════
// 0 总览 / 1 下行焦点 / 2 双通道 / 3 趋势 / 4 网络健康
// 5 设备状态 / 6 路由健康 / 7 WAN / 8 客户端排行 / 9 无线 AC
#define PAGE_COUNT 10
#define PIN_BUTTON_PREV GPIO_NUM_0
#define PIN_BUTTON_NEXT GPIO_NUM_14
#define BUTTON_DEBOUNCE_MS 30

static lv_obj_t *s_pg[PAGE_COUNT];
static lv_obj_t *s_dots[PAGE_COUNT];
static uint32_t s_last_key_ms = 0;
static uint32_t s_dot_feedback_until_ms = 0;

typedef struct {
    gpio_num_t pin;
    int stable;
    int last_raw;
    uint32_t changed_ms;
} page_button_t;

static page_button_t s_btn_prev = { .pin = PIN_BUTTON_PREV, .stable = 1, .last_raw = 1 };
static page_button_t s_btn_next = { .pin = PIN_BUTTON_NEXT, .stable = 1, .last_raw = 1 };

// 焦点页元素
static lv_obj_t *lbl_focus_num, *lbl_focus_unit, *lbl_focus_sub, *lbl_focus_peak;
// 双通道页元素
static lv_obj_t *lbl_dual_down, *lbl_dual_down_unit, *lbl_dual_up, *lbl_dual_up_unit;
static lv_obj_t *lbl_dual_ping, *lbl_dual_ip, *lbl_dual_state;
// 网络健康页元素
static lv_obj_t *lbl_net_status, *lbl_net_detail, *lbl_net_ping;
static lv_obj_t *lbl_net_down, *lbl_net_up, *lbl_net_footer;
// 系统页元素
static lv_obj_t *lbl_sys_ip, *lbl_sys_clients, *lbl_sys_uptime, *lbl_sys_heap;
// 健康页/WAN页/排行页/AC页元素
static lv_obj_t *lbl_health_status, *lbl_cpu, *lbl_mem, *lbl_temp, *lbl_uptime, *lbl_health_meta;
static lv_obj_t *lbl_cpu_unit, *lbl_mem_unit;
static lv_obj_t *lbl_wan[2][3];   // [i][0=状态 1=IP 2=网关]
static lv_obj_t *lbl_wan_dot[2];
static lv_obj_t *lbl_cli[3][2];   // [i][0=名称 1=速率]
static lv_obj_t *lbl_top_count, *lbl_top_total;
static lv_obj_t *lbl_ac, *lbl_ap, *lbl_clt_2g, *lbl_clt_5g;

static void show_page(int idx, int dir) {
    if (idx < 0 || idx >= PAGE_COUNT || idx == s_page) return;
    (void)dir;  // 页面切换采用原子换页，方向只保留给按键语义和调用方。

    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_add_flag(s_pg[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_translate_x(s_pg[i], 0, 0);
        // 页码指示点:当前页=强调黄,其余=灰(颜色即编码)
        lv_obj_set_style_bg_color(s_dots[i],
            lv_color_hex(i == idx ? CLR_YELLOW : 0x3A4150), 0);
    }

    // Partial-buffer LCD cannot safely retain two full-page compositions while
    // translating them. Hide every old page first, then expose the target at
    // zero offset so the next full-screen invalidation has a single stable
    // composition and cannot leave a previous-page ghost behind.
    lv_obj_remove_flag(s_pg[idx], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_translate_x(s_pg[idx], 0, 0);
    s_page = idx;
    // Partial buffers need a full-screen redraw after page composition changes.
    lv_obj_invalidate(lv_scr_act());
}

static void button_feedback(void) {
    if (s_page >= 0 && s_page < PAGE_COUNT) {
        lv_obj_set_style_bg_color(s_dots[s_page], lv_color_hex(CLR_TEXT), 0);
        s_dot_feedback_until_ms = (uint32_t)(esp_timer_get_time() / 1000) + 220;
    }
}

static lv_obj_t *mk_page(lv_obj_t *scr) {
    lv_obj_t *pg = lv_obj_create(scr);
    lv_obj_remove_style_all(pg);
    lv_obj_set_pos(pg, 0, 0);
    lv_obj_set_size(pg, LCD_W, LCD_H);
    return pg;
}


// 页面 1:下行焦点 —— 一屏一个问题：此刻下载有多快？
static void build_page_focus(lv_obj_t *pg) {
    mk_pill(pg, 12, 12, "TOTAL DOWN", CLR_DOWN, 0x000000);

    canvas_meteor = lv_canvas_create(pg);
    lv_obj_set_pos(canvas_meteor, 12, 44);
    lv_canvas_set_draw_buf(canvas_meteor, &cvs_meteor);

    lbl_focus_num = mk_label(pg, 12, 42, UI_FONT_METRIC, CLR_TEXT);
    lv_obj_set_width(lbl_focus_num, 296);
    lv_obj_set_style_text_align(lbl_focus_num, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(lbl_focus_num, 2, 0);
    lv_label_set_text(lbl_focus_num, "--");
    lbl_focus_unit = mk_label(pg, 12, 90, UI_FONT_META, CLR_DOWN);
    lv_obj_set_width(lbl_focus_unit, 296);
    lv_obj_set_style_text_align(lbl_focus_unit, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl_focus_unit, "MB/s");
    // 关键主值使用项目 2bpp 原生数字字体，避免 4bpp 灰边与运行时缩放。
    lbl_focus_sub = mk_label(pg, 12, 122, UI_FONT_META, CLR_DIM);
    lv_label_set_text(lbl_focus_sub, "UP -- MB/s");
    mk_block(pg, 12, 112, 296, 1, CLR_BORDER);
    lbl_focus_peak = mk_label(pg, 0, 122, UI_FONT_META, CLR_YELLOW);
    lv_label_set_text(lbl_focus_peak, "");
    lv_obj_align(lbl_focus_peak, LV_ALIGN_TOP_RIGHT, -12, 122);
}

// 页面 2:双通道 —— 同时回答下行和上行，微型曲线保留方向感。
static void build_page_dual(lv_obj_t *pg) {
    lbl_dual_state = mk_label(pg, 12, 3, UI_FONT_META, CLR_GREEN);
    lv_label_set_text(lbl_dual_state, "WAN ONLINE");
    lbl_dual_ping = mk_label(pg, 0, 3, UI_FONT_META, CLR_PING);
    lv_obj_align(lbl_dual_ping, LV_ALIGN_TOP_RIGHT, -12, 3);
    lv_label_set_text(lbl_dual_ping, "GW -- MS");
    mk_block(pg, 12, 22, 296, 1, CLR_BORDER);
    mk_block(pg, 159, 32, 1, 104, CLR_BORDER);

    mk_pill(pg, 12, 30, "DOWN", CLR_DOWN, 0x000000);
    mk_pill(pg, 172, 30, "UP", CLR_UP, 0x000000);
    lbl_dual_down = mk_label(pg, 12, 50, UI_FONT_METRIC, CLR_TEXT);
    lbl_dual_up = mk_label(pg, 172, 50, UI_FONT_METRIC, CLR_TEXT);
    lv_label_set_text(lbl_dual_down, "--");
    lv_label_set_text(lbl_dual_up, "--");
    lbl_dual_down_unit = mk_label(pg, 12, 90, UI_FONT_META, CLR_DOWN);
    lbl_dual_up_unit = mk_label(pg, 172, 90, UI_FONT_META, CLR_UP);
    lv_label_set_text(lbl_dual_down_unit, "MB/s");
    lv_label_set_text(lbl_dual_up_unit, "MB/s");

    canvas_dual_down = lv_canvas_create(pg);
    lv_obj_set_pos(canvas_dual_down, 12, 105);
    lv_canvas_set_draw_buf(canvas_dual_down, &cvs_dual_down);
    canvas_dual_up = lv_canvas_create(pg);
    lv_obj_set_pos(canvas_dual_up, 176, 105);
    lv_canvas_set_draw_buf(canvas_dual_up, &cvs_dual_up);

    lbl_dual_ip = mk_label(pg, 12, 145, UI_FONT_META, CLR_DIM);
    lv_label_set_text(lbl_dual_ip, "IP ---");
}

// 页面 3:趋势优先 —— 无坐标轴无网格，数值负责解释曲线。
static void build_page_curve(lv_obj_t *pg) {
    // Three fixed columns leave a visible gap between live values. The short
    // prefixes preserve the meaning while keeping large rates readable.
    lbl_curve_down = mk_label(pg, 12, 1, UI_FONT_META, CLR_DOWN);
    constrain_metric_label(lbl_curve_down, 96);
    lv_label_set_text(lbl_curve_down, "D --");
    lbl_curve_up = mk_label(pg, 112, 1, UI_FONT_META, CLR_UP);
    constrain_metric_label(lbl_curve_up, 96);
    lv_label_set_text(lbl_curve_up, "U --");
    lbl_curve_ping = mk_label(pg, 0, 1, UI_FONT_META, CLR_PING);
    constrain_metric_label(lbl_curve_ping, 88);
    lv_obj_align(lbl_curve_ping, LV_ALIGN_TOP_RIGHT, -12, 1);
    lv_obj_set_style_text_align(lbl_curve_ping, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(lbl_curve_ping, "GW --");
    mk_block(pg, 12, 19, 296, 1, CLR_BORDER);

    canvas_curve2 = lv_canvas_create(pg);
    lv_obj_set_pos(canvas_curve2, 12, 26);
    lv_canvas_set_draw_buf(canvas_curve2, &cvs_curve2);

    lbl_curve_state = mk_label(pg, 12, 140, UI_FONT_META, CLR_DIM);
    lv_label_set_text(lbl_curve_state, "10S LIVE");
    lv_obj_t *legend = mk_label(pg, 0, 140, UI_FONT_META, CLR_DIM);
    lv_obj_align(legend, LV_ALIGN_TOP_RIGHT, -12, 140);
    lv_label_set_text(legend, "D / U / P");
}

// 页面 4:网络健康 —— 先给结论，再给 PING/上下行三项证据。
static void build_page_network(lv_obj_t *pg) {
    lv_obj_t *title = mk_label(pg, 12, 3, UI_FONT_TITLE, CLR_DIM);
    lv_label_set_text(title, "NETWORK HEALTH");
    // The italic 36px font is numeric-only; use the ASCII label font for
    // ONLINE/OFFLINE so LVGL never falls back to missing-glyph squares.
    lbl_net_status = mk_label(pg, 12, 29, UI_FONT_TITLE, CLR_GREEN);
    lv_label_set_text(lbl_net_status, "WAN ONLINE");
    lbl_net_detail = mk_label(pg, 12, 60, UI_FONT_TITLE, CLR_GREEN);
    lv_label_set_text(lbl_net_detail, "NETWORK STABLE");
    mk_block(pg, 12, 88, 296, 1, CLR_BORDER);

    lbl_net_ping = mk_label(pg, 12, 101, UI_FONT_META, CLR_PING);
    lbl_net_down = mk_label(pg, 112, 101, UI_FONT_META, CLR_DOWN);
    lbl_net_up = mk_label(pg, 220, 101, UI_FONT_META, CLR_UP);
    constrain_metric_label(lbl_net_ping, 88);
    constrain_metric_label(lbl_net_down, 96);
    constrain_metric_label(lbl_net_up, 88);
    lv_obj_set_style_text_align(lbl_net_up, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(lbl_net_ping, "GW --");
    lv_label_set_text(lbl_net_down, "D --");
    lv_label_set_text(lbl_net_up, "U --");
    lbl_net_footer = mk_label(pg, 12, 132, UI_FONT_META, CLR_DIM);
    lv_label_set_text(lbl_net_footer, "NO ACTIVE ALERTS");
}

// 页面 5:设备状态页 —— 本机 IP、客户端、运行时间和堆内存。
static void sys_row(lv_obj_t *pg, int y, const char *name, uint32_t pill_clr, lv_obj_t **val) {
    mk_pill(pg, 12, y, name, pill_clr, 0x000000);
    *val = mk_label(pg, 0, y + 2, UI_FONT_META, CLR_TEXT);
    lv_obj_align(*val, LV_ALIGN_TOP_RIGHT, -12, y + 2);
    lv_label_set_text(*val, "--");
}

static void build_page_sys(lv_obj_t *pg) {
    sys_row(pg, 8,  "IP",      CLR_UP,     &lbl_sys_ip);
    sys_row(pg, 34, "CLIENTS", CLR_DOWN,   &lbl_sys_clients);
    sys_row(pg, 60, "DEVICE UP", CLR_YELLOW, &lbl_sys_uptime);
    sys_row(pg, 86, "HEAP",    CLR_PING,   &lbl_sys_heap);
    mk_block(pg, 12, 112, 296, 1, CLR_BORDER);
    lv_obj_t *hint = mk_label(pg, 12, 130, UI_FONT_META, CLR_DIM);
    lv_label_set_text(hint, "BOOT PREV / IO14 NEXT");
}


// 页面 6:路由健康页 —— CPU/MEM 双大数字 + 核心元信息。
static void build_page_health(lv_obj_t *pg) {
    lv_obj_t *title = mk_label(pg, 12, 2, UI_FONT_TITLE, CLR_DIM);
    lv_label_set_text(title, "ROUTER HEALTH");
    lbl_health_status = mk_label(pg, 0, 5, UI_FONT_META, CLR_GREEN);
    lv_obj_align(lbl_health_status, LV_ALIGN_TOP_RIGHT, -12, 5);
    lv_label_set_text(lbl_health_status, "ONLINE");
    mk_block(pg, 12, 22, 296, 1, CLR_BORDER);

    mk_pill(pg, 12, 31, "CPU", CLR_PING, 0x000000);
    mk_pill(pg, 172, 31, "MEM", CLR_UP, 0x000000);
    lbl_cpu = mk_label(pg, 12, 47, UI_FONT_METRIC, CLR_TEXT);
    lv_label_set_text(lbl_cpu, "--");
    lbl_mem = mk_label(pg, 172, 47, UI_FONT_METRIC, CLR_TEXT);
    lv_label_set_text(lbl_mem, "--");
    lbl_cpu_unit = mk_label(pg, 12, 71, UI_FONT_META, CLR_PING);
    lv_label_set_text(lbl_cpu_unit, "%");
    align_value_suffix(lbl_cpu, lbl_cpu_unit);
    lbl_mem_unit = mk_label(pg, 172, 71, UI_FONT_META, CLR_UP);
    lv_label_set_text(lbl_mem_unit, "%");
    align_value_suffix(lbl_mem, lbl_mem_unit);
    mk_block(pg, 12, 91, 296, 1, CLR_BORDER);
    mk_pill(pg, 12, 100, "TEMP", CLR_YELLOW, 0x000000);
    lbl_temp = mk_label(pg, 92, 102, UI_FONT_META, CLR_TEXT);
    lv_label_set_text(lbl_temp, "--");
    mk_pill(pg, 172, 100, "UPTIME", CLR_DOWN, 0x000000);
    lbl_uptime = mk_label(pg, 244, 102, UI_FONT_META, CLR_TEXT);
    lv_label_set_text(lbl_uptime, "--");
    lbl_health_meta = mk_label(pg, 12, 130, UI_FONT_META, CLR_DIM);
    lv_label_set_text(lbl_health_meta, "CLIENTS -- | HEAP -- | VER --");
}

// 页面 7:WAN 线路详情页 —— 状态点 + 状态词 + 公网 IP + 网关。
static void build_page_wan(lv_obj_t *pg) {
    for (int i = 0; i < 2; i++) {
        int y = 12 + i * 74;
        char name[8];
        snprintf(name, sizeof(name), "WAN%d", i + 1);
        mk_pill(pg, 12, y, name, CLR_UP, 0x000000);
        lbl_wan_dot[i] = lv_obj_create(pg);
        lv_obj_remove_style_all(lbl_wan_dot[i]);
        lv_obj_set_style_bg_opa(lbl_wan_dot[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(lbl_wan_dot[i], 3, 0);
        lv_obj_set_pos(lbl_wan_dot[i], 302, y + 4);
        lv_obj_set_size(lbl_wan_dot[i], 6, 6);
        lbl_wan[i][0] = mk_label(pg, 0, y + 2, UI_FONT_META, CLR_GREEN);
        lv_obj_align(lbl_wan[i][0], LV_ALIGN_TOP_RIGHT, -24, y + 2);
        lv_label_set_text(lbl_wan[i][0], "OFFLINE");
        mk_pill(pg, 12, y + 22, "IP", CLR_DOWN, 0x000000);
        lbl_wan[i][1] = mk_label(pg, 0, y + 24, UI_FONT_META, CLR_TEXT);
        lv_obj_align(lbl_wan[i][1], LV_ALIGN_TOP_RIGHT, -12, y + 24);
        lv_label_set_text(lbl_wan[i][1], "--");
        mk_pill(pg, 12, y + 44, "GW", CLR_DIM, 0x000000);
        lbl_wan[i][2] = mk_label(pg, 0, y + 46, UI_FONT_META, CLR_DIM);
        lv_obj_align(lbl_wan[i][2], LV_ALIGN_TOP_RIGHT, -12, y + 46);
        lv_label_set_text(lbl_wan[i][2], "--");
        if (i == 0) mk_block(pg, 12, y + 64, 296, 1, CLR_BORDER);
    }
}

// 页面 8:终端流量排行页 —— Top3 下行 + 客户端和总流量摘要。
static void build_page_top(lv_obj_t *pg) {
    lv_obj_t *t = mk_label(pg, 12, 5, UI_FONT_META, CLR_DIM);
    lv_label_set_text(t, "TOP DOWNLOADERS");
    lbl_top_count = mk_label(pg, 0, 5, UI_FONT_META, CLR_DIM);
    lv_obj_align(lbl_top_count, LV_ALIGN_TOP_RIGHT, -12, 5);
    lv_label_set_text(lbl_top_count, "-- CLIENTS");
    for (int i = 0; i < 3; i++) {
        int y = 28 + i * 38;
        char rk[4];
        snprintf(rk, sizeof(rk), "#%d", i + 1);
        mk_pill(pg, 12, y, rk, i == 0 ? CLR_YELLOW : 0x3A4150, i == 0 ? 0x000000 : CLR_TEXT);
        lbl_cli[i][0] = mk_label(pg, 58, y + 2, UI_FONT_META, CLR_TEXT);
        lv_obj_set_width(lbl_cli[i][0], 150);
        lv_label_set_long_mode(lbl_cli[i][0], LV_LABEL_LONG_CLIP);
        lv_label_set_text(lbl_cli[i][0], "--");
        lbl_cli[i][1] = mk_label(pg, 0, y + 2, UI_FONT_META, CLR_DOWN);
        lv_obj_align(lbl_cli[i][1], LV_ALIGN_TOP_RIGHT, -12, y + 2);
        lv_label_set_text(lbl_cli[i][1], "--");
        if (i < 2) mk_block(pg, 12, y + 30, 296, 1, CLR_BORDER);
    }
    mk_block(pg, 12, 143, 296, 1, CLR_BORDER);
    lbl_top_total = mk_label(pg, 12, 149, UI_FONT_META, CLR_DIM);
    lv_label_set_text(lbl_top_total, "TOTAL DOWN -- | UP --");
}

// 页面 9:无线 AC 状态页。
static void build_page_ac(lv_obj_t *pg) {
    mk_pill(pg, 12, 12, "WIRELESS AC", CLR_UP, 0x000000);
    lbl_ac = mk_label(pg, 0, 14, UI_FONT_META, CLR_TEXT);
    lv_obj_align(lbl_ac, LV_ALIGN_TOP_RIGHT, -12, 14);
    lv_label_set_text(lbl_ac, "--");
    mk_block(pg, 12, 30, 296, 1, CLR_BORDER);
    mk_pill(pg, 12, 42, "AP ONLINE", CLR_DOWN, 0x000000);
    lbl_ap = mk_label(pg, 0, 44, UI_FONT_META, CLR_TEXT);
    lv_obj_align(lbl_ap, LV_ALIGN_TOP_RIGHT, -12, 44);
    lv_label_set_text(lbl_ap, "--");
    mk_pill(pg, 12, 72, "2.4G", CLR_YELLOW, 0x000000);
    mk_pill(pg, 188, 72, "5G", CLR_PING, 0x000000);
    lbl_clt_2g = mk_label(pg, 12, 90, UI_FONT_METRIC, CLR_TEXT);
    lbl_clt_5g = mk_label(pg, 188, 90, UI_FONT_METRIC, CLR_TEXT);
    lv_label_set_text(lbl_clt_2g, "--");
    lv_label_set_text(lbl_clt_5g, "--");
    mk_block(pg, 12, 140, 296, 1, CLR_BORDER);
    lv_obj_t *radio_hint = mk_label(pg, 12, 148, UI_FONT_META, CLR_DIM);
    lv_label_set_text(radio_hint, "CLIENTS BY RADIO");
}


// ── 夜间自动降背光:APP_NIGHT_START~APP_NIGHT_END 时段降到 APP_BL_NIGHT_PCT ──
// 依赖 SNTP;时间未同步(年<2024)时不动作。
static void night_dim_poll(void) {
    time_t t = time(NULL);
    struct tm tm_;
    localtime_r(&t, &tm_);
    if (tm_.tm_year + 1900 < 2024) return;  // 未授时
    bool night = (APP_NIGHT_START > APP_NIGHT_END)
        ? (tm_.tm_hour >= APP_NIGHT_START || tm_.tm_hour < APP_NIGHT_END)   // 跨零点
        : (tm_.tm_hour >= APP_NIGHT_START && tm_.tm_hour < APP_NIGHT_END);
    if (night != s_night_dim) {
        s_night_dim = night;
        lcd_set_backlight(night ? APP_BL_NIGHT_PCT : APP_BL_PCT);
        if (s_roll_timer) {
            lv_timer_set_period(s_roll_timer, night ? (1000 / 15) : (1000 / CURVE_FPS));
        }
        refresh_source_label();
    }
}

static bool button_pressed(page_button_t *button, uint32_t now) {
    int raw = gpio_get_level(button->pin);
    if (raw != button->last_raw) {
        button->last_raw = raw;
        button->changed_ms = now;
        return false;
    }
    if (raw == button->stable || now - button->changed_ms < BUTTON_DEBOUNCE_MS) return false;

    button->stable = raw;
    return raw == 0;
}

// 两个物理按键各自只负责一个方向，按下稳定 30ms 后立即翻页。
static void buttons_poll(void) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    bool prev = button_pressed(&s_btn_prev, now);
    bool next = button_pressed(&s_btn_next, now);
    if (prev == next) return;  // 无按键或同时按下时不改变页面。

    s_last_key_ms = now;
    if (prev) show_page((s_page + PAGE_COUNT - 1) % PAGE_COUNT, -1);
    else show_page((s_page + 1) % PAGE_COUNT, 1);
    button_feedback();
}

static void ui_create(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(CLR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    for (int i = 0; i < PAGE_COUNT; i++) s_pg[i] = mk_page(scr);
    lv_obj_t *pg0 = s_pg[0];

    dot_status = lv_obj_create(pg0);
    lv_obj_remove_style_all(dot_status);
    lv_obj_set_style_bg_color(dot_status,
        lv_color_hex(APP_DEMO_MODE ? CLR_GREEN : CLR_DIM), 0);
    lv_obj_set_style_bg_opa(dot_status, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dot_status, 3, 0);
    lv_obj_set_pos(dot_status, 12, 9);
    lv_obj_set_size(dot_status, 6, 6);

    lbl_status = mk_label(pg0, 23, 1, UI_FONT_META, CLR_TEXT);
    lv_label_set_text(lbl_status, APP_DEMO_MODE ? "DEMO ONLINE" : "WIFI CONNECTING");

    lbl_online = mk_label(pg0, 0, 1, UI_FONT_META, CLR_DIM);
    lv_label_set_text(lbl_online, "-- CLIENTS");
    lv_obj_align(lbl_online, LV_ALIGN_TOP_MID, 0, 1);

    // PING 元信息收进橙色描边胶囊(黑底+橙描边+橙字)
    lbl_ping_bg = lv_obj_create(pg0);
    lv_obj_remove_style_all(lbl_ping_bg);
    lv_obj_set_size(lbl_ping_bg, LV_SIZE_CONTENT, 18);
    lv_obj_align(lbl_ping_bg, LV_ALIGN_TOP_RIGHT, -12, 0);
    lv_obj_set_style_bg_opa(lbl_ping_bg, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(lbl_ping_bg, lv_color_hex(CLR_PING), 0);
    lv_obj_set_style_border_width(lbl_ping_bg, 1, 0);
    lv_obj_set_style_radius(lbl_ping_bg, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(lbl_ping_bg, 6, 0);
    lbl_ping = mk_label(lbl_ping_bg, 0, 0, UI_FONT_META, CLR_PING);
    lv_label_set_text(lbl_ping, "GW -- MS");
    lv_obj_center(lbl_ping);

    mk_pill(pg0, 12, 20, "DOWN", CLR_DOWN, 0x000000);
    mk_pill(pg0, 172, 20, "UP", CLR_UP, 0x000000);

    lbl_down = mk_label(pg0, 12, 38, UI_FONT_METRIC, CLR_TEXT);
    lbl_down_unit = mk_label(pg0, 116, 56, UI_FONT_META, CLR_DOWN);
    lbl_up = mk_label(pg0, 172, 38, UI_FONT_METRIC, CLR_TEXT);
    lbl_up_unit = mk_label(pg0, 276, 56, UI_FONT_META, CLR_UP);
    lv_label_set_text(lbl_down, "--");
    lv_label_set_text(lbl_down_unit, "MB/s");
    lv_label_set_text(lbl_up, "--");
    lv_label_set_text(lbl_up_unit, "MB/s");
    lv_obj_align_to(lbl_down_unit, lbl_down, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, -6);
    lv_obj_align_to(lbl_up_unit, lbl_up, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, -6);

    // 反模式:去掉竖向分隔条,靠留白分区

    // 反模式:去掉圆角边框面板;分区只用 1px 细分隔线
    mk_block(pg0, 12, 80, 296, 1, CLR_BORDER);

    lv_obj_t *graph = lv_obj_create(pg0);
    lv_obj_remove_style_all(graph);
    lv_obj_set_pos(graph, 12, 84);
    lv_obj_set_size(graph, 296, 74);

    lbl_main_curve_state = mk_label(graph, 0, 1, UI_FONT_META, CLR_DIM);
    lv_label_set_text(lbl_main_curve_state, APP_DEMO_MODE ? "10S DEMO" : "WARMING UP");
    // 图例:3px 色块 + 小字(色块即编码,不占胶囊)
    mk_block(graph, 78, 6, 3, 3, CLR_DOWN);
    lv_obj_t *legend_down = mk_label(graph, 85, 1, UI_FONT_META, CLR_DOWN);
    lv_label_set_text(legend_down, "DOWN");
    mk_block(graph, 142, 6, 3, 3, CLR_UP);
    lv_obj_t *legend_up = mk_label(graph, 149, 1, UI_FONT_META, CLR_UP);
    lv_label_set_text(legend_up, "UP");
    mk_block(graph, 186, 6, 3, 3, CLR_PING);
    lv_obj_t *legend_ping = mk_label(graph, 193, 1, UI_FONT_META, CLR_PING);
    lv_label_set_text(legend_ping, "GW");

    canvas_curve = lv_canvas_create(graph);
    lv_obj_set_pos(canvas_curve, 0, 24);
    lv_canvas_set_draw_buf(canvas_curve, &cvs_curve);

    lbl_ip = mk_label(pg0, 12, 153, UI_FONT_META, CLR_DIM);
    lv_label_set_text(lbl_ip, APP_DEMO_MODE ? "IP DEMO" : "IP ---");
    lbl_source = mk_label(pg0, 0, 153, UI_FONT_META, CLR_DIM);
    lv_label_set_text(lbl_source, APP_DEMO_MODE ? "DEMO DATA" : "IKUAI WAIT");
    lv_obj_align(lbl_source, LV_ALIGN_TOP_RIGHT, -12, 153);

    build_page_focus(s_pg[1]);
    build_page_dual(s_pg[2]);
    build_page_curve(s_pg[3]);
    build_page_network(s_pg[4]);
    build_page_sys(s_pg[5]);
    build_page_health(s_pg[6]);
    build_page_wan(s_pg[7]);
    build_page_top(s_pg[8]);
    build_page_ac(s_pg[9]);

    // 页码指示点(底部居中,10 点)
    const int dots_w = PAGE_COUNT * 4 + (PAGE_COUNT - 1) * 4;
    const int dots_x = (LCD_W - dots_w) / 2;
    for (int i = 0; i < PAGE_COUNT; i++) {
        s_dots[i] = lv_obj_create(scr);
        lv_obj_remove_style_all(s_dots[i]);
        lv_obj_set_size(s_dots[i], 4, 4);
        lv_obj_set_style_radius(s_dots[i], 2, 0);
        lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_COVER, 0);
        lv_obj_set_pos(s_dots[i], dots_x + i * 8, 164);
    }
    show_page(0, 1);

    gpio_config_t btn = {
        .pin_bit_mask = (1ULL << PIN_BUTTON_PREV) | (1ULL << PIN_BUTTON_NEXT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    s_btn_prev.stable = s_btn_prev.last_raw = gpio_get_level(s_btn_prev.pin);
    s_btn_next.stable = s_btn_next.last_raw = gpio_get_level(s_btn_next.pin);
    s_btn_prev.changed_ms = s_btn_next.changed_ms = now;
}

// 静默降灰/活跃点亮:无流量时大数字降为深灰(30号站待机精神)
#define CLR_IDLE 0x4A5260
static void set_active_style(lv_obj_t *big, lv_obj_t *unit, uint32_t unit_clr, bool active) {
    lv_obj_set_style_text_color(big, lv_color_hex(active ? CLR_TEXT : CLR_IDLE), 0);
    if (unit) lv_obj_set_style_text_color(unit, lv_color_hex(active ? unit_clr : CLR_IDLE), 0);
}

// PING 胶囊变色:正常=橙,高延迟(>=80ms)或超时=红(颜色即编码)
static void set_ping_style(float ms) {
    bool bad = (ms < 0 || ms >= 80.0f);
    uint32_t c = bad ? CLR_RED : CLR_PING;
    lv_obj_set_style_border_color(lbl_ping_bg, lv_color_hex(c), 0);
    lv_obj_set_style_text_color(lbl_ping, lv_color_hex(c), 0);
}

static bool sample_fresh(uint32_t ts, uint32_t max_age_sec) {
    if (!ts) return false;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
    return now >= ts && now - ts <= max_age_sec;
}

static void set_curve_state(const char *text, uint32_t color) {
    if (lbl_main_curve_state) {
        lv_label_set_text(lbl_main_curve_state, text);
        lv_obj_set_style_text_color(lbl_main_curve_state, lv_color_hex(color), 0);
    }
    if (lbl_curve_state) {
        lv_label_set_text(lbl_curve_state, text);
        lv_obj_set_style_text_color(lbl_curve_state, lv_color_hex(color), 0);
    }
}

static uint32_t trust_color(data_trust_t trust) {
    switch (trust) {
        case DATA_TRUST_LIVE: return CLR_GREEN;
        case DATA_TRUST_STALE: return CLR_PING;
        case DATA_TRUST_TIMEOUT:
        case DATA_TRUST_OFFLINE: return CLR_RED;
        default: return CLR_DIM;
    }
}

static const char *trust_source_text(data_trust_t trust) {
    switch (trust) {
        case DATA_TRUST_LIVE: return s_night_dim ? "LIVE | NIGHT" : "IKUAI LIVE";
        case DATA_TRUST_STALE: return "IKUAI STALE";
        case DATA_TRUST_TIMEOUT: return "IKUAI TIMEOUT";
        case DATA_TRUST_OFFLINE: return "DATA OFFLINE";
        case DATA_TRUST_DEMO: return s_night_dim ? "DEMO | NIGHT" : "DEMO DATA";
        case DATA_TRUST_NO_DATA: return "NO DATA";
        default: return "IKUAI WAIT";
    }
}

static void refresh_source_label(void) {
    if (!lbl_source) return;
    lv_label_set_text(lbl_source, trust_source_text(s_data_trust));
    uint32_t color = trust_color(s_data_trust);
    if (s_night_dim && (s_data_trust == DATA_TRUST_LIVE || s_data_trust == DATA_TRUST_DEMO)) {
        color = CLR_YELLOW;
    }
    lv_obj_set_style_text_color(lbl_source, lv_color_hex(color), 0);
}

static void set_curve_trust(data_trust_t trust, bool window_ready) {
    s_data_trust = trust;
    switch (trust) {
        case DATA_TRUST_LIVE:
            set_curve_state(window_ready ? "10S LIVE" : "WARMING UP",
                            window_ready ? CLR_GREEN : CLR_DIM);
            break;
        case DATA_TRUST_STALE: set_curve_state("STALE", CLR_PING); break;
        case DATA_TRUST_TIMEOUT: set_curve_state("TIMEOUT", CLR_RED); break;
        case DATA_TRUST_OFFLINE: set_curve_state("OFFLINE", CLR_RED); break;
        case DATA_TRUST_DEMO: set_curve_state("10S DEMO", CLR_DIM); break;
        case DATA_TRUST_NO_DATA: set_curve_state("NO DATA", CLR_DIM); break;
        default: set_curve_state("WARMING UP", CLR_DIM); break;
    }
    refresh_source_label();
}

static void link_failure_text(const char **detail, const char **footer) {
    switch (ikuai_get_link_state()) {
    case IKUAI_LINK_AUTH_ERROR:
        *detail = "CHECK TOKEN / AUTH";
        *footer = "UPDATE CREDENTIALS";
        break;
    case IKUAI_LINK_TIMEOUT:
        *detail = "API TIMEOUT";
        *footer = "CHECK ROUTER LOAD";
        break;
    case IKUAI_LINK_HTTP_ERROR:
        *detail = "API HTTP ERROR";
        *footer = "CHECK ROUTER API";
        break;
    case IKUAI_LINK_NETWORK_ERROR:
        *detail = "ROUTER UNREACHABLE";
        *footer = "CHECK ROUTER / WIFI";
        break;
    case IKUAI_LINK_WAIT:
    default:
        *detail = "WAITING FOR API";
        *footer = "CONNECTING";
        break;
    }
}

static float s_last_ping_ms = -1;
static bool s_last_active = false;
static bool s_up_active = false;
static bool s_last_sys_ok = false;

// 缓存最近一次速率文本,供焦点页使用
static char s_last_d[16] = "--", s_last_u[16] = "--";
static const char *s_last_du = "MB/s", *s_last_uu = "MB/s";
static long s_last_clients = -1;

// 焦点页/系统页数据刷新(每秒)
static void pages_update(void) {
    lv_label_set_text(lbl_focus_num, s_last_d);
    lv_label_set_text(lbl_focus_unit, s_last_du);
    char line[64];
    snprintf(line, sizeof(line), "UP %s %s", s_last_u, s_last_uu);
    lv_label_set_text(lbl_focus_sub, line);

    // 双通道页与趋势页共用同一份 1s 数值缓存，曲线则按帧连续推进。
    lv_label_set_text(lbl_dual_down, s_last_d);
    lv_label_set_text(lbl_dual_down_unit, s_last_du);
    lv_label_set_text(lbl_dual_up, s_last_u);
    lv_label_set_text(lbl_dual_up_unit, s_last_uu);
    set_active_style(lbl_dual_down, lbl_dual_down_unit, CLR_DOWN, s_last_active);
    set_active_style(lbl_dual_up, lbl_dual_up_unit, CLR_UP, s_up_active);
    snprintf(line, sizeof(line), "IP %s", s_ip);
    lv_label_set_text(lbl_dual_ip, line);
    snprintf(line, sizeof(line), "GW %s", s_last_ping_ms < 0 ? "-- MS" : "");
    if (s_last_ping_ms >= 0) snprintf(line, sizeof(line), "GW %.0f MS", (double)s_last_ping_ms);
    lv_label_set_text(lbl_dual_ping, line);
    lv_obj_set_style_text_color(lbl_dual_ping,
        lv_color_hex(s_last_ping_ms < 0 || s_last_ping_ms >= 80 ? CLR_RED : CLR_PING), 0);

    snprintf(line, sizeof(line), "D %s%s", s_last_d, s_last_du);
    lv_label_set_text(lbl_curve_down, line);
    snprintf(line, sizeof(line), "U %s%s", s_last_u, s_last_uu);
    lv_label_set_text(lbl_curve_up, line);
    if (s_last_ping_ms < 0) snprintf(line, sizeof(line), "GW --");
    else snprintf(line, sizeof(line), "GW %.0f", (double)s_last_ping_ms);
    lv_label_set_text(lbl_curve_ping, line);
    lv_obj_set_style_text_color(lbl_curve_ping,
        lv_color_hex(s_last_ping_ms < 0 || s_last_ping_ms >= 80 ? CLR_RED : CLR_PING), 0);

    // 网络健康页先给可读结论，颜色只作为第二层编码。
    uint32_t net_color;
    const char *net_status;
    const char *net_detail;
    const char *net_footer;
    if (!s_last_sys_ok) {
        net_color = CLR_RED;
        net_status = "WAN OFFLINE";
        if (s_wifi_ok) {
            link_failure_text(&net_detail, &net_footer);
        } else {
            net_detail = s_wifi_retry_count ? "WIFI RECONNECTING" : "WIFI CONNECTING";
            net_footer = s_wifi_retry_count ? "RETRY BACKOFF ACTIVE" : "WAITING FOR WIFI";
        }
    } else if (s_last_ping_ms < 0) {
        net_color = CLR_RED;
        net_status = "PING TIMEOUT";
        net_detail = "TRAFFIC DATA AVAILABLE";
        net_footer = "CHECK GATEWAY ICMP";
    } else if (s_last_ping_ms >= 80) {
        net_color = CLR_PING;
        net_status = "DEGRADED";
        net_detail = "HIGH LATENCY";
        net_footer = "WATCH PING TREND";
    } else {
        net_color = CLR_GREEN;
        net_status = "WAN ONLINE";
        net_detail = "NETWORK STABLE";
        net_footer = "NO ACTIVE ALERTS";
    }
    lv_label_set_text(lbl_net_status, net_status);
    lv_label_set_text(lbl_net_detail, net_detail);
    lv_label_set_text(lbl_net_footer, net_footer);
    lv_obj_set_style_text_color(lbl_net_status, lv_color_hex(net_color), 0);
    lv_obj_set_style_text_color(lbl_net_detail, lv_color_hex(net_color), 0);
    lv_label_set_text(lbl_dual_state, s_last_sys_ok ? "WAN ONLINE" : "WAN OFFLINE");
    lv_obj_set_style_text_color(lbl_dual_state, lv_color_hex(s_last_sys_ok ? CLR_GREEN : CLR_RED), 0);

    if (s_last_ping_ms < 0) snprintf(line, sizeof(line), "GW --");
    else snprintf(line, sizeof(line), "GW %.0f", (double)s_last_ping_ms);
    lv_label_set_text(lbl_net_ping, line);
    snprintf(line, sizeof(line), "D %s", s_last_d);
    lv_label_set_text(lbl_net_down, line);
    snprintf(line, sizeof(line), "U %s", s_last_u);
    lv_label_set_text(lbl_net_up, line);

    lv_label_set_text(lbl_sys_ip, s_ip);
    snprintf(line, sizeof(line), "%ld", s_last_clients < 0 ? 0 : s_last_clients);
    lv_label_set_text(lbl_sys_clients, line);
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    snprintf(line, sizeof(line), "%luh %02lum", (unsigned long)(up / 3600), (unsigned long)((up % 3600) / 60));
    lv_label_set_text(lbl_sys_uptime, line);
    snprintf(line, sizeof(line), "%u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024));
    lv_label_set_text(lbl_sys_heap, line);

    lv_label_set_text(lbl_health_status, s_last_sys_ok ? "ONLINE" : "OFFLINE");
    lv_obj_set_style_text_color(lbl_health_status,
        lv_color_hex(s_last_sys_ok ? CLR_GREEN : CLR_RED), 0);
    snprintf(line, sizeof(line), "%ld CLIENTS", s_last_clients < 0 ? 0 : s_last_clients);
    lv_label_set_text(lbl_top_count, line);
    snprintf(line, sizeof(line), "TOTAL DOWN %s%s | UP %s%s",
             s_last_d, s_last_du, s_last_u, s_last_uu);
    lv_label_set_text(lbl_top_total, line);

    // 焦点页:活跃点亮/静默降灰 + 峰值标记
    {
        uint32_t pk = s_peak_down;
        bool act = s_last_active;
        set_active_style(lbl_focus_num, lbl_focus_unit, CLR_DOWN, act);
        if (pk >= 1024) {
            char pv[16]; const char *pu;
            fmt_rate(pk, &s_peak_unit, pv, sizeof(pv), &pu);
            char pl[40];
            snprintf(pl, sizeof(pl), "PEAK %s %s *", pv, pu);
            lv_label_set_text(lbl_focus_peak, pl);
        }
    }

    // ── 扩展页(健康/WAN/排行/AC)──
    ikuai_extra_t ex;
    bool has_ex = false;
#if APP_DEMO_MODE
    uint32_t now_sec = (uint32_t)(esp_timer_get_time() / 1000000);
    memset(&ex, 0, sizeof(ex));
    ex.ok = true; has_ex = true;
    ex.system_ts = now_sec; ex.wan_ts = now_sec;
    ex.clients_ts = now_sec; ex.ac_ts = now_sec;
    ex.cpu_temp = 46; ex.uptime_sec = 86400 * 3 + 3600 * 7;
    strncpy(ex.version, "3.7.21", sizeof(ex.version));
    ex.wan_cnt = 1;
    strncpy(ex.wan[0].name, "wan1", 12); strncpy(ex.wan[0].ip, "100.64.1.2", 16);
    strncpy(ex.wan[0].gateway, "100.64.1.1", 16); ex.wan[0].ok = 1;
    ex.client_cnt = 3;
    strncpy(ex.client[0].name, "Xiaomi-TV", 24); ex.client[0].down_bps = 6 * 1024 * 1024;
    strncpy(ex.client[1].name, "MacBook-Pro", 24); ex.client[1].down_bps = 2400 * 1024;
    strncpy(ex.client[2].name, "192.168.9.33", 24); ex.client[2].down_bps = 380 * 1024;
    ex.ac_on = 1; ex.ap_online = 2; ex.ap_count = 2; ex.clt_2g = 7; ex.clt_5g = 9;
#else
    has_ex = ikuai_get_extra(&ex);
#endif
    bool system_fresh = has_ex && sample_fresh(ex.system_ts, 10);
    bool wan_fresh = has_ex && sample_fresh(ex.wan_ts, 10);
    bool clients_fresh = has_ex && sample_fresh(ex.clients_ts, 10);
    bool ac_fresh = has_ex && sample_fresh(ex.ac_ts, 10);
    // 健康页:CPU/MEM 用 1s 实时数据;温度/运行时间用扩展数据
    {
        ikuai_sys_t sy;
        if (ikuai_get_sys(&sy) && sy.ok) {
            snprintf(line, sizeof(line), "%.0f", (double)sy.cpu_pct);
            lv_label_set_text(lbl_cpu, line);
            snprintf(line, sizeof(line), "%.0f", (double)sy.mem_pct);
            lv_label_set_text(lbl_mem, line);
        } else {
            lv_label_set_text(lbl_cpu, "--");
            lv_label_set_text(lbl_mem, "--");
        }
        align_value_suffix(lbl_cpu, lbl_cpu_unit);
        align_value_suffix(lbl_mem, lbl_mem_unit);
    }
    if (has_ex) {
        if (system_fresh) {
        snprintf(line, sizeof(line), "%.0f C", (double)ex.cpu_temp);
        lv_label_set_text(lbl_temp, line);
        uint32_t d = ex.uptime_sec / 86400, h = (ex.uptime_sec % 86400) / 3600;
        snprintf(line, sizeof(line), "%lud %02luh", (unsigned long)d, (unsigned long)h);
        lv_label_set_text(lbl_uptime, line);
        snprintf(line, sizeof(line), "%ld CLT | %uK HEAP | V%s",
                 s_last_clients < 0 ? 0 : s_last_clients,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024),
                 ex.version[0] ? ex.version : "--");
        lv_label_set_text(lbl_health_meta, line);
        } else {
            lv_label_set_text(lbl_temp, "STALE");
            lv_label_set_text(lbl_uptime, "STALE");
            snprintf(line, sizeof(line), "%ld CLT | %uK HEAP | VSTALE",
                     s_last_clients < 0 ? 0 : s_last_clients,
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024));
            lv_label_set_text(lbl_health_meta, line);
        }
        // WAN 页
        for (int i = 0; i < 2; i++) {
            bool on = i < ex.wan_cnt;
            bool ok = on && ex.wan[i].ok;
            lv_label_set_text(lbl_wan[i][0], !wan_fresh ? "STALE" : (ok ? "ONLINE" : "OFFLINE"));
            lv_obj_set_style_text_color(lbl_wan[i][0],
                lv_color_hex(!wan_fresh ? CLR_PING : (ok ? CLR_GREEN : CLR_RED)), 0);
            lv_label_set_text(lbl_wan[i][1], wan_fresh && on ? ex.wan[i].ip : "--");
            lv_label_set_text(lbl_wan[i][2], wan_fresh && on ? ex.wan[i].gateway : "--");
            lv_obj_set_style_bg_color(lbl_wan_dot[i],
                lv_color_hex(!wan_fresh ? CLR_PING : (ok ? CLR_GREEN : CLR_RED)), 0);
        }
        // 排行页
        for (int i = 0; i < 3; i++) {
            if (clients_fresh && i < ex.client_cnt) {
                lv_label_set_text(lbl_cli[i][0], ex.client[i].name);
                char v[16]; const char *u;
                fmt_rate(ex.client[i].down_bps, &s_client_unit[i], v, sizeof(v), &u);
                snprintf(line, sizeof(line), "%s %s", v, u);
                lv_label_set_text(lbl_cli[i][1], line);
            } else {
                s_client_unit[i] = RATE_UNIT_AUTO;
                lv_label_set_text(lbl_cli[i][0], clients_fresh ? "--" : "STALE");
                lv_label_set_text(lbl_cli[i][1], clients_fresh ? "--" : "STALE");
            }
        }
        // AC 页
        if (ac_fresh) {
            lv_label_set_text(lbl_ac, ex.ac_on ? "ON" : "OFF");
            snprintf(line, sizeof(line), "%d / %d", ex.ap_online, ex.ap_count);
            lv_label_set_text(lbl_ap, line);
            snprintf(line, sizeof(line), "%02d", ex.clt_2g);
            lv_label_set_text(lbl_clt_2g, line);
            snprintf(line, sizeof(line), "%02d", ex.clt_5g);
            lv_label_set_text(lbl_clt_5g, line);
        } else {
            lv_label_set_text(lbl_ac, "STALE");
            lv_label_set_text(lbl_ap, "STALE");
            lv_label_set_text(lbl_clt_2g, "--");
            lv_label_set_text(lbl_clt_5g, "--");
        }
    } else {
        lv_label_set_text(lbl_temp, "--");
        lv_label_set_text(lbl_uptime, "--");
        snprintf(line, sizeof(line), "%ld CLT | %uK HEAP | V--",
                 s_last_clients < 0 ? 0 : s_last_clients,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024));
        lv_label_set_text(lbl_health_meta, line);
        for (int i = 0; i < 2; i++) {
            lv_label_set_text(lbl_wan[i][0], "OFFLINE");
            lv_obj_set_style_text_color(lbl_wan[i][0], lv_color_hex(CLR_RED), 0);
            lv_label_set_text(lbl_wan[i][1], "--");
            lv_label_set_text(lbl_wan[i][2], "--");
            lv_obj_set_style_bg_color(lbl_wan_dot[i], lv_color_hex(CLR_RED), 0);
        }
        for (int i = 0; i < 3; i++) {
            lv_label_set_text(lbl_cli[i][0], "--");
            lv_label_set_text(lbl_cli[i][1], "--");
        }
        lv_label_set_text(lbl_ac, "--");
        lv_label_set_text(lbl_ap, "--");
        lv_label_set_text(lbl_clt_2g, "--");
        lv_label_set_text(lbl_clt_5g, "--");
    }
}

// 每秒：更新数字 + 采样目标
static void ui_update(void) {
#if APP_DEMO_MODE
    static uint32_t tick;
    tick++;

    // 三组不同周期的三角波，离线即可检查数字、字体和曲线动画。
    uint32_t d_phase = tick % 18;
    uint32_t u_phase = (tick + 5) % 14;
    uint32_t p_phase = (tick + 3) % 10;
    uint32_t d_tri = d_phase <= 9 ? d_phase : 18 - d_phase;
    uint32_t u_tri = u_phase <= 7 ? u_phase : 14 - u_phase;
    uint32_t p_tri = p_phase <= 5 ? p_phase : 10 - p_phase;
    uint32_t down_bps = (2U + d_tri * 3U) * 1024U * 1024U;
    uint32_t up_bps = (1U + u_tri) * 384U * 1024U;
    float ping_ms = 12.0f + (float)(p_tri * 7U);

    char d[16], u[16], line[32];
    const char *du, *uu;
    fmt_rate(down_bps, &s_down_unit, d, sizeof(d), &du);
    fmt_rate(up_bps, &s_up_unit, u, sizeof(u), &uu);
    lv_label_set_text(lbl_down, d);
    lv_label_set_text(lbl_up, u);
    lv_label_set_text(lbl_down_unit, du);
    lv_label_set_text(lbl_up_unit, uu);
    lv_obj_align_to(lbl_down_unit, lbl_down, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, -6);
    lv_obj_align_to(lbl_up_unit, lbl_up, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, -6);
    s_last_active = down_bps >= ACTIVE_BPS; s_up_active = up_bps >= ACTIVE_BPS;
    s_last_sys_ok = true;
    set_active_style(lbl_down, lbl_down_unit, CLR_DOWN, s_last_active);
    set_active_style(lbl_up, lbl_up_unit, CLR_UP, s_up_active);
    s_last_ping_ms = ping_ms;
    set_ping_style(ping_ms);
    strncpy(s_last_d, d, sizeof(s_last_d)); strncpy(s_last_u, u, sizeof(s_last_u));
    s_last_du = du; s_last_uu = uu; s_last_clients = 18;
    lv_label_set_text(lbl_online, "18 CLIENTS");
    lv_label_set_text(lbl_status, "DEMO ONLINE");
    lv_obj_set_style_bg_color(dot_status, lv_color_hex(CLR_GREEN), 0);
    snprintf(line, sizeof(line), "GW %.0f MS", ping_ms);
    lv_label_set_text(lbl_ping, line);
    lv_label_set_text(lbl_ip, "IP DEMO");

    s_track[TRACK_DOWN].target = (float)d_tri / 9.0f;
    s_track[TRACK_UP].target = (float)u_tri / 7.0f;
    s_track[TRACK_PING].target = ping_ms / 47.0f;
    meteor_note_rate(down_bps);
    set_curve_trust(DATA_TRUST_DEMO, true);
    pages_update();
    return;
#endif

    ikuai_sys_t s;
    ikuai_curve_t c;

    bool sys_ok = ikuai_get_sys(&s) && s.ok;
    s_last_sys_ok = sys_ok;
    if (sys_ok) {
        char d[16], u[16];
        const char *du, *uu;
        fmt_rate(s.down_bps, &s_down_unit, d, sizeof(d), &du);
        fmt_rate(s.up_bps, &s_up_unit, u, sizeof(u), &uu);
        lv_label_set_text(lbl_down, d);
        lv_label_set_text(lbl_up, u);
        lv_label_set_text(lbl_down_unit, du);
        lv_label_set_text(lbl_up_unit, uu);
        lv_obj_align_to(lbl_down_unit, lbl_down, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, -6);
        lv_obj_align_to(lbl_up_unit, lbl_up, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, -6);
        s_last_active = s.down_bps >= ACTIVE_BPS; s_up_active = s.up_bps >= ACTIVE_BPS;
        set_active_style(lbl_down, lbl_down_unit, CLR_DOWN, s_last_active);
        set_active_style(lbl_up, lbl_up_unit, CLR_UP, s_up_active);
        meteor_note_rate(s.down_bps);
        strncpy(s_last_d, d, sizeof(s_last_d)); strncpy(s_last_u, u, sizeof(s_last_u));
        s_last_du = du; s_last_uu = uu; s_last_clients = (long)s.online_cnt;
        char line[32];
        snprintf(line, sizeof(line), "%lu CLIENTS", (unsigned long)s.online_cnt);
        lv_label_set_text(lbl_online, line);
        lv_label_set_text(lbl_status, "WAN ONLINE");
        lv_obj_set_style_bg_color(dot_status, lv_color_hex(CLR_GREEN), 0);

        if (s.down_bps > s_peak_down) s_peak_down = s.down_bps;
        else s_peak_down = (uint32_t)(((uint64_t)s_peak_down * 63 + s.down_bps) / 64);
        if (s_peak_down < 1024) s_peak_down = 1024;

        if (s.up_bps > s_peak_up) s_peak_up = s.up_bps;
        else s_peak_up = (uint32_t)(((uint64_t)s_peak_up * 63 + s.up_bps) / 64);
        if (s_peak_up < 1024) s_peak_up = 1024;

        s_track[TRACK_DOWN].target = (float)s.down_bps / (float)s_peak_down;
        s_track[TRACK_UP].target = (float)s.up_bps / (float)s_peak_up;
    } else {
        lv_label_set_text(lbl_down, "--");
        lv_label_set_text(lbl_up, "--");
        lv_label_set_text(lbl_down_unit, "");
        lv_label_set_text(lbl_up_unit, "");
        lv_label_set_text(lbl_online, "-- CLIENTS");
        if (s_wifi_ok) lv_label_set_text(lbl_status, "WAN OFFLINE");
        else if (s_wifi_retry_count) {
            char retry[24];
            snprintf(retry, sizeof(retry), "WIFI RETRY %u", (unsigned)s_wifi_retry_count);
            lv_label_set_text(lbl_status, retry);
        } else {
            lv_label_set_text(lbl_status, "WIFI CONNECTING");
        }
        lv_obj_set_style_bg_color(dot_status, lv_color_hex(CLR_RED), 0);
        s_last_active = false;
        s_up_active = false;
        s_down_unit = RATE_UNIT_AUTO;
        s_up_unit = RATE_UNIT_AUTO;
        strncpy(s_last_d, "--", sizeof(s_last_d));
        strncpy(s_last_u, "--", sizeof(s_last_u));
        s_last_du = "";
        s_last_uu = "";
        s_last_clients = -1;
        set_active_style(lbl_down, lbl_down_unit, CLR_DOWN, false);
        set_active_style(lbl_up, lbl_up_unit, CLR_UP, false);
        set_ping_style(-1);
        s_meteor_last_bps = 0;
        s_track[TRACK_DOWN].target = 0;
        s_track[TRACK_UP].target = 0;
    }

    if (ikuai_get_curve(&c) && c.n > 0) {
        set_curve_trust(!sys_ok ? DATA_TRUST_STALE :
                        (c.n < 10 ? DATA_TRUST_WARMING : DATA_TRUST_LIVE),
                        c.n >= 10);
        int slot = (c.head - 1 + IKUAI_CURVE_MAX) % IKUAI_CURVE_MAX;
        float p = c.ping_ms[slot];
        if (p < 0) {
            lv_label_set_text(lbl_ping, "GW -- MS");
            set_ping_style(-1);
            s_last_ping_ms = -1;
            s_track[TRACK_PING].target = 0;
        }
        else {
            char line[24];
            snprintf(line, sizeof(line), "GW %.0f MS", p);
            lv_label_set_text(lbl_ping, line);
            set_ping_style(p);
            s_last_ping_ms = p;
            if (p > s_peak_ping) s_peak_ping = p;
            else s_peak_ping = s_peak_ping * 0.985f + p * 0.015f;
            if (s_peak_ping < 50.0f) s_peak_ping = 50.0f;
            s_track[TRACK_PING].target = p / s_peak_ping;
        }
    } else {
        lv_label_set_text(lbl_ping, "GW -- MS");
        set_ping_style(-1);
        s_last_ping_ms = -1;
        set_curve_trust(sys_ok ? DATA_TRUST_NO_DATA :
                        (s_wifi_ok ? DATA_TRUST_TIMEOUT : DATA_TRUST_OFFLINE), false);
        s_track[TRACK_PING].target = 0;
    }

    static char s_last_ip[16] = "";
    if (strcmp(s_ip, s_last_ip) != 0) {
        strncpy(s_last_ip, s_ip, sizeof(s_last_ip));
        char line[32];
        snprintf(line, sizeof(line), "IP %s", s_ip);
        lv_label_set_text(lbl_ip, line);
    }
    pages_update();
}

static lv_timer_t *s_ui_timer;

static void ui_timer_cb(lv_timer_t *t) {
    ui_update();
    night_dim_poll();

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_dot_feedback_until_ms &&
        (int32_t)(now_ms - s_dot_feedback_until_ms) >= 0) {
        s_dot_feedback_until_ms = 0;
        if (s_page >= 0 && s_page < PAGE_COUNT)
            lv_obj_set_style_bg_color(s_dots[s_page], lv_color_hex(CLR_YELLOW), 0);
    }

    // 60s 无操作自动回主页(摆件的归宿感)
    if (s_page != 0 && s_last_key_ms &&
        (uint32_t)(esp_timer_get_time() / 1000) - s_last_key_ms > 60000) {
        show_page(0, -1);
    }

    // T-Display-S3 无 WS2812：状态由屏上状态点与文案承担
    // heap 监控（30s）
    static uint32_t s_last_heap_log = 0;
    static uint32_t s_min_heap = 0xFFFFFFFF;
    static uint32_t s_last_curve_frames = 0;
    uint32_t sec_now = (uint32_t)(esp_timer_get_time() / 1000000);
    if (sec_now - s_last_heap_log >= 30) {
        uint32_t elapsed = sec_now - s_last_heap_log;
        s_last_heap_log = sec_now;
        uint32_t free8 = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        uint32_t frame_delta = s_curve_frames - s_last_curve_frames;
        s_last_curve_frames = s_curve_frames;
        uint32_t fps_x10 = elapsed ? frame_delta * 10 / elapsed : 0;
        if (free8 < s_min_heap) s_min_heap = free8;
        ESP_LOGI(TAG, "heap free=%u min=%u largest=%u curve_fps=%u.%u",
                 (unsigned)free8, (unsigned)s_min_heap, (unsigned)largest,
                 (unsigned)(fps_x10 / 10), (unsigned)(fps_x10 % 10));
    }

}

static void roll_timer_cb(lv_timer_t *t) {
    curve_roll();
}

static void lv_tick_cb(void *arg) { lv_tick_inc(1); }

static void ui_task(void *arg) {
    lv_init();
    lv_port_disp_init();

    s_c_bg = RGB565(0x00, 0x00, 0x00);
    s_c_grid = RGB565(0x00, 0x00, 0x00);
    s_track[TRACK_DOWN].color = RGB565(0x37, 0xC4, 0xD6);
    s_track[TRACK_UP].color = RGB565(0x3F, 0xA9, 0xF5);
    s_track[TRACK_PING].color = RGB565(0xFF, 0x7A, 0x1A);
    for (int i = 0; i < TRACK_COUNT; i++) {
        s_track[i].dim = mix565(s_c_bg, s_track[i].color, 120);
        s_track[i].fill = mix565(s_c_bg, s_track[i].color, 55);
    }

    ui_create();
    curve_clear();
    uint16_t *b2 = (uint16_t *)cvs_curve2.data;
    for (int i = 0; i < CURVE2_W * CURVE2_H; i++) b2[i] = s_c_bg;
    uint16_t *bd = (uint16_t *)cvs_dual_down.data;
    uint16_t *bu = (uint16_t *)cvs_dual_up.data;
    for (int i = 0; i < DUAL_CURVE_W * DUAL_CURVE_H; i++) {
        bd[i] = s_c_bg;
        bu[i] = s_c_bg;
    }

    // Render the initial black/background frame while the backlight is still
    // off, then enable it only after the i80 transfer has completed.
    lv_refr_now(lv_display_get_default());
    bool backlight_ready = lv_port_disp_wait_first_flush(1000);
    if (backlight_ready) {
        lcd_set_backlight(APP_BL_PCT);
    } else {
        // Fail dark instead of exposing unknown GRAM. The UI task keeps
        // running and will recover if a late transfer eventually completes.
        ESP_LOGE(TAG, "first display flush timed out; waiting with backlight off");
    }

    const esp_timer_create_args_t tmr_args = { .callback = lv_tick_cb, .name = "lv_tick" };
    esp_timer_handle_t tmr;
    ESP_ERROR_CHECK(esp_timer_create(&tmr_args, &tmr));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tmr, 1000));

    s_ui_timer = lv_timer_create(ui_timer_cb, 1000, NULL);
    s_roll_timer = lv_timer_create(roll_timer_cb, 1000 / CURVE_FPS, NULL);

    for (;;) {
        if (!backlight_ready && lv_port_disp_first_flush_done()) {
            backlight_ready = true;
            lcd_set_backlight(APP_BL_PCT);
            ESP_LOGW(TAG, "late first display flush completed; backlight enabled");
        }
        buttons_poll();
        lv_timer_handler();
        // Give IDLE0 enough scheduling time on the 160 MHz default clock;
        // LVGL software rendering still runs at its configured 33 ms period.
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ─── Entry ───────────────────────────────────────────────────────────

void widget_start(void) {
    s_eg = xEventGroupCreate();
    if (!s_eg) ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
#if APP_DEMO_MODE
    s_wifi_ok = true;
    strncpy(s_ip, "DEMO", sizeof(s_ip));
    ESP_LOGI(TAG, "offline demo mode; Wi-Fi and iKuai disabled");
#else
    wifi_start();
    ikuai_monitor_start();
#endif
    ESP_LOGI(TAG, "heap at start: free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    // LVGL's widget construction/render path needs more than a 4K-word stack,
    // while Wi-Fi/TLS already consumes the scarce internal RAM.  Keep the
    // task control block internal but place the UI worker stack in PSRAM.
    BaseType_t ui_created = xTaskCreatePinnedToCoreWithCaps(
        ui_task, "ui", 8192, NULL, 6, NULL, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ui_created != pdPASS) {
        ESP_LOGE(TAG, "ui task create failed: psram_free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    } else {
        ESP_LOGI(TAG, "ui task started on core 1");
    }
}
