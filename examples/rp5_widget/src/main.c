// LILYGO T-Display-S3 — 桌面信息摆件（LVGL 界面）
// 树莓派网关健康度、代理状态与出口速率
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "rp5_monitor.h"
#include "nvs_flash.h"
#include "lcd_driver.h"
#include "lv_port_disp.h"
#include "desktop_widget.h"
#if __has_include("config.h")
#include "config.h"
#else
#include "config.example.h"
#endif

static const char *TAG = "main";

void app_main(void) {
    ESP_LOGI(TAG, "=== rp5 console / smooth timeline === reset_reason=%d", esp_reset_reason());

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Keep the backlight off until LVGL has rendered and transferred its
    // first complete frame.  Otherwise the panel can show undefined GRAM
    // contents as a power-on garbage screen before the UI task starts.
    lcd_init();

    widget_start();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        rp5_sys_t state={0};rp5_proxy_t proxy={0};bool live=rp5_get_sys(&state);
        rp5_get_proxy(&proxy);
        static uint32_t last_frames, last_tx;
        // Two different numbers, because they measure different things: LVGL's
        // RENDER_READY counts frames, the i80 callback counts DMA transfers, and
        // partial rendering pushes one transfer per dirty band. Reporting only the
        // latter is what made this console look like it was hitting 60 fps.
        uint32_t frames = lv_port_disp_frame_count();
        uint32_t tx = lcd_frame_count();
        float fps = (frames - last_frames) / 30.0f;
        float tps = (tx - last_tx) / 30.0f;
        last_frames = frames;
        last_tx = tx;
        // One heartbeat line per 30 s keeps a console-only diagnosis possible:
        // live=0 means the agent is unreachable, delay=0 means it never measured.
        ESP_LOGI(TAG,"rp5 uptime=%llu live=%d heap=%lu node=%s delay=%lums conns=%lu",
            (unsigned long long)(esp_timer_get_time()/1000000),live,(unsigned long)esp_get_free_heap_size(),
            proxy.node_valid?proxy.node:"-",proxy.delay_valid?(unsigned long)proxy.delay_ms:0UL,
            (unsigned long)proxy.conns);
        ESP_LOGI(TAG,"fps=%.1f i80_tx=%.1f frames=%lu",fps,tps,(unsigned long)frames);
    }
}
