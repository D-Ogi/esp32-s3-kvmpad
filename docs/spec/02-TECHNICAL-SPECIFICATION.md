# 02 — Firmware and hardware technical specification

**Project:** KVM Essential · **version:** 1.1.0 · **goal:** a single-hardware, reproducible production-oriented implementation · **hardware:** ESP32-S3-Touch-LCD-7.

## 0. Overriding principle

"Bleeding-edge" does **not** mean picking arbitrary nightly versions. It means a current, supported stack, zero hardware guesswork, a minimal number of layers, separation of real-time HID from the screen and Wi-Fi, explicit security guarantees, metrics and automated tests. For M0 use stable versions; the exact snapshot is accepted only after compilation + HIL.

## 1. Hardware — the **ESP32-S3-Touch-LCD-7** model only

| Element | Confirmed parameter / note |
|---|---|
| SoC | ESP32-S3 (2 × LX7, up to 240 MHz), USB OTG **Full-Speed** (12 Mbit/s), Wi-Fi 2.4 GHz, BLE 5 LE |
| RAM | 512 KiB SRAM in the SoC + 8 MiB PSRAM on the module typical for this board |
| Flash | the Waveshare documentation is inconsistent: the resources section points to the N16R8 / 16 MiB variant, the general table is sometimes 8 MiB; **read it physically and do not hardcode 16 MiB** |
| LCD | 7", IPS, **800 × 480**, RGB parallel (16-bit), target format RGB565 |
| Touch | Goodix **GT911** I²C, up to **5 simultaneous points**, IRQ GPIO4 |
| I/O expander | **CH422G**, I²C on the same bus as GT911 |
| USB | native OTG, pins GPIO19/20; shared with CAN (requires EXIO5=USB) |
| UART debug | a separate Type-C through USB-to-UART; distinguish it from the native USB port (HID) |
| Power supply | 5 V Type-C; an alternative 1S Li-ion pack according to the manufacturer's instructions; no joining of VBUS without checking the schematic |

### 1.1 Pinmap — the board support package (BSP) contract

| Function | GPIO/EXIO | Rule |
|---|---|---|
| Touch SDA | GPIO8 | shared I²C bus |
| Touch SCL | GPIO9 | shared I²C bus |
| Touch IRQ | GPIO4 | interrupt input, the ISR only signals readiness |
| Touch reset | CH422G EXIO1 | access only through the board HAL |
| LCD DISP/backlight-enable | CH422G EXIO2 | do not assume PWM on EXIO2; check the driver/circuit first |
| LCD reset | CH422G EXIO3 | drive it in the correct startup order |
| USB mux/CAN mux | CH422G EXIO5 | **LOW = USB**; CAN disabled in v1 |
| USB D− | GPIO19 | available to the native USB device, do not change the pinmux at runtime |
| USB D+ | GPIO20 | as above |
| RGB PCLK | GPIO7 | timing from the Waveshare demo |
| RGB VSYNC / HSYNC / DE | GPIO3 / GPIO46 / GPIO5 | do not treat as free GPIO |
| LCD RGB Blue B3/B4/B5/B6/B7 | GPIO14 / GPIO38 / GPIO18 / GPIO17 / GPIO10 | mapping per the Waveshare table |
| LCD RGB Green G2/G3/G4/G5/G6/G7 | GPIO39 / GPIO0 / GPIO45 / GPIO48 / GPIO47 / GPIO21 | mapping per the Waveshare table |
| LCD RGB Red R3/R4/R5/R6/R7 | GPIO1 / GPIO2 / GPIO42 / GPIO41 / GPIO40 | mapping per the Waveshare table |
| SD_MOSI/SCK/MISO | GPIO11/12/13 | the card is optional, not required in v1 |
| SD_CS | CH422G EXIO4 | optional |
| RS485 | GPIO15/16 | do not use in v1 |

**TECH-001 MUST:** from the channel mapping above, the M1 implementer must determine and document the **exact order of `data_gpio_nums[]`** (from LSB to MSB according to RGB565), compare it with the Waveshare example and verify it physically. Document the verification with a red/green/blue test, a color swatch and an alignment pattern test. GPIO0/45/46 are tied to bootstrapping functions; on this board they have assigned roles and the electrical setup / states at boot must not be changed arbitrarily.

