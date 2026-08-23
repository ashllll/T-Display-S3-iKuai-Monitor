# Changelog / 变更日志

## 2026-08-23 — Rendering, trust state, and navigation hardening

### 中文

- 焦点吞吐主数字从 LVGL 默认 4bpp Montserrat 48 改为项目原生 2bpp 清晰数字字体，避免低像素密度屏上的灰边。
- 首帧 flush 增加 1 秒超时；成功后才开背光，超时时保持黑屏并继续等待迟到的完成回调，避免花屏和 UI 任务永久等待。
- 将 `WARMING/LIVE/STALE/TIMEOUT/OFFLINE/DEMO/NO DATA` 收敛为统一数据可信度模型；`STALE` 使用橙色提醒，不再冒充红色故障。
- 吞吐单位增加 768/1152 回切迟滞，降低阈值附近 `KB/s` 与 `MB/s` 反复切换。
- 将两个板载按键改为固定方向：BOOT/GPIO0 上一页、IO14/GPIO14 下一页；移除多击窗口与按压时长判断，30ms 消抖后立即响应，十页系统最多五步可达。
- 修复切页时上一页残留：移除旧页/新页同时平移的 190ms 动画，改为隐藏全部旧页、显示目标页并整屏失效，适配 LVGL partial buffer。

### English

- Replaced the focus throughput value's built-in 4bpp Montserrat 48 font with the project's native crisp 2bpp metric font.
- Added a one-second first-flush timeout. The backlight turns on only after a completed transfer; timeouts stay dark while waiting for a late completion instead of blocking forever.
- Centralized `WARMING/LIVE/STALE/TIMEOUT/OFFLINE/DEMO/NO DATA` as data-trust states; stale data now uses warning orange rather than fault red.
- Added 768/1152 unit hysteresis to prevent repeated `KB/s` and `MB/s` switching near thresholds.
- Assigned the two onboard buttons dedicated directions: BOOT/GPIO0 for previous and IO14/GPIO14 for next. Removing multi-click and hold timing makes navigation respond immediately after 30 ms debounce while keeping every page within five steps.
- Fixed previous-page ghosts during navigation by removing the 190 ms two-page translate animation and switching atomically to one visible page followed by a full-screen invalidation for the LVGL partial buffer.

### Validation boundary / 验证边界

The live-data build enabled `APP_REQUIRE_LIVE_MODE`, which would fail compilation if `APP_DEMO_MODE` were enabled. PlatformIO completed at 67.3% RAM and 37.0% Flash. USB-JTAG verified the bootloader, partition table, and application independently, then runtime logs confirmed 8 MB PSRAM, `LCD ready`, the UI task on core 1, Wi-Fi/IP acquisition, an iKuai HTTP 200 response, and a healthy 30-second heap/curve report. No first-flush timeout, panic, assert, or reboot appeared. Cold-boot flower-screen removal, font sharpness, and both physical button directions still require direct physical-screen observation.

## 2026-08-21 — Data trust and recovery UX / 数据可信度与恢复体验

### 中文

- 为 system、WAN、客户端排行和无线 AC 数据分别记录成功时间；超过 10 秒显示 `STALE`，不再把旧缓存伪装成实时数据。
- 兼容 iKuai 不同固件的速率单位：原始瞬时值作为兜底，只有累计计数差分关系合理时才换算，避免 1024 倍误差；客户端排行同步使用检测到的单位。
- 将 Ping 文案统一为 `GW`，明确显示的是本地网关延迟；网络健康页新增路由不可达、API 超时、Token/认证失败和 HTTP 错误提示。
- Wi‑Fi 断线改为指数退避重连（最长 30 秒），网络变化时重建网关 Ping 会话并清空旧速率基线。
- 单击/双击判定窗口从 350ms 缩短为 260ms，并短暂闪亮页码点确认按键；背光映射扩展到 0–100%，模板日间 45%、夜间 12%，主页显示 `NIGHT DIM`。
- 实机复核：成功烧录到 T-Display-S3，8MB PSRAM 测试通过，获取真实 Wi‑Fi 地址、启动网关 Ping、iKuai HTTPS 返回 200；随后监听 25 秒无重启、assert、panic 或 abort。

### English

- Added independent freshness timestamps for system, WAN, top clients, and wireless AC data; values become `STALE` after 10 seconds instead of looking live when cached.
- Added iKuai rate-unit protection: raw instantaneous fields remain the fallback and counter-derived rates are accepted only when their unit relationship is plausible, preventing a silent 1024x error. Client ranking uses the detected scale too.
- Renamed ping labels to `GW` to identify local-gateway latency; the network-health page now distinguishes unreachable router, API timeout, token/auth failure, and HTTP error.
- Wi-Fi recovery now uses exponential backoff up to 30 seconds; a network change recreates the gateway-ping session and resets old rate baselines.
- Reduced the single/double-click decision window from 350ms to 260ms and briefly flashes the active page dot as feedback. Backlight mapping now accepts 0–100%; the template uses 45% day and 12% night levels, with `NIGHT DIM` shown on the home footer.
- Physical recheck: flashed to T-Display-S3, passed the 8 MB PSRAM test, obtained a real Wi-Fi address, started gateway ping, and received HTTP 200 from the live iKuai API; a further 25-second monitor showed no reboot, assert, panic, or abort.

### Validation boundary / 验证边界

The serial run confirms boot, networking, API consumption, and runtime stability. Final judgment of font sharpness, brightness, and every page transition still requires direct observation of the physical display.

## 2026-08-21 — UI layout and physical-link validation

### 中文

- 修复 CPU/MEM 数值变化时 `%` 单位位置不随文本宽度更新的问题，避免单位漂移和重叠。
- 保留速率单位的动态宽度对齐，统一小屏上的数字、单位和胶囊标签基线。
- 更新中英文根 README 与固件 README：明确十页固件结构、UI SVG 预览、背光默认值和 15 FPS 曲线行为。
- 新增本次实机验证记录：固件成功编译并烧录到 T-Display-S3，8MB PSRAM 测试通过，Wi‑Fi 关联成功，真实 iKuai HTTPS API 返回成功，监控期间未观察到重启。
- 私有 Wi‑Fi、iKuai token、证书、串口路径和本地生成配置继续保持在 Git 之外。

### English

- Fixed CPU/MEM `%` suffix placement when the value width changes, preventing drift and overlap.
- Kept width-aware alignment for rate units so values, units, and capsule labels share a stable small-screen baseline.
- Updated the bilingual root README and firmware README with the ten-page structure, UI SVG previews, backlight defaults, and current 15 FPS curve behavior.
- Added physical validation evidence: the firmware built and flashed to a T-Display-S3, the 8 MB PSRAM test passed, Wi‑Fi associated, the live iKuai HTTPS API returned successfully, and no reboot was observed during the monitored run.
- Private Wi‑Fi settings, iKuai tokens, certificates, serial paths, and generated local configuration remain outside Git.

### Validation boundary / 验证边界

Build, serial, and API evidence confirm the firmware is running on the target board and can consume live telemetry. Final visual acceptance of every page—including display-angle readability and all transition states—still requires direct observation of the physical screen.
