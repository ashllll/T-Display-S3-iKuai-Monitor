# T-Display-S3 iKuai 网络监控屏 / iKuai Network Monitor

面向 T-Display-S3（非触摸版）的桌面监控摆件固件：LVGL 界面实时显示 iKuai 路由器 WAN 状态、上下行速率与滚动趋势曲线。UI 采用酷态科（CUKTECH）小屏设计语言：纯黑底 + 像素对齐数字 + 彩色胶囊标签 + 无网格渐变曲线；主页和速率焦点页均使用不缩放的 36px 数字字体，避免插值模糊。

本目录是 T-Display-S3 的独立监控固件入口，面向板载 ST7789 屏幕以及 GPIO0、GPIO14 两个物理按键。

## 最新 UI 更新 / Latest UI update

CPU/MEM 百分比现在会在数值从 `--`、个位数变化到三位数时重新锚定 `%` 单位，避免单位漂移、重叠或与数字脱节。速率单位也按当前值宽度重新对齐；本版同时保留大号原生数字和单一焦点排版，优先保证真实小屏可读性。WAN、客户端排行和无线 AC 还会按各自时间戳显示 `STALE`，避免网络异常时误读缓存。

When CPU or memory values change width—from `--` to one, two, or three digits—the `%` suffix is re-anchored after each update. This prevents drift and overlap on the physical 320×170 display. Rate units use the same width-aware alignment, while the large native-number layout remains the readability baseline.
Each extended data source now has its own freshness timestamp and shows `STALE` when it expires. Realtime throughput uses iKuai's `stream.download` and `stream.upload` fields directly; cumulative `total_down` and `total_up` are not differenced into a second rate, so the monitor stays aligned with the router's own rate display. The UI formats the direct value as `B/s`, `KB/s`, or `MB/s`. Gateway latency is labeled `GW` so it is not mistaken for an Internet speed test.

## 十页设计与显示目标

![CUKTECH 十页界面设计稿 v2](../../designs/cuktech-ui-concepts/cuktech-ui-10-versions-v2.svg)

设计稿中的视觉方向已整理成一套连续页面，而不是十套互相冲突的主题：高频总览、下行焦点、双通道、趋势和网络健康位于前五页；设备与路由管理信息位于后五页。

## 页面与按键

设备为**十页双键翻页**结构（酷态科“一屏一焦点”原则），底部居中 10 个页码点（黄=当前页）：

| 页 | 内容 | 数据源（iKuai v4.0 API） |
|---|---|---|
| 1 | 主页数据墙：WAN 状态 / PING 胶囊 / 上下行大数字 / 三色趋势曲线 | monitoring/system（1s） |
| 2 | 速率焦点页：原生 48px 总下行速率 + UP 副行 | 同上 |
| 3 | 双通道页：上下行双大数字 + 两条独立微型趋势曲线 | 同上 |
| 4 | 趋势优先页：300×108 无网格三色滚动曲线 + 当前数值 | 同上 + ICMP ping |
| 5 | 网络健康页：WAN 状态结论 + PING/上下行证据 + 告警建议 | 同上 + 本地 Wi-Fi 状态 |
| 6 | 路由负载页：总连接数 + TCP/UDP/ICMP + 客户端/丢包/抖动 | monitoring/system + ICMP |
| 7 | 路由健康页：CPU/内存 + 温度 + 运行时间 + 累计 RX/TX + 版本 | monitoring/system 附带字段（零额外请求） |
| 8 | WAN 线路详情页：状态 + 实时 D/U + 连接数 + 丢包 + 公网 IP | monitoring/interfaces-status（同源约 9s 更新） |
| 9 | 终端流量排行页：Top3 分别显示实时下载与上传 | monitoring/clients-online（同源约 9s 更新） |
| 10 | 无线 AC 状态页：AC/AP/2.4G·5G + 活跃/空闲/峰值 | network/ac/services + monitoring/wireless-statistics（同源约 9s 更新） |

> 扩展端点采用**轮转制**：每 3 秒只发一个额外请求（WAN → 终端 → AC 循环），所以同一数据源约 9 秒更新一次，连续 20 秒未成功刷新则显示 `STALE`；AC 未开启时第 10 页显示 OFF 且不再请求无线统计。