### 1.2 Mandatory hardware trials (M0)

1. Record the PCB revision, the markings and the actual flash/PSRAM layout, among others `esptool flash_id`, `idf.py size-components`, the PSRAM log.
2. Verify both physical USB-C connectors (which one connects to the HOST as HID, which one to UART-debug), the presence of VBUS and the start on the USB HID port alone.
3. I²C scan: CH422G and GT911; GT911 may have the address `0x5D` or `0x14` depending on reset/IRQ, **do not guess**. Determine the address on the given revision. The ESP_LCD_TOUCH_GT911 example default is `0x5D`.
4. `EXIO5` in the USB state before the USB stack is initialized; after a device restart, perform a re-enumeration on the host.
5. For 5 points report the complete set of `track_id`, the coordinates and the time; do not reorder them artificially.
6. Measure RGB stability with Wi-Fi, USB HID and an NVS / OTA write at the same time.
7. Carry out the HOST off / auxiliary power test separately; do not risk backfeed. For the lack of an appropriate power configuration, prepare an explicit limitation of the status function.

## 2. Versions and technologies

**Base set for M0 integration validation:**

| Layer | Initial proposal | Note |
|---|---|---|
| SDK | **ESP-IDF v6.1 stable** | the official branch (2026-08-27), pin to a commit/tag and toolchain |
| Firmware language | C17/C11-style with ESP-IDF | minimal overhead, component split, easy testing on the host |
| GUI | **LVGL 9.6.0** + **`esp_lvgl_adapter` 0.7.4 (M0 candidate)** | v9.6 is the current stable release (2026-09-16); the adapter supports RGB / LVGL 9.6. After the M0 proof-of-compatibility with HIL, pin the working choice; `esp_lvgl_port` 2.9.0 is the controlled comparison/fallback variant (ADR-013, document 07) |
| LCD | native `esp_lcd` RGB | timing configuration according to Waveshare, not a "generic SPI LCD" |
| Touch | `esp_lcd_touch_gt911` 1.2.1 + `esp_lcd_touch` | force `ESP_LCD_TOUCH_MAX_POINTS=5` and test `track_id` |
| USB HID | **`esp_tinyusb` 3.0.0** / TinyUSB 0.21.x | 3.0 is a breaking release; refer to the migration, do not copy old 2.x examples |
| RTOS | FreeRTOS in IDF | tasks, typed events, watchdog |
| Network | ESP-IDF Wi-Fi + `esp_https_server` / WebSocket | TLS, pairing, rate-limit, ownership, an optional CLIENT session |
| Storage | NVS (with encryption in production) | configuration; no storing of key streams |
| OTA | `esp_https_ota` + signature/rollback | insensitive to power loss; production secure boot and encryption |
| Static analysis | clang-tidy/Cppcheck/clang-scan-build + warnings | CMake/host test; variants without "-Werror" if the vendor is not clean |

**TECH-002 MUST:** the `TASK-000` stage creates `idf_component.yml` and `dependencies.lock` on the basis of a **working** build, and CI checks their consistency. Do not add `tinyusb` again as a separate, conflicting dependency (in 3.0 the component contains the appropriate implementation).

**TECH-003 SHOULD:** when v6.1 does not work with the board sample, fix the BSP port in a separate task and tests; a pinned fallback to a supported 5.5.x is permissible only after a documented reproduction and an ADR. Never "jump between frameworks" without an ADR.

## 3. Modular architecture — data flow

```text
              GT911 ── I2C/IRQ ── TouchInput ──┐
                                                ├─> GestureFSM ───────┐
 LCD<─RGB/PSRAM<─ LVGL UI <─ UiModel <──────────┤                    │
                  │                             └─> LocalKeyAction ──┤
                 CH422G                                              v
              (reset/USB_SEL)                         InputArbiter (one owner)
                                                             │
 Wi-Fi <─TLS/WSS─ RemoteSession ── Auth ── ProtocolValidator─┘
                                                             │
                                Typed bounded queue / key state / epoch
                                                             │
                                               HidTranslator / HidScheduler
                                                             │
                                    TinyUSB: boot keyboard + boot mouse
                                                             │
                                                        USB-C → HOST

Telemetry (read-only): USB state, CLIENT session, free heap, crashes, latency
Settings/OTA: controlled maintenance path; no direct injection around arbiter
```

