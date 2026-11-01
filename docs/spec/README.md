# KVMpad (formerly KVM Essential) — complete implementation specification

**Documentation version:** 1.1.0 — 2026-11-01  
**Device:** Waveshare **ESP32-S3-Touch-LCD-7** (not 7B/7C, not the version without Touch)  
**Panel:** 7 inches, 800 × 480, landscape, RGB565, GT911 (5 points)  
**Status:** specification and reference design; it does **not** declare implemented firmware or approved HIL test results.  
**Language:** English edition of the v1.1.0 package (translated 2026-11-01 from the Polish original, whose archive has
SHA256 `dc99ba86e3e78f509522e48c60cf7dc806fe6820f720259abee89e03334db343`); requirement IDs, section numbers, interface, event and code symbol names are unchanged.  
**Integrity:** the package is frozen — `MANIFEST-SHA256.md`, checked with `python -I tools/verify_spec_manifest.py`; changes only through ADRs in `docs/ADR/`.

## What we are building

A touch-based, local KVM controller: the device emulates **a USB HID keyboard and mouse toward the HOST computer**, shows an independent status of the HOST connection and of the remote **CLIENT** session, and after authorization it can accept control events from a remote client over the network. **It does not capture HDMI or video**: this board alone is not a complete KVM-over-IP with an image. The video path is a separate subsystem, outside the scope of v1.

The choice of the reference architecture (ADR-001): **ESP32-S3 ⇄ USB HID ⇄ HOST**; **CLIENT ⇄ authenticated, encrypted transport ⇄ ESP32-S3**. The mouse and keyboard on the LCD work locally without Wi-Fi. Details and open decisions: `06-DECISIONS-AND-RISKS.md`.

## Documents: recommended reading order

| File | Role / what it contains |
|---|---|
| `01-FUNCTIONAL-AND-PROCESS-SPECIFICATION.md` | User behavior, the 800×480 screen, gestures, keys, statuses, scenarios, edge cases, FR/NFR requirements |
| `02-TECHNICAL-SPECIFICATION.md` | Board, GPIO, GT911, CH422G, USB HID, RGB/LVGL, FreeRTOS, PSRAM, security, OTA, repo, build |
| `03-CONTRACTS-AND-PROTOCOLS.md` | C contracts, event model, machine states, CLIENT communication, JSON, validation, HID queues and sequences |
| `04-TEST-PLAN-AND-ACCEPTANCE-CRITERIA.md` | Unit, integration, hardware/HIL, performance, security, traceability, final DoD |
| `05-IMPLEMENTATION-PLAN.md` | Atomic tasks, order, dependencies, task briefs, evidence and quality gates |
| `06-DECISIONS-AND-RISKS.md` | ADRs, architectural assumptions, risks, compatibility, uncertainties that require tests |
| `AGENTS.md` | Mandatory working contract for everyone implementing in the repository |
| `07-KVMpad-UI-FRAMEWORK-ARCHITECTURE.md` | **ADR-013, the LVGL 9.6 architecture, Observer, UI Core, touch router, EEZ Studio and UI tests** |
| `AGENTS-UI.md` | Rules and small tasks for contributors working on the UI |
| `reference/kvm-essential-800x480.html` | **Interactive mockup** — a visual reference, not firmware code |
| `reference/kvm-essential-ux-overview.png` | A visual overview of the two modes |

**Precedence rule:** security and hardware compatibility → `02`/`03` → the behavior described in `01` → the UI architecture `07` (for UI) → the tests from `04` → the HTML mockup. In case of a contradiction **do not guess**: describe it in an ADR and block the given fragment until it is clarified. The HTML may use simplified demonstration logic, which is NOT a normative HID driver.

## Minimum completion criteria for v1

- [ ] USB: the HOST enumerates the ESP32 as a boot keyboard + boot mouse (or a documented, tested equivalent), works in BIOS/UEFI on the supported test platforms.
- [ ] GT911: correctly recognizes 1/2/5 points and `track_id`; no random clicks after the number of fingers changes.
- [ ] UI: 800×480 without page scrolling; Mouse, Keyboard, Shortcuts; two independent, real statuses.
- [ ] Critical: **no held keys/buttons remain in the DOWN state** after a loss of control, a mode change, a session restart, suspend or an error.
- [ ] UI/HID work without the Internet; the remote CLIENT is not needed for local operation.
- [ ] The remote path — only after encryption, authentication and the assignment of control; the local user has priority.
- [ ] Release: dependency control, tests and OTA signing; production eFuse security only after a verified recovery procedure.
- [ ] Evidence: enumeration logs, test report, benchmarks from hardware, the checklists from `04`.

## Identifiers and scope

- **MUST** — required for v1; **SHOULD** — recommended, may land after v1 with a justification; **MAY** — optional.
- `FR-xxx` — functional, `PROC-xxx` — flows, `NFR-xxx` — non-functional, `TECH-xxx` — implementation, `SEC-xxx` — security, `AT-xxx` — acceptance, `TASK-xxx` — work.
- Every implementation PR / commit has to indicate the requirement and test IDs.

## Hardware facts and sources (verification status: 2026-11-01)

1. Waveshare — device description, LCD, pins, CH422G, USB/CAN: https://docs.waveshare.com/ESP32-S3-Touch-LCD-7
2. Waveshare — instructions, UART and USB port, memory, example performance: https://docs.waveshare.com/ESP32-S3-Touch-LCD-7/Instructions-For-Use
3. Waveshare — example code: https://github.com/waveshareteam/ESP32-S3-Touch-LCD-7
4. Espressif — ESP-IDF v6.1, documentation: https://docs.espressif.com/projects/esp-idf/en/v6.1/
5. Espressif — RGB LCD, PSRAM and bounce buffers: https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/peripherals/lcd/rgb_lcd.html
6. Espressif — `esp_lvgl_port` (verified version 2.9.0): https://components.espressif.com/components/espressif/esp_lvgl_port
7. Espressif — `esp_lcd_touch_gt911` (verified version 1.2.1): https://components.espressif.com/components/espressif/esp_lcd_touch_gt911
8. Espressif — `esp_tinyusb` (verified version 3.0.0): https://components.espressif.com/components/espressif/esp_tinyusb
9. Espressif — HTTPS Server: https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/protocols/esp_https_server.html
10. Espressif — Secure Boot v2: https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/security/secure-boot-v2.html
11. Espressif — the GT911 driver with `track_id`: https://github.com/espressif/esp-bsp/blob/master/components/lcd_touch/esp_lcd_touch_gt911/esp_lcd_touch_gt911.c
12. TinyUSB — reference HID composite: https://docs.tinyusb.org/en/latest/examples/device/hid_composite.html

> **Note on versions:** the documentation lists the versions available on 2026-11-01; it does not claim that their combination has already been compiled. M0 requires building a working proof-of-compatibility and freezing the versions that actually work in `dependencies.lock`.


## What changed in v1.1.0?

The additional documents `07` and `AGENTS-UI` establish *KVMpad UI Core* instead of the monolithic `ui_lvgl`. Candidate update: LVGL 9.6.0 + `esp_lvgl_adapter` 0.7.4 to be **tested**, and not as a completed integration. The optional EEZ Studio generator (LVGL/no-Flow) is a visual tool, not a runtime framework. Documents 01/03/04 retain the normative HID, FSM and security contracts. The old v1.0 artifacts remain unmodified.
