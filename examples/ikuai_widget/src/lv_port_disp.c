// LVGL → ST7789 显示适配层（横屏 320x170）
// - 双缓冲（320x40 两片，共 51.2KB）
// - 字节序由 esp_lcd i80 的 swap_color_bytes 在发送阶段处理
#include "lv_port_disp.h"
#include "lvgl.h"
#include "lcd_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DRAW_BUF_ROWS 40

// LVGL 9's lv_color_t is an RGB888 helper type (3 bytes). The panel buffer
// must match the display color format, RGB565 (2 bytes per pixel), otherwise
// LVGL renders more rows than the i80 DMA transaction can carry.
static uint16_t s_buf1[LCD_W * DRAW_BUF_ROWS];
static uint16_t s_buf2[LCD_W * DRAW_BUF_ROWS];
static volatile bool s_first_flush_done;

static void disp_flush_done(void *ctx) {
    lv_display_flush_ready((lv_display_t *)ctx);
    s_first_flush_done = true;
}

static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    lcd_flush_area(area->x1, area->y1, area->x2, area->y2, px_map);
}

void lv_port_disp_init(void) {
    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, disp_flush_cb);
    lv_display_set_buffers(disp, s_buf1, s_buf2, sizeof(s_buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lcd_set_flush_done_cb(disp_flush_done, disp);
}

bool lv_port_disp_wait_first_flush(uint32_t timeout_ms) {
    TickType_t started = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ticks == 0) timeout_ticks = 1;
    while (!s_first_flush_done) {
        if ((xTaskGetTickCount() - started) >= timeout_ticks) return false;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return true;
}

bool lv_port_disp_first_flush_done(void) {
    return s_first_flush_done;
}