**Prohibited:** connecting `touch_isr → tud_hid_*` directly; calling LVGL in ISR/TinyUSB callbacks; holding the I²C mutex during animations; passing raw JSON from the network to HID without validation; using `malloc/free` on every touch sample; letting the UI task bypass the arbiter.

### 3.1 Repository components

```text
kvm-essential/
  AGENTS.md
  docs/                       # specifications (this package)
  reference/                  # HTML mockup, does not compile to the MCU
  main/app_main.c
  components/
    board_hal/                # I2C + CH422G, BSP, USB_SEL, LCD init, backlight
    touch_input/              # GT911 0..5 points, IRQ, track_id, raw sampling
    gesture_engine/           # pure gesture state machine, unit-testable
    ui_core/                  # router, UI projection/model, lifecycle, typed actions
    ui_theme/                 # central LVGL styles/tokens
    ui_components/            # reusable status/buttons/modals/keyboard tiles
    ui_views/                 # 800x480 Mouse/Keyboard/Shortcuts/settings
    ui_touch_bridge/          # single reader GT911 -> LVGL/gesture input
    ui_platform/              # selected ESP LVGL adapter/display integration
    hid_core/                 # key map, pressed-ledger, arbiter, release_all
    usb_hid_device/           # descriptors, TinyUSB callbacks, USB states
    remote_protocol/          # version, parser, validation, session management
    secure_remote/            # TLS, pairing, permissions, leases
    config_store/             # NVS schema, migrations
    update_manager/           # signed OTA, maintenance, rollback
    diagnostics/              # metrics, fault counters, bounded log
  test/
    host_unit/                # deterministic FSM/HID/protocol tests
    fuzz/                     # touch sequences + remote frames
    hil/                      # pyusb/hidapi/uinput/test harness
  tools/                      # flash, export stats, check IDs
  sdkconfig.defaults
  idf_component.yml           # if project-level needs, otherwise component-local
  dependencies.lock
  CMakeLists.txt
```

The API boundaries are in `03-CONTRACTS-AND-PROTOCOLS.md`; do not change public types on different branches without versioning. No global LVGL pointer available to the network.

## 4. Task, ISR and scheduling model

| Task (logical) | Way of working | Relative priority | Approximate target |
|---|---|---|---|
| `usb_device` | TinyUSB callbacks + the stack task | high, not suppressed | reacts to enum/suspend/ready report |
| `hid_dispatch` | one HID state queue; zero long blocks | high | input handling within ≤1 scheduler cycle |
| `touch_sampler` | IRQ->notification; bounded read 0..5 points; periodic sanity | high | sample trigger ≤10 ms target, depends on GT911 |
| `gesture_worker` | pure, fast FSM | medium/high | no dynamic allocations |
| `lvgl_render` | one exclusive LVGL loop | medium | target 25–30 FPS UI effect |
| `network_io` | TLS + parsing + sessions | medium/low | does not block LOCAL input |
| `storage_ota` | rare, controlled operations | low | maintenance, `RELEASE_ALL` before a write |
| `metrics` | buckets/counters | low | does not log keys |

**TECH-004 MUST:** calibration of affinity/priorities after measurement (`uxTaskGetSystemState`, runtime stats); do not assign "Wi-Fi core0/UI core1 hard-wired" as a guarantee. ESP-IDF and the drivers have their own core/priority requirements; test with Wi-Fi and RGB DMA together.

**TECH-005 MUST:** the queues have a known limit; HID down/up, mode/revoke and `RELEASE_ALL` are **non-droppable** events. `pointer_move` may be merged (summed) into at most one report per cycle; an overflow of the control queue triggers a fail-safe `RELEASE_ALL` and an error counter, not a random omission of an UP.

**TECH-006 MUST:** use monotonic time (e.g. `esp_timer_get_time`), not NTP time, for gesture thresholds or leases. Events have `origin`, `epoch`, `sequence`, `timestamp_us` and an unambiguous order.

