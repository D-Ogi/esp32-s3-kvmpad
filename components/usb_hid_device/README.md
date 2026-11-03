# `usb_hid_device`

> **Status:** `IMPLEMENTED_SW` — descriptors, state machine and callbacks (TASK-050, USB part). Enumeration and the
> LED output report verified on Windows 11 with board unit #1 (2026-11-02); Linux and UEFI `NOT_RUN_HW`.
> Since M3 every report comes from the HID scheduler (`hid_core`, run by `hid_dispatch` in the TinyUSB task through the
> transport hooks); this component no longer sends the neutral start itself when a scheduler is installed.

**Role:** USB layer: boot keyboard + boot mouse descriptors, TinyUSB callbacks, LED output, USB states (enum/suspend/ready).

| | |
|---|---|
| Tasks | TASK-050 (USB/HIL part) |
| Specification | 02 §7.1; 03 §5.1–5.3; ADR-005 |
| Tests | host `test/host_unit/test_usb_hid_reports.c`; HIL `test/hil/usb_enum_windows.ps1 -CapsLed` (AT-HID-01); Linux/UEFI enumeration `NOT_RUN_HW` |

| File | Content |
|---|---|
| `include/usb_hid_reports.h`, `src/usb_hid_reports.c` | pure report builders (boot keyboard 8 B, mouse 3 B boot / 5 B report with AC Pan, consumer) |
| `include/usb_hid_device.h`, `src/usb_hid_device.c` | descriptors (VID `0x303A` / prototype PID `0x400C`, TECH-037), TinyUSB callbacks, mount epoch, transport hooks (`usb_hid_device_set_transport`, `_request_pump` via `usbd_defer_func`, `_send` refused outside the TinyUSB task — TECH-034); fallback neutral reports only without a scheduler |

**Rules (MUST):**

- `esp_tinyusb` 3.0.0 is a breaking release — do not copy 2.x examples; do not add a separate `tinyusb`.
- The only component that includes `tusb.h` / `tinyusb.h`.
- Without a fresh event the device does not generate movement (TECH-064).

**Extensions (`docs/EXTENSIONS.md`):**

- ADR-018: a 3rd HID Consumer Control interface (TECH-072); `AC Pan` in the mouse report protocol, boot format unchanged (TECH-073).
- **ESP32-S3 endpoint budget: 4 usable IN** (5 with EP0, TinyUSB `dwc2_esp32.h`). 3 × HID = 3 IN. USB CDC on the OTG port does not fit — debug through the USB-UART port.

Governing documents: `AGENTS.md`.
