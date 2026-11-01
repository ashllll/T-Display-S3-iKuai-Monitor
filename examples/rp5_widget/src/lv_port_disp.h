#pragma once

#include <stdbool.h>
#include <stdint.h>

// LVGL 显示适配：170x320 RGB565，双缓冲，flush 到 ST7789(i80)
void lv_port_disp_init(void);
bool lv_port_disp_wait_first_flush(uint32_t timeout_ms);
bool lv_port_disp_first_flush_done(void);
// Rendered frames (one per LVGL refresh that actually had dirty areas), unlike
// lcd_frame_count(), which counts i80 transfers and rises with the number of bands.
uint32_t lv_port_disp_frame_count(void);
