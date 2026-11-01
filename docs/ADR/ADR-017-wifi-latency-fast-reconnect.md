# ADR-017 — Wi-Fi: modem sleep disabled and fast reconnect
Status: ACCEPTED, PENDING-HW (measurement)
Date: 2026-11-01
Approver: project owner (2026-11-01)

## Context

- Prior art `docs/prior-art/esp32-kvm-ip.md`: according to the author, `esp_wifi_set_ps(WIFI_PS_NONE)` removes about 200 ms of latency
  of the first packet after a pause; fast reconnect through a BSSID + channel cache in NVS.
- The board is powered from USB HOST — saving radio power is not a goal.
- Remote input has NFR budgets (spec 01 §NFR, 04 §9); TASK-080 covers Wi-Fi STA/provisioning.

## Decision

- **TECH-070 SHOULD:** when Wi-Fi STA is enabled, `WIFI_PS_NONE`. The value is adopted only after the AT-EXT-01 measurement;
  if the measurement shows no gain or shows an RGB/USB regression, we return to the default IDF mode (an update of this ADR).
- **TECH-071 SHOULD:** after a successful connection, store the AP BSSID and channel in `config_store`. At startup, try a connection
  from the cache with a time limit (≤ 3 s, to be calibrated), then a full scan. A failed attempt invalidates the cache.
  The cache does not contain the password (the password only in NVS covered by production encryption, TECH-056).
- **Rejected from the original:** a static IP cache (conflicts with DHCP), the Wi-Fi password in Kconfig/`sdkconfig`.

## Consequences

- Higher radio current draw (acceptable with USB power) — check the temperature and that there is no impact on RGB scanout.
- Wi-Fi still must not block local HID (02 §4, FR-212).

## Tests

- AT-EXT-01 (HIL): latency of the first packet after 10 s / 60 s of silence, `WIFI_PS_MIN_MODEM` vs `WIFI_PS_NONE`, p50/p95.
- AT-EXT-02 (HIL): reconnect from the cache vs a full scan; the AP changes channel/BSSID → fallback to the scan, cache invalidated.
- AT-EXT-03 (unit): the cache record contains no secrets; an incorrect CRC/schema → ignored.

## Migration

None. Depends on ADR-015 (the Wi-Fi owner component for TASK-080).