**BOOT 键（GPIO0）= 上一页；IO14 键（GPIO14）= 下一页。** 两个按键各自只负责一个方向，按下经 30ms 消抖后立即响应，不等待双击窗口，也不区分短按/长按。双向翻页把十页系统的最坏导航成本限制在 5 次，当前页码点会短暂闪亮作为反馈。**60 秒无操作自动回主页**。

## 动态行为（酷态科语言的灵魂细节）

- **静默降灰 / 活跃点亮**：单方向流量 < 48KB/s 时，对应大数字和单位自动降为深灰 `#4A5260`（30号站待机页精神）；有流量即点亮
- **PING 胶囊变色**：正常 = 橙描边；延迟 ≥ 80ms 或超时 = 红色——不看数字先读色
- **下行曲线渐变填充**：30号站同款 sparkline，亮线 + 向下渐淡填充，不覆盖其他轨道
- **焦点页峰值标记**：会话峰值以 `PEAK x.x MB/s *` 黄字常驻（模式 C 规格）
- **原子换页**：切页先清除上一页的完整组合，再显示目标页并整屏刷新，避免 partial buffer 屏幕留下前一页残影
- **焦点页流星汇入**：仅在流量明显变化时短暂出现 6 颗青/蓝流星，避免持续粒子遮挡主数字；设置 `APP_REDUCED_MOTION=1` 可关闭流星动效
- **夜间静音模式**：SNTP 授时（ntp.aliyun.com，CST-8）后，23:00–07:00 背光自动降到 `APP_BL_NIGHT_PCT`（模板默认 12%），主页显示 `NIGHT DIM`，曲线保持 15 FPS，并关闭流星动效；时段可在 `config.h` 改，时间未同步时不动作

## 硬件

- **主控**：ESP32-S3R8（Wi-Fi / BLE，16MB Flash，8MB OPI PSRAM）
- **屏幕**：ST7789，170×320，**8-bit 并口（i80，非 SPI）**
- **按键**：BOOT（GPIO0）固定上一页；IO14（GPIO14）固定下一页

## ⚠ 重点注意事项（移植/烧录前必读）

1. **GPIO15 是外设总电源**：必须在初始化最前面置高，否则 LCD、背光全部不工作。已在 `lcd_init()` 首步处理，改动启动流程时不要删掉。
2. **屏幕是并口不是 SPI**：D0-D7 = GPIO 39/40/41/42/45/46/47/48，WR=8、DC=7、CS=6、RST=5、BL=38。驱动基于 `esp_lcd` i80，与 SPI 驱动不通用。
3. **sdkconfig 必须匹配**：Flash 16MB QIO 80MHz、PSRAM **OPI** 80MHz。已提供 `sdkconfig.defaults`，首次构建会自动生效；若用旧 sdkconfig 构建请先删除再重建。
4. **外部供电（非 USB-C）时必须禁用 USB CDC**，否则开机等待串口连接、程序不启动：取消 `platformio.ini` 中 `build_flags` 注释即可。
5. **烧录失败时手动进下载模式**：按住 BOOT → 按一下 RST → 松开 RST → 松开 BOOT，然后重新 upload。
6. **分辨率**：屏幕按横屏 320×170 适配（底部信息行紧贴下边缘，改布局时注意）。
7. **状态不依赖额外外设**：本固件只使用屏幕和板载 BOOT/IO14 按键；在线状态由屏幕状态点与文案显示。

## 功能

- **LVGL 监控面板**（横屏 320×170，背光最高限制 50%，模板日间默认 45%）：WAN 在线状态、在线设备数、网关 PING（橙色描边胶囊）、下行/上行实时速率大数字（像素对齐 36px 数字、青/蓝胶囊标签）、约 20 秒三色滚动趋势曲线（15 FPS，临界阻尼平滑）
- **iKuai 路由器数据源**：HTTPS + Bearer token + 固定证书，1 秒轮询
- **离线演示模式**：`APP_DEMO_MODE=1` 时使用模拟数据运行

## 构建 / Build

