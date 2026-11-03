// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// net_status (TASK-080, ADR-015): owner of the Wi-Fi STA link and its read-only status. One task (below the HID,
// touch and LVGL priorities) runs the link state machine (net_fsm.h); esp_wifi events reach it through a queue, so
// no caller ever blocks on the network (FR-406). The radio stays off while no network is configured: local HID,
// touch and UI never depend on Wi-Fi (TASK-080 DoD, AT-CONN-03).
//
// Secrets: the SSID and password live only in the esp_wifi NVS storage (TECH-056). Runtime connect settings
// (cached BSSID/channel) are applied in RAM only, so a connection attempt never writes flash. Nothing here logs a
// password, and the status carries neither the SSID nor the IP address.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "net_fsm.h"

#ifdef __cplusplus
extern "C" {
#endif

/** ADR-017 cache persistence, provided by the integrator (config_store, TASK-090). NULL members = RAM only. */
typedef struct {
    bool (*load)(net_cache_t *out); // false = no valid record (missing, bad CRC or schema)
    void (*store)(const net_cache_t *c);
    void (*invalidate)(void);
} net_cache_hooks_t;

typedef struct {
    net_cache_hooks_t cache;
    bool power_save_off; // TECH-070: WIFI_PS_NONE while the STA runs (AT-EXT-01: p50 3 ms vs 26..94 ms with modem sleep)
} net_status_cfg_t;

typedef struct {
    net_status_t link;  // the state machine view
    int8_t rssi;        // of the current AP, 0 = not associated
    bool radio_on;
    bool started;
    uint32_t events;    // esp_wifi / IP events received
    uint32_t events_dropped; // the queue was full (the FSM timeouts recover)
    uint32_t scans;
    // ADR-017 / AT-EXT-02: the last time the link came back, from the start of the cycle (a bench reconnect
    // request, or the first connect attempt) to the IP, and whether the attempt that won was the cached one.
    uint32_t last_connect_ms; // 0 = no cycle completed yet
    bool last_connect_cached;
    bool power_save_off; // TECH-070 mode in force
} net_status_info_t;

typedef struct {
    uint8_t channel;
    int8_t rssi;
    uint8_t authmode;   // wifi_auth_mode_t
    bool hidden;        // empty SSID
    char ssid[33];      // for the Settings list only; never logged by this component
} net_scan_ap_t;

/** Initialise esp_wifi and start the link task. Idempotent. Returns quickly; the link comes up in the background. */
esp_err_t net_status_start(const net_status_cfg_t *cfg);
bool net_status_running(void);
void net_status_get(net_status_info_t *out);

/**
 * Blocking active scan (up to ~3 s) for Settings or diagnostics; never call it from the HID, touch or LVGL task.
 * Starts the radio for the scan when it is off and stops it again afterwards if no network is configured.
 * `aps` may be NULL to only count. ESP_ERR_INVALID_STATE while a connection attempt owns the radio.
 */
esp_err_t net_status_scan(net_scan_ap_t *aps, uint16_t max, uint16_t *found);

/** Keep the radio on (diagnostic stress load); it is stopped again by net_status_release_radio(). */
esp_err_t net_status_ensure_radio(void);
void net_status_release_radio(void);

/**
 * Provisioning from the local Settings screen (TASK-070C): store the credentials in the esp_wifi NVS storage and
 * reconnect. The strings are not kept or logged here. `pass` may be "" for an open network.
 */
esp_err_t net_status_set_credentials(const char *ssid, const char *pass);

/**
 * Bench (AT-EXT-01): switch the STA power save between WIFI_PS_NONE (`off` = true, the TECH-070 default) and
 * WIFI_PS_MIN_MODEM. Applied at once when the radio is on and kept for later radio starts until a reboot.
 */
esp_err_t net_status_set_power_save_off(bool off);

/**
 * Bench (AT-EXT-02): drop the association and let the state machine reconnect, timing the cycle. `full` = forget
 * the in-RAM BSSID/channel record first, so the reconnect takes the full-scan path (the stored record is kept).
 */
esp_err_t net_status_bench_reconnect(bool full);

/** Forget the stored network (factory reset, Settings): radio off, cache invalidated. */
esp_err_t net_status_forget(void);

#ifdef __cplusplus
}
#endif
