#pragma once

// ── 配置模板：复制为 src/config.h 并填写真实值 ──────────────────────
// src/config.h 已在 .gitignore 中，不会进入版本库

// 1 = 离线演示数据，不连接 Wi-Fi/iKuai；0 = 使用下面的真实配置。
// 允许正式构建环境通过 -DAPP_DEMO_MODE 强制选择模式。
#ifndef APP_DEMO_MODE
#define APP_DEMO_MODE 1
#endif

// 1 = 减少页面滑动和流星动效，保留曲线滚动；0 = 标准动效
#define APP_REDUCED_MOTION 1

#define APP_WIFI_SSID "YOUR_SSID"
#define APP_WIFI_PASS "YOUR_PASSWORD"

#define APP_TZ "CST-8"

#define APP_BL_PCT 45

// 夜间自动降背光(SNTP 授时后生效;手动息屏优先)
// 时段跨零点也可,例如 23~7 表示 23:00~次日 07:00
#define APP_NIGHT_START 23
#define APP_NIGHT_END   7
#define APP_BL_NIGHT_PCT 12

// 曲线固定归一化上限：上下行单位为 B/s，默认 125,000,000 B/s = 1 Gbit/s。
// PING 上限单位为 ms。按实际链路能力覆盖这些值不会改变原始数据或 PEAK 文案。
#define APP_CURVE_DOWN_CEILING_BPS 125000000U
#define APP_CURVE_UP_CEILING_BPS   125000000U
#define APP_CURVE_PING_CEILING_MS  200.0f

// ── iKuai 路由器监视 ───────────────────────────────────────────────
// token 获取：~/.ikuai-cli/config.json（ikuai-cli auth set-token）
#define IKUAI_HOST  "https://192.0.2.1"
#define IKUAI_TOKEN "REPLACE_WITH_IKUAI_TOKEN"