```bash
# 正式 PlatformIO 环境强制非 Demo；先创建本地真实配置
cp src/config.example.h src/config.h
cp src/ikuai_cert.example.h src/ikuai_cert.h
# 填写 src/config.h 与 src/ikuai_cert.h 后编译
pio run -e t-display-s3

# 烧录
pio run -e t-display-s3 -t upload
pio device monitor
```

`platformio.ini` 已将 `APP_DEMO_MODE=0` 和 `APP_REQUIRE_LIVE_MODE=1` 固定用于本环境。没有本地配置时，固件仍是实时模式，但只能显示连接等待/错误状态，不会显示模拟 Demo 数据。

`APP_REDUCED_MOTION=1` 可关闭流星动效，保留原子换页和曲线滚动，适合对动效敏感或需要更安静显示的场景。最新实机验证记录见仓库根目录 [`CHANGELOG.md`](../../CHANGELOG.md)。

趋势曲线使用固定上限归一化，静态遥测不会因自动缩放而自行移动。可在本地 `config.h` 中通过 `APP_CURVE_DOWN_CEILING_BPS`、`APP_CURVE_UP_CEILING_BPS` 和 `APP_CURVE_PING_CEILING_MS` 覆盖默认上限；速率单位始终是 B/s，默认 `125000000` B/s 等于 1 Gbit/s，PING 默认上限为 200 ms。焦点页 `PEAK` 仍取最近 60 个原始下载样本的最大值，不参与曲线缩放。

LVGL 9.2.2 由 `src/idf_component.yml` 管理，版本锁定在 `dependencies.lock`；`components/lv_conf.h` 是本板的精简配置。`src/config.h`、`src/ikuai_cert.h`、构建目录和生成的 `sdkconfig` 均为本地文件，不要提交。

## Build / English

The PlatformIO environment is fixed to live mode with `APP_DEMO_MODE=0` and `APP_REQUIRE_LIVE_MODE=1`. On a clean checkout, copy both example headers to `src/config.h` and `src/ikuai_cert.h`, fill in the local Wi‑Fi/router settings and CA certificate, then build and upload. Without those local files the firmware remains in live mode but can only show connection-wait/error states; it never uses simulated Demo data. Set `APP_REDUCED_MOTION` to `1` for a calmer display mode; the scheduled night mode keeps the 15 FPS curve and suppresses decorative motion.

Trend normalization uses stable configurable ceilings, so unchanged telemetry cannot move vertically because of autoscaling. Override `APP_CURVE_DOWN_CEILING_BPS`, `APP_CURVE_UP_CEILING_BPS`, and `APP_CURVE_PING_CEILING_MS` in local `config.h`; rate ceilings are always B/s (the default `125000000` B/s is 1 Gbit/s) and the default ping ceiling is 200 ms. The focus-page `PEAK` remains the maximum of the latest 60 raw download samples and is independent of animation scaling.

LVGL 9.2.2 is managed by `src/idf_component.yml` and pinned in `dependencies.lock`. The board-specific trimmed feature set lives in `components/lv_conf.h`. Local credentials, certificates, build output, and generated `sdkconfig` files are intentionally excluded from Git.

The current optimized image has been flashed to a physical T-Display-S3. USB-JTAG verified all three images, and runtime logs confirmed the 8 MB PSRAM test, the 8 KB HTTP response buffer in PSRAM, live Wi-Fi/iKuai API recovery, and steady `curve_fps=15.0`. Internal RAM settled at 86,879 bytes free with a 31,744-byte largest block. Anonymous parser evidence confirmed three client rows, the current AC-off state, and two WAN status/stream pairs. Physical rendering, startup artifacts, button feedback, clipping on pages 6–10, and rapid page switching still require a direct-screen check.

## 目录结构

```
src/
├── main.c                # 入口
├── desktop_widget.c/h    # LVGL 监控面板界面（酷态科设计语言）
├── ikuai_monitor.c/h     # iKuai 路由器监控
├── lcd_driver.c/h        # ST7789 i80 并口驱动（esp_lcd）
├── lv_port_disp.c/h      # LVGL 显示移植层
└── fonts/                # 像素对齐的 LVGL 标签与数字字体
```
