# ADR-015 — Component ownership for TASK-080 (Wi-Fi STA, network status, USB state publisher)
Status: ACCEPTED (owner, 2026-11-02)
Date: 2026-11-02
Approver: project owner (decision of 2026-11-02, accepted as proposed)

## Context

- 05 TASK-080 asks for a USB state publisher, Wi-Fi STA/provisioning, a read-only CLIENT state `DISABLED..LISTENING`, a UI
  detail dialog and a local-only fallback. The 02 §3.1 component tree names no owner; `docs/REPO-LAYOUT.md` records the gap.
- 01 §6.1/§6.2 and FR-401/403: HOST and CLIENT are independent; Wi-Fi association or an IP alone never means
  `Client connected`. FR-406: the network must not stall USB, touch or the GUI; reconnect uses jitter and backoff.
- ADR-017 (TECH-070/071): modem sleep off after measurement; BSSID + channel cache in `config_store`, no secrets in it.
- TASK-050 (M3) already publishes the HOST USB state: `usb_hid_device` → `hid_dispatch` snapshot (`kvm_host_usb_t`), and the
  UI glue copies it into the UI mailbox (TASK-060B).
- TECH-056: secrets only in NVS covered by production encryption; logs never contain secrets (TECH-046).

## Options

- **A — Wi-Fi inside `secure_remote`.** One network component, but the Wi-Fi link (needed for status, OTA, time) would live in
  the TLS/pairing code, and `secure_remote` could not be disabled without losing the network status.
- **B — Wi-Fi inside `diagnostics`.** Wrong layer: diagnostics is read-only telemetry, not a link owner.
- **C — a new component `net_status/`** that owns the Wi-Fi STA link and its status; `usb_hid_device` keeps the HOST USB state;
  `secure_remote` (TASK-100) later owns every CLIENT state above `CLIENT_NETWORK_DOWN`.

## Decision

Option **C**.

- **`components/net_status/`** owns the Wi-Fi STA link:
  - `net_fsm.{c,h}`: pure C, host-tested. Link states `OFF → NOT_CONFIGURED | CONNECTING_CACHED → SCANNING →
    CONNECTING → CONNECTED (IP) → BACKOFF → …`. Reconnect backoff is exponential with jitter, bounded (FR-406). The cached
    attempt has a time limit and a failure invalidates the cache (TECH-071).
  - `net_wifi.c`: the ESP-IDF glue. It runs in its own task below the HID, touch and LVGL priorities, takes `esp_wifi` events
    through a queue and never blocks a caller. `WIFI_PS_NONE` while STA runs (TECH-070, pending the AT-EXT-01 measurement;
    a Kconfig option restores the IDF default).
  - The status snapshot (`net_status_get()`): link state, RSSI, channel, IP present, last failure reason, counters.
    The SSID and the IP address are for the detail dialog only (07 §7: the top bar never grows with them). Never a password.
- **Credentials and provisioning.** The SSID and password live only in the `esp_wifi` NVS storage (`WIFI_STORAGE_FLASH`),
  covered by NVS encryption in production (TECH-056). They are never put in `config_store`, Kconfig, `sdkconfig`, logs or
  the console history. Provisioning in v1 is local only: Settings → Wi-Fi, a scan list plus the password typed on the
  on-screen keyboard (TASK-070C). No SoftAP or BLE provisioning in v1: no unauthenticated radio surface on first boot
  (TECH-040: first boot Remote OFF). The serial console offers `wifi`, `wifi scan` (channel, signal and security only,
  no SSIDs) and `wifi forget`. A `wifi join` exists only behind the bench option `CONFIG_KVM_NET_CONSOLE_JOIN` (default
  off, never in a release build) for HIL before the Settings screen exists; it is sent only by
  `test/hil/wifi_provision.py`, which takes the credentials from environment variables, keeps no log of the echoed
  line and prints only the result; the firmware wipes the arguments and the console history afterwards.
- **CLIENT state.** `net_status` reports only the network part. Until `secure_remote` exists (TASK-100) the CLIENT state is
  `CLIENT_DISABLED` (Remote is OFF and cannot be turned on). From TASK-100, `secure_remote` derives `NETWORK_DOWN` from
  the `net_status` link and owns `LISTENING` and every state above it (FR-403: never `Connected` from Wi-Fi alone).
- **USB state publisher.** Stays in `usb_hid_device` → `hid_dispatch` (M3). No second publisher.
- **Status aggregation for the UI.** `main/ui_glue.c` (TASK-060B) publishes the HID, touch, gesture and network slices into the
  UI mailbox. The detail dialog is a `ui_views` overlay (TASK-070C) fed by the same snapshots.
- **ADR-017 cache.** `net_status` asks `config_store` (TASK-090) for the BSSID/channel record and writes it after a successful
  connection; the record has no secret and a bad CRC or schema makes it ignored (AT-EXT-03).

## Consequences

- One more component, with a pure core that is tested on the host like `gesture_engine`.
- A network failure is contained in `net_status`: local HID, touch and UI keep working (DoD of TASK-080, AT-CONN-03).
- `secure_remote` depends on `net_status`, never the other way round.

## Tests

- Host unit (`test/net_status/`): every FSM transition; backoff bounds and jitter; cache attempt timeout and invalidation;
  disconnect storms; the snapshot never claims a link without an IP; no secret in the snapshot or the cache record.
- AT-CONN-03 (HIL): USB mounted, no Wi-Fi configured → HOST `Active`, CLIENT `Off`, local HID works.
- AT-CONN-04/05: with TASK-100. AT-EXT-01/02: measured on board #1 on 2026-11-02 with the console benches `wifi ps none|min` and `wifi reconnect [full]` (`test/reports/2026-11-02_M6_AT-EXT-01-02_d36ab0a.yaml`); an AP channel/BSSID change remains `NOT_RUN_HW`.

## Migration

None. `docs/REPO-LAYOUT.md` gap 1 is closed by this ADR once accepted.