## 5. GT911: driver and point transformation

**TECH-010 MUST:** a single `touch_input` layer owns the I²C device and the touch handle. GT911 has a maximum of 5 points, `track_id`, x/y and, in the raw code, `strength`. Before being passed to the FSM, the points are sorted stably by `track_id` or mapped to slots, not by the "first point in the buffer".

**TECH-011 MUST:** at startup, read the `product ID` and the address and verify the X/Y ranges. The 0° rotation transformation is checked with five control points (the corners + the center). The `mirror_x/y`, `swap_xy` configuration is explicit in the BSP, without "try three permutations blindly".

**TECH-012 MUST:** the `IRQ` ISR only does a notify / flag. An I²C read only from a task, with a timeout. After an I²C error: a counter and clearing of the active gesture, a recovery attempt with backoff. No leftover DOWN.

**TECH-013 MUST:** the I²C shared with CH422G has a single manager/mutex with a short hold time; the GT911 read takes place **before** expensive NVS writes. Do not reset Touch from another task during a read.

**TECH-014 MUST:** also deliver to `gesture_engine` the `touch_count=0` snapshot, a gap indicator and `track_id`; the disappearance of a finger is a critical event. Verify `CONFIG_ESP_LCD_TOUCH_MAX_POINTS=5` (or the current equivalent) at build time/test time.

**TECH-015 SHOULD:** compare the ready-made LVGL 9.4+ gesture recognition with our classifier, but **do not mix** the two as simultaneous HID sources. LVGL gets lightweight UI events; a dedicated gesture engine gives decidability and testability.

## 6. LCD RGB + PSRAM: memory and limitations

### 6.1 Framebuffer budget

```text
RGB565  = 800 × 480 × 2 = 768 000 B = 750 KiB
1 × framebuffer = ~0.733 MiB
2 × framebuffer = 1 536 000 B = 1 500 KiB (~1.465 MiB)
1 × 10-line bounce buffer = 800 × 10 × 2 = 16 000 B
2 × 10-line bounce buffer = 32 000 B internal DMA-capable memory
2 × 20-line bounce buffer = 64 000 B internal DMA-capable memory
```

**TECH-020 MUST:** in v1 prefer `fb_in_psram = true`; benchmark A: 1 frame + double bounce 10 lines; benchmark B: 1 frame + bounce 20 lines; variant C: double-buffer only if it improves the absence of tearing/response without starving Wi-Fi/USB. The choice is made on the basis of p95 latency and RGB stability, not FPS alone.

**TECH-021 MUST:** for the RGB bounce mode, the Espressif documentation warns about the dependency on the cache: during a Flash write/OTA or long critical sections a momentary rendering problem may occur. In `update_manager`, switch to `MAINTENANCE`, disable active HID, suspend expensive UI updates; **do not guarantee** flawless refresh during flash.

**TECH-022 MUST:** do not use a dynamic web engine, a browser or a WebView on the ESP. LVGL draws native widgets; carry the CSS prototype over as geometry, text and UX rules, not as a runtime.

**TECH-023 MUST:** no full-screen 60 FPS redraw just for animations; minimal refresh for state changes, a lower CPU/PSRAM cost. Metrics: `fps_render`, `tear_count`, `psram_alloc`, `internal_heap_min`, `hid_latency_p95`.

**TECH-024 SHOULD:** before you enable `CONFIG_SPIRAM_SPEED_120M`, 120MHz memory and a 64B data cache, check the qualified hardware revision and `menuconfig` in the chosen IDF; the Waveshare configurations for the sample benchmarks are not automatically optimal with network+USB.

**TECH-025 MUST:** the image is fully functional with Wi-Fi disconnected. The PSRAM load from Wi-Fi must be measured. HID quality takes precedence over 30 FPS.

## 7. USB HID device — host-independent mode

### 7.1 Interface and descriptors

