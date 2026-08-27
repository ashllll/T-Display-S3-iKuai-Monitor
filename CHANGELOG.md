# Changelog / 变更日志

## 2026-08-26 — Router load and extended telemetry

### 中文

- 第 6 页由 ESP32 本机调试信息改为路由负载：总连接数、TCP/UDP/ICMP、在线客户端、网关丢包与抖动。
- 网络健康页新增 60 点窗口丢包率、平均延迟和相邻有效样本抖动，并按证据区分丢包、高延迟和链路不稳。
- WAN 页合并 `iface_check` 与 `iface_stream`，显示分线路实时上下行、连接数、累计量和收发丢包；未配置的第二 WAN 显示 `UNUSED`，不再误报离线。
- 终端排行增加上传速率；路由健康页改为路由累计 RX/TX；无线页增加活跃、空闲和历史峰值终端数。
- 扩展数据仍每 3 秒只请求一类，同一数据源约 9 秒更新；可信窗口据此调整为 20 秒，并修复 AC 统计失败时旧数据被重新标记为实时的问题。
- 重绘第 7–10 页预览并同步完整十页 v2 总览；旧概念稿明确标记为 legacy，不再与当前固件页面合同混淆。
- 修复路由温度空数组被误当成 `0 C`：兼容数字/字符串数组；当前接口没有温度样本时明确显示 `NO DATA`，不以 ESP32 本机温度冒充路由器温度。
- 修复 Top Clients 因 JSON 字段顺序导致上下行恒为 0，并把 WAN、CPU/MEM、连接数、累计量、运行时间和版本字段改为独立有效性判定；字段缺失不再冒充真实零值。
- 全局 WAN 状态改由线路检查结果驱动，WAN 对象解析严格限制在各自 JSON 边界；AC 正常关闭使用灰色待机语义，背光在面板上电前即被主动拉低。

### English

- Replaced the ESP32 debug page with router load: total, TCP, UDP, and ICMP sessions, online clients, gateway loss, and jitter.
- Added 60-sample loss, average latency, and adjacent-valid-sample jitter evidence to Network Health, with distinct conclusions for loss, latency, and instability.
- Merged `iface_check` and `iface_stream` on the WAN page for per-line live rates, sessions, totals, and dropped packets; an unconfigured second WAN now reads `UNUSED` instead of false offline.
- Added upload rate to Top Clients, router cumulative RX/TX to Router Health, and active, idle, and peak client counts to Wireless AC.
- Kept one extended request every three seconds, yielding about nine seconds per source; adjusted freshness to 20 seconds and fixed stale AC statistics being relabeled live after a failed statistics response.
- Redrew the pages 7–10 preview and synchronized the complete ten-page v2 overview; the older concept sheet is explicitly marked legacy instead of posing as the firmware contract.
- Fixed an empty router-temperature array being misread as `0 C`: numeric and quoted arrays are accepted, while a missing sample now reads `NO DATA` rather than substituting the ESP32's own temperature.
- Fixed Top Clients rates being forced to zero by JSON key order, and added independent validity contracts for WAN, CPU/memory, session counts, totals, uptime, and version fields so missing data cannot masquerade as a real zero.
- Drove global WAN state from line-check results, bounded every WAN parse to its own JSON object, rendered an intentionally disabled AC as idle instead of failed, and asserted backlight-off before panel power-up.

### Validation boundary / 验证边界

PlatformIO completed with 90,948 bytes RAM and 1,173,661 bytes Flash. OpenOCD selected the Espressif USB-JTAG device with serial `AC:A7:04:F6:68:D4`, independently wrote and verified bootloader, partition table, and the Live application, then reset the board. The reset capture passed the 8 MB PSRAM test, recovered from expected pre-DHCP connection attempts to HTTP 200, and anonymously confirmed three client rows, AC-off state, and two WAN status/stream pairs. A following steady-state sample reached `curve_fps=15.0`, with 86,879 bytes internal RAM free and a 31,744-byte largest block; no panic, assert, abort, or reboot appeared. Direct observation is still required for clipping, semantic colors, and rapid page switching on pages 6–10. Router addresses, client names, and credentials are not logged.

## 2026-08-26 — Real-time trend clock and PSRAM headroom

### 中文

- 曲线弹簧保持固定 15 Hz 临界阻尼步长，横轴按真实经过时间累计；LVGL 回调偶发延迟时补齐列数，使 300px 全宽趋势仍对应约 20 秒。
- 将五个 CPU 绘制用 Canvas 历史缓冲区和 8KB HTTP 响应缓冲区显式迁移到 PSRAM；LCD i80 DMA 缓冲继续保留在内部 RAM。
- HEAP 页面和 30 秒日志改为报告真正稀缺的内部 RAM、最小值与最大连续块，不再让 8MB PSRAM 掩盖内部内存压力。
- JSON 轻量解析器的固定搜索窗口收口到响应字符串终点，截断或格式变化时不再构造越过 NUL 的窗口。

### English

- Kept the curve spring on a fixed 15 Hz critically damped step while accumulating real elapsed time for horizontal columns, preserving the approximately 20-second meaning of the 300 px trend under LVGL callback jitter.
- Moved five CPU-rendered canvas-history buffers and the 8 KB HTTP response buffer explicitly to PSRAM while keeping the LCD i80 DMA buffers in internal RAM.
- Changed the HEAP page and 30-second telemetry to report scarce internal RAM, its minimum, and largest contiguous block instead of hiding pressure behind the 8 MB PSRAM total.
- Clamped the lightweight JSON parser's fixed search windows to each response's terminating NUL so truncated or changed payloads cannot create out-of-string boundaries.

### Validation boundary / 验证边界

USB-JTAG flashed and independently verified the bootloader, partition table, and Live application, then reset the board. Runtime logs confirmed the 8 MB PSRAM test, the 8 KB response buffer in PSRAM, Wi-Fi/IP acquisition, a live iKuai HTTP 200 response, and a steady second-window `curve_fps=15.0`. Internal free RAM settled at 87,767 bytes with a 31,744-byte largest block; the 40-second steady-state capture showed no error, panic, assert, or reboot. Initial HTTP connection errors occurred before DHCP completed and recovered automatically. Direct observation is still required for startup artifacts, button feedback, rapid page switching, and any visible two-column catch-up stutter.

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
