# Prior art: KMChris/esp32-kvm-ip

| | |
|---|---|
| Repo | https://github.com/KMChris/esp32-kvm-ip |
| Reviewed commit | `e7f8a95cec8ca20b147427ae940f84eb3f77aec0` (2026-03-27) |
| License | **MIT** (Copyright (c) 2026 Krzysztof Mizgała) |
| Scope | ESP32-S3 DevKit without a screen; Windows host → Wi-Fi/UDP → ESP32 → USB HID → target PC |
| Size | approx. 900 lines of C (firmware) + 1100 lines of Python (Windows server) |
| Assessment | 2026-11-01, while setting up the KVMpad repository |

## Verdict

**We are not porting the code. We are taking a few ideas.** The firmware breaks several MUST rules of our specification.
After adaptation, little more than the standard TinyUSB macros would be left of it.

## Incompatibilities with our specification (what NOT to copy)

| Their solution | Our rule | Consequence |
|---|---|---|
| One composite HID interface with report IDs, `HID_ITF_PROTOCOL_NONE`, 16-bit mouse | TECH-031, ADR-005: separate boot keyboard and boot mouse interfaces | no boot protocol, i.e. a risk in BIOS/UEFI |
| `hid_task` calls `tud_hid_report()` directly from the queue; no ledger and no `RELEASE_ALL` | AGENTS §2.2–2.3, 03 §4 | losing Wi-Fi or the server while a key is pressed leaves it in the DOWN state |
| `xQueueSend(..., 10 ms)` → "dropping event" when the queue is full | TECH-005: DOWN/UP must not be lost | a lost UP = stuck key |
| Plaintext UDP without authentication; a `seq` filter as the only protection | SEC, ADR-002: TLS + auth + lease + physical approval | any host on the LAN can inject keystrokes |
| The Wi-Fi password in Kconfig, i.e. baked into `sdkconfig` and the firmware image | TECH-056, TASK-080 provisioning | a secret in the build artifact |
| VID/PID `0x303A:0x4001` (Espressif's VID) | TECH-037, R-09 | must not be distributed with a borrowed VID/PID |
| `esp_tinyusb` 2.1.1 + a separate `tinyusb`, `'*'` dependencies, IDF 5.5.2 | TECH-002: `esp_tinyusb` 3.0.0, without a separate `tinyusb`, without `*` | the 2.x API does not match 3.0 (breaking) |

## What is worth taking (as ideas; we write the code ourselves)

**Accepted 2026-11-01:** items 1–2 → ADR-017, items 3–4 → ADR-018, item 5 → ADR-019 (on the CLIENT side),
item 6 → ADR-020, item 8 → ADR-021 (PROPOSED). Item 7 (USB Kconfig) → a pattern for TASK-050/130. Index: `docs/EXTENSIONS.md`.

1. **`esp_wifi_set_ps(WIFI_PS_NONE)`.** The author states that this removes approx. 200 ms of first-packet delay after an idle period (`main/main.c:43`).
   It concerns the remote path (TASK-100) and the NFR measurements (TASK-120). Current draw is not a problem, because the board is powered from USB.
   → a candidate for `sdkconfig.defaults`/Wi-Fi init; **measure** it in HIL, do not take it on trust.
2. **Fast Wi-Fi reconnect:** a cache of BSSID + channel (+ optionally a static IP) in NVS (`wifi_manager.c`).
   It fits TASK-080 (`config_store` + Wi-Fi). Note: the cache must not store secrets in plain text outside encrypted NVS.
3. **Consumer Control (media keys).** The spec allows this optionally (TECH-031, 03 §5.1, "Optional HID 2").
   In our case exclusively as a **third, separate interface** or an additional report that does not break the boot keyboard/mouse.
4. **The horizontal scroll field `AC Pan` (Consumer page 0x0238) in the mouse report.** For 03 §5.3, check whether it can be added
   without losing boot mouse compatibility (e.g. in a second, non-boot interface).
5. **Typing text as a sequence of keystrokes** (`server/clipboard_typer.py`, including PL Programmer via AltGr).
   For us this would be a new feature (e.g. "Type text" in Shortcuts or a CLIENT command). It requires a new FR and an ADR:
   the character → usage map depends on the HOST layout, and 07 §7 forbids an "ASCII-as-HID" keymap. Explicit per-layout tables are therefore needed,
   along with interruption (Esc) and `RELEASE_ALL` on completion. **Product question.**
6. **Mouse jiggler** (0 px movement, so that the HOST does not fall asleep). A small, optional idea for Shortcuts/Settings. **Product question.**
7. **Kconfig for USB strings and VID/PID.** A good pattern for TASK-050/130, but the default values must be explicitly test values.
8. **Native Windows client** (WinAPI `WH_KEYBOARD_LL`/`WH_MOUSE_LL` + Raw Input, Scroll Lock toggling).
   This is a realistic "CLIENT" variant to be decided in ADR-002 (native vs browser), but the protocol must be replaced with our v1 (WSS + auth).

## License in the event of borrowing

MIT allows a fragment to be taken into a PolyForm Noncommercial project **provided that** the MIT notice
(copyright + permission notice) is kept next to the copied fragment and in `THIRD_PARTY_NOTICES`. The ideas from the list above on their own
do not require attribution. A mention in `docs/prior-art/` is enough as an honest acknowledgement of the source of inspiration.