- **TECH-030 MUST:** the ESP32-S3 acts as a **USB device**, the HOST as the **USB host**. Immediately after CH422G `EXIO5=LOW`, start the resident USB device stack on **native USB-C**, not on USB-to-UART.
- **TECH-031 MUST:** exposure of at least two HID functions: a **boot-compatible keyboard** and a **boot-compatible relative mouse**, with **two separate HID interfaces** preferred, with the correct subclass/protocol; BIOS/UEFI compatibility must be confirmed physically. Additional report IDs and Consumer Control are optional, without breaking boot support.
- **TECH-032 MUST:** do not require a proprietary driver on the HOST for the basic mouse/keyboard. Check Windows 11, Linux and UEFI (macOS — if available).
- **TECH-033 MUST:** the USB HID keyboard contains a modifier bitmap, 6 ordinary usage slots for the boot protocol; a combined Ctrl+Alt+Delete fits within the limit. Mouse: a button bitmap and signed relative X/Y, wheel (if the boot mouse report has no wheel, define a fallback report interface and an OS test). Do not confuse the 8-bit X/Y with an absolute LCD coordinate.
- **TECH-034 MUST:** the TinyUSB task is the only caller of the `tud_hid_*` API; the `tud_mount_cb`, `tud_umount_cb`, `tud_suspend_cb`, `tud_resume_cb` callbacks (or their compatible equivalents) emit state, they do not perform UI.
- **TECH-035 MUST:** after `mount`, initialize a neutral keyboard and mouse state; after `umount`, zero the ledger and make sure that no old command from the queues is replayed.
- **TECH-036 MUST:** active mouse movement and the keyboard must not block each other on the UI timer; the scheduler dispatches reports according to the availability of endpoints, it prefers `RELEASE_ALL` and keyboard up/down over bulk movement.
- **TECH-037 MUST:** the USB VID/PID choice is correct for a prototype/test; **do not distribute the product with an accidentally borrowed VID/PID** (including the Espressif example). A production VID/PID requires an entitlement/agreement.

### 7.2 Ledger and release-all

- `pressed_keyboard[0..255]`, `modifier_mask`, `pressed_mouse_buttons`, `last_usb_epoch` are maintained only by `hid_core`.
- Every DOWN sequence has an exact UP or a fail-safe transition. If we lose USB before the UP, the nearest new mount starts in a neutral state.
- Do not send an UP without reflecting the final state bitmap (the boot keyboard has simultaneous usages); a "neutral report" after dropping all the entries is sufficient.
- There is no `Client JSON -> TinyUSB` path without the arbiter.
- On an owner/session epoch change, a gesture timeout, a USB error, OTA, a factory reset and Wi-Fi loss **if the current owner is REMOTE**: a **high-priority `RELEASE_ALL`**. A Wi-Fi outage **must not** interrupt correct local HID with `OWNER_LOCAL`. When only the backlight is being turned off, do not accidentally reset active local input; only an actual transition to sleep/maintenance requires a release.
- When the endpoint is unavailable, the reset of the **logical** state is immediate, and the neutral report comes after USB is recovered; do not claim that USB physically delivered the UP with the cable disconnected.

### 7.3 Correct chord sequence

```text
PRE: usb=USB_MOUNTED, owner authorized, no maintenance
PRESSED: modifiers=0x00, keys=[]
1. set modifiers=[LeftCtrl,LeftAlt], keys=[]; await HID IN completion/ready
2. set modifiers=[LeftCtrl,LeftAlt], keys=[Delete]; await completion/ready
3. set modifiers=[LeftCtrl,LeftAlt], keys=[]; await completion/ready
4. set modifiers=[], keys=[]; await completion/ready
5. signal delivered_to_usb_stack (not executed_by_host)
ON ANY INTERRUPT: clear ledger and enqueue neutral keyboard/mouse reports if mounted
```

Waiting for endpoint readiness **with a timeout**; in case of a timeout, without repeating the whole chord (the risk of 2× execution), a fail-safe transition.

## 8. Network, protocol and remote security

