#pragma once

#include <stdint.h>

// LILYGO T-Display-S3 — ST7789 170x320 并口(i80)屏（LVGL 底层）
// 横屏显示：320 宽 x 170 高（swap_xy 旋转 90 度）
#define LCD_W 320
#define LCD_H 170

void lcd_init(void);
void lcd_set_backlight(uint8_t pct);   // 0~100%，底层映射到屏幕 16 级背光

// The brightness axis really is 16 discrete steps (a pulse counter on GPIO38, not a
// PWM duty), so a fade has to be counted in levels. Travel is one pulse per level
// downward and fifteen per level upward, which makes only the dimming direction
// gradual: lcd_set_backlight_level() one level at a time.
void lcd_set_backlight_level(uint8_t level);
uint8_t lcd_backlight_level(void);     // 0 = off, otherwise 1..lcd_backlight_levels()
uint8_t lcd_backlight_levels(void);
uint8_t lcd_backlight_level_of_pct(uint8_t pct);

// 推送一个矩形区域到屏幕；data 为 RGB565 字节流（MSB-first，即高字节在前），
// 每行从左到右连续。底层为 esp_lcd i80 并口，整块直推。
void lcd_flush_area(int x0, int y0, int x1, int y1, const void *data);

typedef void (*lcd_flush_done_cb_t)(void *ctx);
void lcd_set_flush_done_cb(lcd_flush_done_cb_t cb, void *ctx);
// Frames pushed to the panel since boot; the heartbeat derives fps from it.
uint32_t lcd_frame_count(void);
