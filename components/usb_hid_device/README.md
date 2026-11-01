# `usb_hid_device`

> **Status:** `DESIGNED` — directory created in the repository skeleton; no implementation yet.
> Build files (`CMakeLists.txt`, `idf_component.yml`) are created in TASK-000 from a working
> build — do not write them "from memory".

**Role:** USB layer: boot keyboard + boot mouse descriptors, TinyUSB callbacks, LED output, USB states (enum/suspend/ready).

| | |
|---|---|
| Tasks | TASK-050 (USB/HIL part) |
| Specification | 02 §7.1; 03 §5.1–5.3; ADR-005 |
| Tests | AT-HID-*, Windows/Linux/UEFI enumeration (L4 HIL, otherwise `NOT_RUN_HW`) |

**Rules (MUST):**

- `esp_tinyusb` 3.0.0 is a breaking release — do not copy 2.x examples; do not add a separate `tinyusb`.
- The only component that includes `tusb.h` / `tinyusb.h`.
- Without a fresh event the device does not generate movement (TECH-064).

**Extensions (`docs/EXTENSIONS.md`):**

- ADR-018: a 3rd HID Consumer Control interface (TECH-072); `AC Pan` in the mouse report protocol, boot format unchanged (TECH-073).
- **ESP32-S3 endpoint budget: 4 usable IN** (5 with EP0, TinyUSB `dwc2_esp32.h`). 3 × HID = 3 IN. USB CDC on the OTG port does not fit — debug through the USB-UART port.

Governing documents: `AGENTS.md`.