- **TECH-040 MUST:** a 2.4 GHz Wi-Fi LAN as the infrastructure (STA). First boot Remote OFF; pairing only with physical authorization and after the trusted network has been securely configured.
- **TECH-041 MUST:** the CLIENT API only over HTTPS/WSS (TLS >=1.2, prefer 1.3 when it is supported in the given configuration). The exact certificate scheme is described by ADR-002. **Do not present a self-signed certificate as a working "zero-config" browser.**
- **TECH-042 MUST:** authentication attempts limited; unique session tokens, an expiry date, read-only/operator roles, enforcement of local approval, rotation/revoke.
- **TECH-043 MUST:** `RemoteProtocolValidator` rejects an unknown `v`, type, overflow, injection, an invalid lease and an out-of-order sequence. The parser has payload and time limits.
- **TECH-044 MUST:** ownership `LOCAL > REMOTE > NONE`, with an epoch; a remote without a lease can only fetch status. One active operator session; multiple observers only later.
- **TECH-045 MUST:** remote input exclusively allowlisted HID events; no shell commands, URL-fetch, cloud scripts, flash-from-unauthenticated, arbitrary reading of keystrokes.
- **TECH-046 MUST:** privacy: do not log what the user types (nor full usage-ids + timestamps from which passwords could be reconstructed). Aggregated counters and non-secret error reasons are allowed.
- **TECH-047 SHOULD:** distribution with signed firmware, Secure Boot v2, Flash Encryption Release Mode, encrypted NVS, debug disabled in production; see the security procedure below.

### 8.1 Browser CLIENT: an important caveat

When the CLIENT is a Web application, `https://192.168.*` with an unknown, self-signed certificate often does not allow correct WSS without manually establishing trust. It is **not permitted** to work around this with `ws://` or by disabling certificate verification in production. The reference variant requires one of these methods: (A) a dedicated desktop/mobile application with cert pinning, (B) a trusted gateway/reverse proxy with a CA-trusted cert and a secure link to the DEVICE, (C) local domain/CA infrastructure. The choice and the test are in ADR-002, before the implementation of browser pairing.

## 9. Configuration persistence, OTA and recovery

- **TECH-050 MUST:** NVS `schema_version`, CRC/validation, a migrator for every schema change; settings atomic, write debounce (do not write flash on every tap), sane defaults.
- **TECH-051 MUST:** division of the Flash into at least two possible OTA slots with rollback, NVS and room for cert/key storage; the partition design depends on the actual 8/16 MiB. If 8 MiB, calculate the real bin size. **Do not flash a 16 MiB table onto an 8 MiB unit.**
- **TECH-052 MUST:** Signed OTA with verification of the TLS/chain source and of the image signature, a health-check at startup, rollback on an unresponsive USB/GUI, protect against power loss.
- **TECH-053 MUST:** before OTA `RELEASE_ALL`, disconnect the controlling client, block input and show the `Updating` state; after the restart do not restore held state.
- **TECH-054 MUST:** in production, hardware Secure Boot v2 and Flash Encryption are programmed only after recovery tests and a key backup. eFuse is practically irreversible; do not automate this stage without the approval of a physical operator.
- **TECH-055 MUST:** with production secure boot, do not rely on the ROM USB-OTG CDC/DFU for emergency flashing (the protections may disable it). Design the firmware recovery process foreseen and approved by the manufacturer.
- **TECH-056 SHOULD:** NVS and certificates generated per-device, no shared secrets in a public repository; keep the signing keys only in a secure release process.

## 10. Monitoring, diagnostics, fault handling

- **TECH-060 MUST:** the event ID and the error code without confidential data; serial UART for debug, an optional diagnostics screen.
- **TECH-061 MUST:** the metrics `touch_samples`, `touch_i2c_errors`, `gesture_cancels`, `hid_send_errors`, `hid_release_all`, `usb_reenumerations`, `client_auth_denials`, `client_revokes`, `heap_free_min`, `psram_free_min`, `lvgl_fps`, the `touch_to_hid_us` histogram.
- **TECH-062 MUST:** a watchdog for critical tasks; a permanent `I2C busy`, `USB stuck`, `TLS denial` goes into a safe state, not into continuous resets without end.
- **TECH-063 MUST:** log retention a bounded ring; do not keep raw keystrokes or the full remote payload; the diagnostics export requires permissions and masks tokens.
- **TECH-064 MUST:** on every touch failure/fail-to-read for >120 ms, end the active gesture. The USB device should not generate movement by itself without a fresh event.

## 11. Build and CI

### 11.1 Tooling host

