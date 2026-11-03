# `net_status`

> **Status:** `IMPLEMENTED_SW` + on-board check (TASK-080, 2026-11-02; ADR-015 ACCEPTED).

**Role:** owner of the Wi-Fi STA link and its read-only status (ADR-015). HOST USB state stays in `usb_hid_device` →
`hid_dispatch`; every CLIENT state above `NETWORK_DOWN` belongs to `secure_remote` (TASK-100).

| | |
|---|---|
| Tasks | TASK-080 |
| Specification | 01 §6, 02 §8; ADR-015, ADR-017 (TECH-070/071) |
| Tests | `test/net_status/` (host); AT-CONN-03 (HIL); AT-EXT-01/02 (HIL with an access point) |

## Structure

| File | Role |
|---|---|
| `include/net_fsm.h`, `src/net_fsm.c` | Pure C link state machine: `OFF`, `NOT_CONFIGURED`, `CONNECTING_CACHED` (ADR-017 fast path, ≤ 3 s), `CONNECTING`, `ASSOCIATED`, `CONNECTED`, `BACKOFF` (exponential, capped at 60 s, +≤ 25 % jitter). A dropped link retries the cached AP at once; a failed cached attempt invalidates the cache and falls back to a full connect; the driver echo of our own abort is not counted as a failure |
| `include/net_status.h`, `src/net_wifi.c` | ESP-IDF glue: one task (priority 3, core 0) owns the FSM, esp_wifi / IP events arrive through a queue, the radio runs only while the link, a scan or the stress load needs it |

## Rules

- No secret leaves the esp_wifi NVS storage: the status has no SSID, no IP address and no password; the ADR-017 cache
  record is BSSID + channel only. Run-time connect settings are applied with `WIFI_STORAGE_RAM`, so an attempt never
  writes flash; only saving or forgetting a network does.
- The console offers `wifi`, `wifi scan` (channel, RSSI, security; no SSIDs) and `wifi forget`. `wifi join` exists only
  with the bench option `CONFIG_KVM_NET_CONSOLE_JOIN` (default off) and is sent only by `test/hil/wifi_provision.py`.
- Wi-Fi and LWIP buffers are allocated in PSRAM (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`): in internal RAM the driver
  took ~56 KiB next to LVGL, TinyUSB and the RGB bounce buffers.
- `WIFI_PS_NONE` while the STA runs (TECH-070), pending the AT-EXT-01 measurement.

## Integration (`main/`)

`app_main.c` starts it last (local HID, touch and UI never wait for it); `ui_glue.c` publishes `wifi_connected =
ip_present` and `CLIENT = DISABLED` (FR-403); `kvm_diag.c` borrows the radio for `stress` instead of initialising
esp_wifi itself. `app_config.c` passes the ADR-017 cache hooks, backed by the `config_store` record (TASK-090).

Governing documents: `AGENTS.md`.
