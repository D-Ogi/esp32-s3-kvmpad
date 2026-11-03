// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Wi-Fi STA link state machine (TASK-080, ADR-015, ADR-017). Pure C, no ESP-IDF include: the host tests drive
// it with events and a fake clock, the firmware glue (net_wifi.c) turns its actions into esp_wifi calls.
// It never holds a secret: credentials stay in the esp_wifi storage, the cache record has BSSID + channel only.
//
// Link: OFF -> NOT_CONFIGURED | CONNECTING_CACHED (ADR-017 fast path, time-limited) -> CONNECTING (full scan by the
// driver) -> ASSOCIATED (L2 up, waiting for DHCP) -> CONNECTED (IP) -> on loss: cached retry, else BACKOFF ->
// CONNECTING. Backoff is exponential with jitter and bounded (FR-406: no busy loop).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_LINK_OFF = 0,          // radio stopped (Wi-Fi disabled or not started)
    NET_LINK_NOT_CONFIGURED,   // no credentials stored: local-only operation
    NET_LINK_CONNECTING_CACHED,// connect to the cached BSSID/channel (TECH-071), time-limited
    NET_LINK_CONNECTING,       // full connect (the driver scans all channels)
    NET_LINK_ASSOCIATED,       // associated, no IP yet
    NET_LINK_CONNECTED,        // IP address obtained
    NET_LINK_BACKOFF,          // waiting before the next attempt
    NET_LINK_COUNT,
} net_link_t;

typedef enum {
    NET_EV_START = 0,      // radio available; `has_credentials` tells whether a network is configured
    NET_EV_STOP,           // Wi-Fi switched off
    NET_EV_CREDENTIALS_SET,
    NET_EV_CREDENTIALS_CLEARED,
    NET_EV_ASSOCIATED,     // bssid + channel of the AP
    NET_EV_DISCONNECTED,   // reason = driver reason code (diagnostic only)
    NET_EV_GOT_IP,
    NET_EV_LOST_IP,
    NET_EV_TICK,           // timeouts and backoff, every ~100 ms
} net_ev_kind_t;

typedef struct {
    uint8_t bssid[6];
    uint8_t channel; // 1..14
    bool valid;
} net_cache_t;

typedef struct {
    net_ev_kind_t kind;
    uint64_t now_us;
    bool has_credentials; // START
    net_cache_t cache;    // START: the record loaded from config_store (valid=false if none / bad CRC)
    uint8_t bssid[6];     // ASSOCIATED
    uint8_t channel;      // ASSOCIATED
    uint16_t reason;      // DISCONNECTED
} net_event_t;

typedef enum {
    NET_ACT_NONE = 0,
    NET_ACT_CONNECT_CACHED, // connect with bssid + channel set (no full scan)
    NET_ACT_CONNECT_FULL,   // connect without a BSSID: the driver scans
    NET_ACT_DISCONNECT,     // abort the current attempt / link
    NET_ACT_CACHE_STORE,    // persist bssid + channel (debounced by config_store)
    NET_ACT_CACHE_INVALIDATE,
    NET_ACT_RADIO_STOP,
} net_act_kind_t;

#define NET_MAX_ACTIONS 4

typedef struct {
    uint8_t n;
    struct {
        net_act_kind_t kind;
        net_cache_t cache; // CONNECT_CACHED, CACHE_STORE
    } a[NET_MAX_ACTIONS];
} net_actions_t;

typedef struct {
    uint32_t cached_timeout_us;  // ADR-017: <= 3 s, then the full connect
    uint32_t connect_timeout_us; // a full attempt that neither associates nor fails
    uint32_t dhcp_timeout_us;    // associated without an IP
    uint32_t backoff_min_us;
    uint32_t backoff_max_us;
} net_fsm_cfg_t;

#define NET_FSM_CFG_DEFAULT                                                                                    \
    {                                                                                                          \
        .cached_timeout_us = 3000000u, .connect_timeout_us = 15000000u, .dhcp_timeout_us = 10000000u,          \
        .backoff_min_us = 1000000u, .backoff_max_us = 60000000u,                                               \
    }

typedef struct {
    uint32_t attempts_cached;
    uint32_t attempts_full;
    uint32_t cached_fallbacks; // cached attempt failed or timed out -> full connect
    uint32_t connects;         // reached CONNECTED
    uint32_t disconnects;
    uint32_t timeouts;
    uint32_t ignored;          // events that do not apply to the current state
    uint32_t echoes;           // driver echoes of our own DISCONNECT, not counted as failures
} net_fsm_stats_t;

/** The read-only status (ADR-015). No SSID, no password, no IP address: the detail dialog reads those elsewhere. */
typedef struct {
    net_link_t link;
    bool ip_present;          // true only in NET_LINK_CONNECTED (FR-403: still not "CLIENT connected")
    bool configured;
    uint8_t channel;          // of the current/last association, 0 = none
    uint16_t last_reason;     // last driver disconnect reason, 0 = none
    uint32_t consecutive_failures;
    uint64_t backoff_until_us;
    net_fsm_stats_t st;
} net_status_t;

typedef uint32_t (*net_rand_fn)(void *ctx);

typedef struct {
    net_fsm_cfg_t cfg;
    net_rand_fn rnd;
    void *rnd_ctx;
    net_link_t link;
    bool configured;
    net_cache_t cache;      // the record currently believed valid
    net_cache_t pending;    // association not yet confirmed by an IP
    uint64_t deadline_us;   // timeout of the current state, 0 = none
    uint32_t failures;      // consecutive failed attempts (drives the backoff)
    uint16_t last_reason;
    uint8_t channel;
    bool echo_pending;      // our own DISCONNECT before a new attempt: its driver echo is not a failure
    net_fsm_stats_t st;
} net_fsm_t;

/** `rnd` supplies jitter (esp_random in the firmware, a seeded PRNG in tests); NULL = no jitter. */
void net_fsm_init(net_fsm_t *f, const net_fsm_cfg_t *cfg, net_rand_fn rnd, void *rnd_ctx);

/** Apply one event; `out` receives the actions to perform in order (the glue never blocks on them). */
void net_fsm_handle(net_fsm_t *f, const net_event_t *ev, net_actions_t *out);

void net_fsm_status(const net_fsm_t *f, net_status_t *out);

const char *net_link_name(net_link_t l);

/** Delay before attempt number `failures` (1 = first retry): min * 2^(failures-1), capped, plus up to 25 % jitter. */
uint32_t net_fsm_backoff_us(const net_fsm_t *f, uint32_t failures);

#ifdef __cplusplus
}
#endif
