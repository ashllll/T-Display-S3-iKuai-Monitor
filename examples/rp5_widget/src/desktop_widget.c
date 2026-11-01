// Board lifecycle for the rp5 agent console. UI is in rp5_view.c.
#include "desktop_widget.h"
#include "lcd_driver.h"
#if __has_include("config.h")
#include "config.h"
#else
#include "config.example.h"
#endif

#include "lvgl.h"
#include "lv_port_disp.h"
#include "rp5_monitor.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
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

#include "rp5_view.h"
#ifndef APP_DEMO_MODE
#define APP_DEMO_MODE 0
#endif
#if defined(APP_REQUIRE_LIVE_MODE) && APP_DEMO_MODE
#error "APP_REQUIRE_LIVE_MODE requires APP_DEMO_MODE=0"
#endif
#ifndef APP_TZ
#define APP_TZ "CST-8"
#endif
#ifndef APP_NIGHT_START
#define APP_NIGHT_START 23
#define APP_NIGHT_END 7
#define APP_BL_NIGHT_PCT 8
#endif

static const char *TAG="widget";
#define BIT_IP (1 << 0)
static EventGroupHandle_t s_eg;
static char s_ip[16]="---";
static volatile bool s_wifi_ok;
static volatile uint8_t s_wifi_retry_count;
static esp_timer_handle_t s_wifi_retry_timer;
static bool s_night_dim;
static uint8_t s_bl_level_target;   // kept in sync by backlight_set()
static uint64_t s_last_key_ms;
static size_t internal_heap_free(void){return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);}

static void backlight_set(uint8_t pct) {
    lcd_set_backlight(pct);
    s_bl_level_target = lcd_backlight_level();
}
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
        rp5_monitor_network_changed();
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
        rp5_monitor_network_changed();
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

static void night_dim_poll(void) {
    time_t t = time(NULL);
    struct tm tm_;
    localtime_r(&t, &tm_);
    if (tm_.tm_year + 1900 < 2024) return;  // 未授时
    bool night = (APP_NIGHT_START > APP_NIGHT_END)
        ? (tm_.tm_hour >= APP_NIGHT_START || tm_.tm_hour < APP_NIGHT_END)   // 跨零点
        : (tm_.tm_hour >= APP_NIGHT_START && tm_.tm_hour < APP_NIGHT_END);
    if (night == s_night_dim) return;
    s_night_dim = night;
    s_bl_level_target = lcd_backlight_level_of_pct(night ? APP_BL_NIGHT_PCT : APP_BL_PCT);
}

// Called from the 100 ms UI tick. The brightness axis is a 16-step pulse counter, so
// a day/night switch used to be one visible snap in the dark. Dimming one level per
// tick costs at most 1.6 s and is below the threshold where it reads as a change;
// brightening keeps the jump because that direction needs 15 pulses per level.
static void backlight_ramp_poll(void) {
    uint8_t cur = lcd_backlight_level();
    if (cur == s_bl_level_target) return;
    if (cur && s_bl_level_target < cur) lcd_set_backlight_level((uint8_t)(cur - 1));
    else lcd_set_backlight_level(s_bl_level_target);
}

typedef struct {gpio_num_t pin;int stable,last_raw;uint64_t changed_ms;} page_button_t;
#define BUTTON_DEBOUNCE_MS 30
static page_button_t s_btn_prev={.pin=GPIO_NUM_0},s_btn_next={.pin=GPIO_NUM_14};
static bool button_pressed(page_button_t *button, uint64_t now) {
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


static void buttons_poll(void){
    uint64_t now=(uint64_t)(esp_timer_get_time()/1000);
    bool prev=button_pressed(&s_btn_prev,now),next=button_pressed(&s_btn_next,now);
    if(prev==next)return;
    s_last_key_ms=now;rp5_view_step(prev?-1:1);
    rp5_view_refresh(now,s_wifi_ok,s_ip);
}
static void ui_timer_cb(lv_timer_t *timer){
    (void)timer;uint64_t now=(uint64_t)(esp_timer_get_time()/1000);
    if(rp5_view_page()!=0 && s_last_key_ms && now-s_last_key_ms>60000)rp5_view_home();
    rp5_view_refresh(now,s_wifi_ok,s_ip);night_dim_poll();backlight_ramp_poll();
}
static void animation_timer_cb(lv_timer_t *timer){
    (void)timer;rp5_view_animate((uint64_t)(esp_timer_get_time()/1000));
}
static void lv_tick_cb(void *arg) { lv_tick_inc(1); }

static void ui_task(void *arg) {
    lv_init();
    lv_port_disp_init();

    rp5_view_create();
    gpio_config_t btn={.pin_bit_mask=(1ULL<<GPIO_NUM_0)|(1ULL<<GPIO_NUM_14),.mode=GPIO_MODE_INPUT,
        .pull_up_en=GPIO_PULLUP_ENABLE,.pull_down_en=GPIO_PULLDOWN_DISABLE,.intr_type=GPIO_INTR_DISABLE};
    gpio_config(&btn);
    s_btn_prev.stable=s_btn_prev.last_raw=gpio_get_level(s_btn_prev.pin);
    s_btn_next.stable=s_btn_next.last_raw=gpio_get_level(s_btn_next.pin);
    s_btn_prev.changed_ms=s_btn_next.changed_ms=(uint64_t)(esp_timer_get_time()/1000);
    // Render the initial black/background frame while the backlight is still
    // off, then enable it only after the i80 transfer has completed.
    lv_refr_now(lv_display_get_default());
    bool backlight_ready = lv_port_disp_wait_first_flush(1000);
    if (backlight_ready) {
        backlight_set(APP_BL_PCT);
    } else {
        // Fail dark instead of exposing unknown GRAM. The UI task keeps
        // running and will recover if a late transfer eventually completes.
        ESP_LOGE(TAG, "first display flush timed out; waiting with backlight off");
    }

    const esp_timer_create_args_t tmr_args = { .callback = lv_tick_cb, .name = "lv_tick" };
    esp_timer_handle_t tmr;
    ESP_ERROR_CHECK(esp_timer_create(&tmr_args, &tmr));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tmr, 1000));

    lv_timer_create(ui_timer_cb, 100, NULL);
    lv_timer_create(animation_timer_cb, LV_DEF_REFR_PERIOD, NULL);

    for (;;) {
        if (!backlight_ready && lv_port_disp_first_flush_done()) {
            backlight_ready = true;
            backlight_set(APP_BL_PCT);
            ESP_LOGW(TAG, "late first display flush completed; backlight enabled");
        }
        buttons_poll();
        // Sleep exactly until LVGL's next timer; a fixed 10 ms delay added
        // phase jitter on top of the refresh period and read as stutter.
        uint32_t next = lv_timer_handler();
        if (next == LV_NO_TIMER_READY) next = 10;
        else if (next < 1) next = 1;
        else if (next > 20) next = 20;
        vTaskDelay(pdMS_TO_TICKS(next));
    }
}

void widget_start(void) {
    s_eg = xEventGroupCreate();
    if (!s_eg) ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
#if APP_DEMO_MODE
    s_wifi_ok = true;
    strncpy(s_ip, "DEMO", sizeof(s_ip));
    ESP_LOGI(TAG, "offline demo mode; Wi-Fi and rp5 agent disabled");
#else
    wifi_start();
    rp5_monitor_start();
#endif
    ESP_LOGI(TAG, "heap at start: heap8_free=%u internal_free=%u internal_largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)internal_heap_free(),
             (unsigned)heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
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