- `idf.py --version` = the pinned version, `idf.py set-target esp32s3`, a CMake project.
- `idf.py build` for debug + release. In CI on a clean container: a build without a locally generated `sdkconfig`, only `sdkconfig.defaults` + `dependencies.lock`.
- `idf.py size` and `size-components` archived as a CI artifact. Exceeding the memory budget blocks the release.
- Host unit tests gesture/hid/protocol with sanitizers, a fuzzer for the edges; the seed and a reproducer of every failure are recorded.
- `clang-format` + static analysis; avoid pulling the whole IDF into system tests where plain C is sufficient.
- `release` requires real HIL, a signature of the artifact + the git sha version, a security report.

### 11.2 Proposed quality budgets to be adjusted after M0

| Resource | Project limit / requirement |
|---|---|
| LCD RAM | 1–2 framebuffers in PSRAM, **not** 2 × full framebuffer in internal RAM |
| Internal heap | "minimum free heap" continuously monitored, a soft floor of 64 KiB under stress; if violated, a revision of the architecture |
| Event queue | no unbounded allocations; an explicit cap of 128 critical events + coalescing mouse move |
| Remote | one active CLIENT-operator, the length of a single message ≤1 KiB (except pairing/control with an explicitly different limit) |
| TLS sockets | a limit of 2 authorized simultaneous connections in v1; not based on speculation about RAM, to be measured after HIL |
| Assets/fonts | static assets on flash; do not load fonts on the fly over the Internet |
| Hot-path input | no JSON parser, logs, malloc/free or `printf` on every sample |

These budgets are **architectural targets**, not the confirmed capacity of the final application; they require HIL telemetry.

## 12. Interoperability and host limitations

- **Windows 11:** a standard HID keyboard/mouse and Windows shortcuts. The selected keyboard layout determines the characters.
- **UEFI/BIOS:** separate boot-compatible HID interfaces are preferred for maximum support. Do not declare full UEFI compliance without physical trials.
- **Linux:** USB HID and a possibly different Alt/Meta/Fn mapping.
- **macOS:** the modifiers in HID do not map 1:1 to the `Win` labels; the target profile may change this in the UI without changing the physical usage.
- **RDP/VNC/KVM software:** some combinations are intercepted by the remoting layer — this is not a USB error if the physical HOST received them but the application did not run the intended action.

## 13. Sources of truth and critical sources

- Board + pinout: https://docs.waveshare.com/ESP32-S3-Touch-LCD-7
- Board examples: https://github.com/waveshareteam/ESP32-S3-Touch-LCD-7
- GT911: https://components.espressif.com/components/espressif/esp_lcd_touch_gt911
- GT911 `track_id` implementation: https://github.com/espressif/esp-bsp/blob/master/components/lcd_touch/esp_lcd_touch_gt911/esp_lcd_touch_gt911.c
- LVGL port + multitouch 9.4+: https://github.com/espressif/esp-bsp/blob/master/components/esp_lvgl_port/README.md
- RGB driver + PSRAM/bounce + cache restrictions: https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/peripherals/lcd/rgb_lcd.html
- TinyUSB wrapper 3.0.0: https://components.espressif.com/components/espressif/esp_tinyusb
- TinyUSB HID composite/boot notes: https://docs.tinyusb.org/en/latest/examples/device/hid_composite.html
- HTTPS Server: https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/protocols/esp_https_server.html
- Security and eFuse: https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/security/secure-boot-v2.html

> **STOP RULE:** Do not call a function by a name recalled from memory if you have not confirmed the header available in the pinned version of the dependency. In particular `esp_tinyusb` 3.x, the LVGL port, the i2c bus API and the `esp_lcd_touch_*` versions have migrations.


## 12. UI architecture update — 1.1.0 (ADR-013)

`07-KVMpad-UI-FRAMEWORK-ARCHITECTURE.md` and `AGENTS-UI.md` apply. The previous shorthand "`ui_lvgl`" has been split into `ui_platform`, `ui_core`, `ui_theme`, `ui_components`, `ui_views`, `ui_touch_bridge`. Do not add a second state-handling framework. Use the LVGL Observer for display bindings. One module is to read GT911, and the UI must not send HID directly. The adapter choice only after TASK-060A and physical validation; v1.0 is not proof of compatibility of the new stack.
