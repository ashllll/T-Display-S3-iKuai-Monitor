/**
 * LVGL v9.2.2 配置 — LILYGO T-Display-S3 rp5 网关监控屏
 * 170x320 RGB565 横屏；只保留本 UI 使用的 Label/Canvas 等组件
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/*==================== 颜色 ====================*/
#define LV_COLOR_DEPTH 16
#define LV_BIG_ENDIAN_SYSTEM 0          /* 小端 CPU；面板字节序在 flush 回调里交换 */

/*==================== 标准库 ====================*/
#define LV_USE_STDLIB_MALLOC   LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING   LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF  LV_STDLIB_CLIB

#define LV_MEM_SIZE (64U * 1024U)
#define LV_DRAW_LAYER_SIMPLE_BUF_SIZE (8 * 1024)  /* 减小临时 layer 缓冲的 heap 波动（默认 24KB） */
#define LV_MEM_POOL_EXPAND_SIZE (32U * 1024U)

/*==================== 刷新/DPI ====================*/
#define LV_DEF_REFR_PERIOD 16          /* 60 Hz 请求；原来 33 = 30 fps 硬上限 */
#define LV_DPI_DEF 130

/*==================== 操作系统 ====================*/
#define LV_USE_OS LV_OS_NONE            /* 单任务驱动 lv_timer_handler */

/*==================== 渲染器 ====================*/
#define LV_USE_DRAW_SW 1
#define LV_DRAW_SW_SHADOW 0              /* UI 不使用阴影 */
#define LV_DRAW_SW_COMPLEX_GRADIENTS 0   /* UI 不使用复杂渐变 */

/*==================== 日志 ====================*/
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

/*==================== 断言（全关省 Flash） ====================*/
#define LV_USE_ASSERT_NULL 0
#define LV_USE_ASSERT_MALLOC 0
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ 0

/*==================== 字体 ====================*/
#define LV_FONT_MONTSERRAT_12 0
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_MONTSERRAT_32 0
#define LV_FONT_MONTSERRAT_36 0
#define LV_FONT_MONTSERRAT_48 0
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/*==================== 控件（只留 Label） ====================*/
#define LV_USE_ANIMIMG    0
#define LV_USE_ARC        0
#define LV_USE_BAR        0
#define LV_USE_BUTTON     0
#define LV_USE_BUTTONMATRIX 0
#define LV_USE_CALENDAR   0
#define LV_USE_CANVAS     1
#define LV_USE_CHART      1
#define LV_USE_CHECKBOX   0
#define LV_USE_DROPDOWN   0
#define LV_USE_IMAGE      1   /* Canvas 内部依赖 image 类型 */
#define LV_USE_IMAGEBUTTON 0
#define LV_USE_KEYBOARD   0
#define LV_USE_LABEL      1
#define LV_USE_LED        0
#define LV_USE_LINE       0
#define LV_USE_LIST       0
#define LV_USE_LOTTIE     0
#define LV_USE_MENU       0
#define LV_USE_MSGBOX     0
#define LV_USE_ROLLER     0
#define LV_USE_SCALE      0
#define LV_USE_SLIDER     0
#define LV_USE_SPAN       0
#define LV_USE_SPINBOX    0
#define LV_USE_SPINNER    0
#define LV_USE_SWITCH     0
#define LV_USE_TEXTAREA   0
#define LV_USE_TABLE      0
#define LV_USE_TABVIEW    0
#define LV_USE_TILEVIEW   0
#define LV_USE_WIN        0

/*==================== 主题/布局 ====================*/
#define LV_USE_THEME_DEFAULT 1
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO 0
#define LV_USE_FLEX 0
#define LV_USE_GRID 0

/*==================== 杂项 ====================*/
#define LV_USE_SYSMON 0                 /* 要量 FPS 时：SYSMON 1 + PERF_MONITOR 1 + LOG_MODE 1 */
#define LV_USE_PERF_MONITOR 0
#define LV_USE_PERF_MONITOR_POS LV_ALIGN_BOTTOM_RIGHT
#define LV_USE_PERF_MONITOR_LOG_MODE 0  /* 0 = 屏上叠；1 = 只打串口日志 */
#define LV_USE_PROFILER 0
#define LV_USE_FONT_PLACEHOLDER 0       /* 缺字不画方块；UI 文案保持 ASCII */

#endif /* LV_CONF_H */
