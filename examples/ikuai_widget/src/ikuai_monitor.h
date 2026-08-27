#pragma once

#include <stdint.h>
#include <stdbool.h>

// iKuai 路由器实时监视数据（https + Bearer token + 固定证书）
// - system 端点 1s 轮询（CPU/内存/在线数/实时上下行速率）
// - ICMP ping 网关 1s 采样（延迟曲线）
// - 60 点曲线缓存（1 秒/点，实时滚动窗口）

#define IKUAI_CURVE_MAX 60

typedef enum {
    IKUAI_LINK_WAIT = 0,
    IKUAI_LINK_ONLINE,
    IKUAI_LINK_NETWORK_ERROR,
    IKUAI_LINK_TIMEOUT,
    IKUAI_LINK_AUTH_ERROR,
    IKUAI_LINK_HTTP_ERROR,
} ikuai_link_state_t;

typedef struct {
    bool ok;
    uint32_t ts;
    bool cpu_valid;
    float cpu_pct;
    bool mem_valid;
    float mem_pct;
    bool online_valid;
    uint32_t online_cnt;
    bool conn_valid;
    uint32_t conn_cnt;
    bool tcp_conn_valid;
    uint32_t tcp_conn_cnt;
    bool udp_conn_valid;
    uint32_t udp_conn_cnt;
    bool icmp_conn_valid;
    uint32_t icmp_conn_cnt;
    uint32_t down_bps;      // iKuai stream.download 瞬时值，内部按 B/s 传递
    uint32_t up_bps;        // iKuai stream.upload 瞬时值，内部按 B/s 传递
    bool totals_valid;
    uint64_t total_down;    // 路由启动以来累计接收字节
    uint64_t total_up;      // 路由启动以来累计发送字节
} ikuai_sys_t;

typedef struct {
    bool ok;
    uint32_t ts;
    int head;                          // 下一个写入位置
    int n;                             // 已填充点数（<= 60）
    float ping_ms[IKUAI_CURVE_MAX];    // 网关延迟 ms
    uint32_t down[IKUAI_CURVE_MAX];    // B/s
    uint32_t up[IKUAI_CURVE_MAX];      // B/s
} ikuai_curve_t;

// ── 扩展信息（每 3s 发一类请求；三类轮转，同一数据源约 9s 更新）──
typedef struct {
    char name[12];
    char ip[16];
    char gateway[16];
    bool status_valid;
    int ok;
    bool conn_valid;
    uint32_t conn_cnt;
    bool rate_valid;
    uint32_t down_bps;
    uint32_t up_bps;
    uint64_t total_down;
    uint64_t total_up;
    uint64_t down_packets;
    uint64_t up_packets;
    bool drop_valid;
    uint64_t down_dropped;
    uint64_t up_dropped;
} ikuai_wan_t;

typedef struct {
    char name[24];
    char ip[16];
    char interface[12];
    char frequency[8];
    bool down_valid;
    uint32_t down_bps;
    bool up_valid;
    uint32_t up_bps;
    int signal_dbm;
} ikuai_client_t;

typedef struct {
    bool ok;
    uint32_t system_ts;             // system/health data last successful sample
    bool     cpu_temp_valid;  // cputemp[0] 是否存在且可解析；空数组不是 0°C
    float    cpu_temp;        // °C（system 端点 cputemp[0]，1s 更新）
    bool     uptime_valid;
    uint32_t uptime_sec;      // 路由运行时间（system 端点 uptime）
    bool     version_valid;
    char     version[16];     // 固件版本 verstring
    uint32_t wan_ts;                // interfaces-status last successful sample
    int  wan_cnt;             // WAN 线路数（interfaces-status，同源约 9s）
    ikuai_wan_t    wan[2];
    uint32_t clients_ts;            // clients-online last successful sample
    int  client_cnt;          // 下行排序 Top3（clients-online，同源约 9s）
    ikuai_client_t client[3];
    uint32_t ac_ts;                 // AC/wireless last successful sample
    int  ac_on;               // AC 功能开关（network/ac/services，同源约 9s）
    int  ap_online, ap_count; // AP 在线/总数（wireless-statistics）
    int  clt_2g, clt_5g;      // 2.4G/5G 终端数
    int  clt_active, clt_inactive, clt_peak;
} ikuai_extra_t;

bool ikuai_get_extra(ikuai_extra_t *out);

void ikuai_monitor_start(void);
bool ikuai_get_sys(ikuai_sys_t *out);
bool ikuai_get_curve(ikuai_curve_t *out);
bool ikuai_recently_ok(void);
ikuai_link_state_t ikuai_get_link_state(void);
void ikuai_monitor_network_changed(void);
