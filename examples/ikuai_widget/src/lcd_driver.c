// LILYGO T-Display-S3 — ST7789 并口(i80)驱动（esp_lcd）
// 170x320 @ 8-bit i80；面板 BGR，开颜色反转；横屏 = swap_xy + mirror_x
//
// ⚠ 重点注意事项（来自 LILYGO 官方）：
// 1. GPIO15 是外设总电源，必须先置高，否则 LCD/背光都不工作
// 2. 该屏不是 SPI，是 8-bit 并口：D0-D7=39/40/41/42/45/46/47/48，WR=8，DC=7，CS=6，RST=5
// 3. 背光 GPIO38；整机 16MB Flash(QIO 80MHz) + 8MB OPI PSRAM（见 sdkconfig.defaults）
#include "lcd_driver.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_rom_sys.h"
#include "esp_log.h"

static const char *TAG = "lcd";

#define PIN_PWR   15   // 外设总电源（必须先拉高！）
#define PIN_BL    38
#define PIN_RST   5
#define PIN_CS    6
#define PIN_DC    7
#define PIN_WR    8
#define PIN_RD    9
static const int s_data_pins[8] = { 39, 40, 41, 42, 45, 46, 47, 48 };

#define PCLK_HZ     (16 * 1000 * 1000)
#define BL_MAX_PCT  100   // 16 级背光映射到 0~100%，应用可按环境调节

// 官方 T-Display-S3 工厂程序使用竖屏 GRAM 偏移 (0, 35)，
// 再通过 swap_xy 转为横屏 320x170。
#define X_GAP 0
#define Y_GAP 35

static esp_lcd_panel_handle_t s_panel;
static lcd_flush_done_cb_t s_flush_done_cb;
static void *s_flush_done_ctx;

typedef struct {
    uint8_t cmd;
    uint8_t data[14];
    uint8_t len;
} st7789_cmd_t;

// T-Display-S3 新版屏幕需要这组 ST7789V 扩展参数才能正常出图。
// 该序列与 LilyGO 官方 factory 程序保持一致。
static const st7789_cmd_t s_st7789_init[] = {
    {0x11, {0}, 0x80},
    {0x3A, {0x05}, 1},
    {0xB2, {0x0B, 0x0B, 0x00, 0x33, 0x33}, 5},
    {0xB7, {0x75}, 1},
    {0xBB, {0x28}, 1},
    {0xC0, {0x2C}, 1},
    {0xC2, {0x01}, 1},
    {0xC3, {0x1F}, 1},
    {0xC6, {0x13}, 1},
    {0xD0, {0xA7}, 1},
    {0xD0, {0xA4, 0xA1}, 2},
    {0xD6, {0xA1}, 1},
    {0xE0, {0xF0, 0x05, 0x0A, 0x06, 0x06, 0x03, 0x2B,
            0x32, 0x43, 0x36, 0x11, 0x10, 0x2B, 0x32}, 14},
    {0xE1, {0xF0, 0x08, 0x0C, 0x0B, 0x09, 0x24, 0x2B,
            0x22, 0x43, 0x38, 0x15, 0x16, 0x2F, 0x37}, 14},
};

static bool lcd_color_trans_done(esp_lcd_panel_io_handle_t io,
                                 esp_lcd_panel_io_event_data_t *edata,
                                 void *user_ctx) {
    (void)io;
    (void)edata;
    (void)user_ctx;
    if (s_flush_done_cb) s_flush_done_cb(s_flush_done_ctx);
    return false;
}

void lcd_set_flush_done_cb(lcd_flush_done_cb_t cb, void *ctx) {
    s_flush_done_cb = cb;
    s_flush_done_ctx = ctx;
}

// ─── Backlight (GPIO38 脉冲式 16 级背光芯片) ─────────────────────────

static uint8_t s_bl_level;

static void bl_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(PIN_BL, 0);
    s_bl_level = 0;
}

void lcd_set_backlight(uint8_t pct) {
    if (pct > BL_MAX_PCT) pct = BL_MAX_PCT;

    // GPIO38 is connected to the board's 16-step backlight controller. The
    // first high level wakes it; subsequent low/high pulses select a level.
    uint8_t target = (uint8_t)((pct * 16u + 50u) / 100u);
    if (target > 16) target = 16;
    if (target == 0) {
        gpio_set_level(PIN_BL, 0);
        vTaskDelay(pdMS_TO_TICKS(3));
        s_bl_level = 0;
        return;
    }
    if (s_bl_level == 0) {
        gpio_set_level(PIN_BL, 1);
        esp_rom_delay_us(30);
        s_bl_level = 16;
    }
    uint8_t pulses = (uint8_t)((16u + s_bl_level - target) % 16u);
    for (uint8_t i = 0; i < pulses; i++) {
        gpio_set_level(PIN_BL, 0);
        gpio_set_level(PIN_BL, 1);
    }
    s_bl_level = target;
}

// ─── 区域推送：i80 整块直推（无 SPI 分块限制）────────────────────────

void lcd_flush_area(int x0, int y0, int x1, int y1, const void *data) {
    esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1 + 1, y1 + 1, data);
}

// ─── Init ────────────────────────────────────────────────────────────

void lcd_init(void) {
    ESP_LOGI(TAG, "ST7789 i80 init (170x320)");

    // 1) 外设总电源：必须最先拉高（官方重点注意事项 #1）
    gpio_config_t pwr = {
        .pin_bit_mask = 1ULL << PIN_PWR,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pwr);
    gpio_set_level(PIN_PWR, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    // RD is unused by the write-only i80 driver, but the LCD controller
    // expects this input high rather than floating.
    gpio_config_t rd = {
        .pin_bit_mask = 1ULL << PIN_RD,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rd);
    gpio_set_level(PIN_RD, 1);

    // 2) i80 总线
    esp_lcd_i80_bus_handle_t i80_bus;
    esp_lcd_i80_bus_config_t bus_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num = PIN_DC,
        .wr_gpio_num = PIN_WR,
        .data_gpio_nums = {
            s_data_pins[0], s_data_pins[1], s_data_pins[2], s_data_pins[3],
            s_data_pins[4], s_data_pins[5], s_data_pins[6], s_data_pins[7],
        },
        .bus_width = 8,
        .max_transfer_bytes = LCD_W * 40 * sizeof(uint16_t) + 16,
        .psram_trans_align = 64,
        .sram_trans_align = 4,
    };
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_cfg, &i80_bus));

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = PIN_CS,
        .pclk_hz = PCLK_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = lcd_color_trans_done,
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 1,
        },
        // Swap only while the i80 peripheral sends the pixels. LVGL keeps
        // its RGB565 buffers in CPU-native order for safe partial reuse.
        .flags = {
            .swap_color_bytes = 1,
        },
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_cfg, &io));

    // 3) ST7789 面板：官方横屏方向为 swap_xy + mirror_y + GRAM 偏移
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, X_GAP, Y_GAP));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));   // 横屏 320x170
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, true));
    // Apply the board-specific ST7789V extension registers before enabling
    // the panel output.  The backlight remains off until the first LVGL frame
    // is transferred, so no uninitialised GRAM is visible during boot.
    for (size_t i = 0; i < sizeof(s_st7789_init) / sizeof(s_st7789_init[0]); i++) {
        const st7789_cmd_t *cmd = &s_st7789_init[i];
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, cmd->cmd, cmd->data, cmd->len & 0x7F));
        if (cmd->len & 0x80) vTaskDelay(pdMS_TO_TICKS(120));
    }

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    bl_init();
    ESP_LOGI(TAG, "LCD ready");
}
