#pragma once

#include <stdint.h>
#include <stdbool.h>

// rp5-agent collector: one read-only HTTP GET per second against the gateway's
// status endpoint (see agent/rp5-agent.py). Rates arrive in bytes/sec, already
// differenced by the agent; the device only stores and displays them.

// Retain the full 120-second chart at 1 Hz, including boundary samples.
#define RP5_CURVE_MAX 128

typedef enum {
    RP5_LINK_WAIT = 0,
    RP5_LINK_ONLINE,
    RP5_LINK_NETWORK_ERROR,
    RP5_LINK_TIMEOUT,
    RP5_LINK_HTTP_ERROR,
    RP5_LINK_CONFIG_ERROR,
    RP5_LINK_STALE,
} rp5_link_state_t;

// Raspberry Pi itself. *valid is false when the agent could not read a field
// (the display draws "—" instead of a stale or invented number).
typedef struct {
    bool ok;
    uint32_t ts;
    bool agent_valid;
    uint32_t agent_up_s;
    bool ex_valid;      // node_exporter answer present
    bool ex_ok;         // node_exporter scrape succeeded
    uint32_t ex_ms;
    bool cpu_valid;
    float cpu_pct;
    bool mem_valid;
    float mem_pct;
    bool temp_valid;
    float temp_c;
    bool load_valid;
    float load1, load5, load15;
    bool uptime_valid;
    uint32_t uptime_s;
    bool disk_valid;
    float disk_pct;
    bool freq_valid;
    uint32_t freq_mhz;
} rp5_sys_t;

// mihomo proxy state.
typedef struct {
    bool ok;
    uint32_t ts;
    char mode[8];
    bool node_valid;
    char node[48];
    bool delay_valid;
    uint32_t delay_ms;
    uint32_t conns;
    uint32_t down_bps, up_bps;      // proxy throughput, bytes/sec
    uint64_t down_total, up_total;
} rp5_proxy_t;

// Gateway interfaces, bytes/sec.
typedef struct {
    bool ok;
    uint32_t ts;
    uint32_t wan_rx, wan_tx;
    uint32_t lan_rx, lan_tx;
    uint32_t px_rx, px_tx;
    bool leases_valid;
    uint32_t leases;
} rp5_gw_t;

typedef struct {
    bool ok;
    uint32_t ts;
    int head;                          // 下一个写入位置
    int n;                             // 已填充点数（<= RP5_CURVE_MAX）
    uint32_t sample_ts[RP5_CURVE_MAX];
    uint32_t down[RP5_CURVE_MAX];      // wan0 RX, B/s
    uint32_t up[RP5_CURVE_MAX];        // wan0 TX, B/s
} rp5_curve_t;

void rp5_monitor_start(void);
bool rp5_get_sys(rp5_sys_t *out);
bool rp5_get_proxy(rp5_proxy_t *out);
bool rp5_get_gw(rp5_gw_t *out);
bool rp5_get_curve(rp5_curve_t *out);
bool rp5_recently_ok(void);
rp5_link_state_t rp5_get_link_state(void);
void rp5_monitor_network_changed(void);
