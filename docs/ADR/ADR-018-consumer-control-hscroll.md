# ADR-018 — Multimedia keys (Consumer Control) and horizontal scrolling
Status: ACCEPTED, PENDING-HW (enumeration/UEFI)
Date: 2026-11-01
Approver: project owner (2026-11-01)

## Context

- TECH-031 and 03 §5.1 allow an optional Consumer Control and a horizontal wheel, **provided that** the boot keyboard
  and the boot mouse remain functional. ADR-005 prefers separate HID interfaces.
- FR-114 and 03 §3 (rule 4) require a horizontal wheel "only in a report that supports pan or with a fallback".
- Prior art: Consumer Control and `AC Pan` in one composite interface with a report ID — we do **not** copy that layout.

## Decision

- **TECH-072 SHOULD:** a third, separate HID interface (subclass 0, protocol NONE) with a Consumer Control report (Usage Page 0x0C)
  and a 16-bit usage (itf 2). Interfaces itf 0 (boot keyboard) and itf 1 (boot mouse) remain unchanged.
- **TECH-073 SHOULD:** `AC Pan` (0x0C:0x0238) as an additional mouse report field in **report protocol**, after the wheel.
  After `SET_PROTOCOL(BOOT)` the mouse sends the boot format only (03 §5.1). If HIL shows a problem, pan moves to itf 2.
- **TECH-074 MUST:** the Consumer Control usage is in the pressed-ledger; DOWN → UP (usage 0) as in FR-203/FR-307;
  `RELEASE_ALL` also sends an empty Consumer report.
- **TECH-075 MUST:** only usages from a compiled-in allowlist are permitted (first version: Volume Up/Down, Mute, Play/Pause,
  Next/Previous Track). The remote CLIENT invokes them exclusively through `input/shortcut` with `command_id` (FR-309), without a raw usage.
- **FR-311 SHOULD:** the SHORTCUTS view has a "Media" group with the above actions.
- Contract 03 §3: the new types `KVM_HID_CONSUMER_DOWN` / `KVM_HID_CONSUMER_UP` are appended **at the end** of the enum,
  without changing the existing values.
- Per 03 §9 item 2, a descriptor change requires: this ADR, a **HOST compatibility matrix** (Windows 11, Linux,
  macOS, ≥ 1 BIOS/UEFI — what works in boot, what only in the OS) and an enumeration test (AT-EXT-04).

## Consequences

- **Endpoint budget** (verified 2026-11-01 in the TinyUSB source `src/portable/synopsys/dwc2/dwc2_esp32.h`:
  `ep_count = 7`, `ep_in_count = 5` including EP0): the ESP32-S3 has **4 usable IN endpoints**. Three HID interfaces take 3.
  USB CDC (console/debug) needs 2 further IN (bulk + notification) and **no longer fits** — debug exclusively through
  the board's separate USB-UART port, never through CDC on the OTG port. Confirm the `esp_tinyusb` 3.0 configuration in TASK-050.
- Some systems/BIOSes may ignore itf 2 — the documentation states what does not work in BIOS (03 §5.1).

## Tests

- AT-EXT-04 (HIL): enumeration of 3 interfaces on Windows 11 and Linux; boot keyboard + boot mouse work in BIOS/UEFI.
- AT-EXT-05 (HIL + unit): every Media action gives DOWN→UP; a loss of USB/owner during it → an empty Consumer report.
- AT-EXT-06 (HIL): horizontal 2-finger scroll in the OS; in boot mode the mouse without the pan field and without interference.
