# ADR-026 — Target board: Waveshare ESP32-S3-Touch-LCD-7B (1024×600, CH32V003 expander)
Status: ACCEPTED (owner, 2026-11-02)
Date: 2026-11-02
Approver: project owner (decision of 2026-11-02: "the 7B must not be excluded — it is the board we build on")

## Context

- The frozen specification names a single board: **ESP32-S3-Touch-LCD-7** — 800×480 RGB panel, GT911, **CH422G** I/O
  expander (`docs/spec/02-TECHNICAL-SPECIFICATION.md` §1, §1.1, §6.1; `AGENTS.md` §1, which also says "NOT for the 7B/7C").
- Board unit #1 on the bench (`hardware/boards.md`, MAC `44:1b:f6:c9:22:9c`) is a **ESP32-S3-Touch-LCD-7B**. Measured in
  TASK-010 on 2026-11-02 (raw logs in `../evidence/hardware/`, SHA256 in `hardware/boards.md`):
  - I²C bus GPIO8/GPIO9 carries exactly two devices, `0x24` and `0x5D`.
  - `0x24` is **not** a CH422G: the CH422G command addresses `0x24` (system) answer but `0x38` (output) and `0x26`
    (input) do not ACK. Register reads return register-file bytes. It is the board's **CH32V003** microcontroller acting as an
    I/O expander with a register protocol (Waveshare 7B schematic and demo, see Sources).
  - GT911 at `0x5D`: product `"911"`, firmware `0x1060`, information block **1024×600**, configuration version 91,
    configured output maximum **1024×600**, 5 touch points.
  - The panel shows the test card with the 7B timing below (seen by the owner on the device; VSYNC measured
    32.746 Hz against a nominal 32.745 Hz). A bounce-buffer scan-out desync seen at first boot is a driver-configuration
    matter of TASK-020, not of the timing (see the TASK-020 report).
- The RGB data/sync GPIOs, the touch IRQ (GPIO4), the USB pins (GPIO19/20) and the module (N16R8) are the same as on the 7.
  What differs is the panel resolution and timing, the expander device and its EXIO roles, and the backlight (PWM on the
  expander instead of an on/off line).
- Sources: Waveshare ESP32-S3-Touch-LCD-7B wiki and schematic (`7B-schematic.pdf`, SHA256
  `60d31638254809aa39af0c5cc774ae4d2ebb73bdc4e57f68593cc4f559dec76c`); Waveshare demo repository for the 7B, commit
  `c652c902db607f7ffb376257393cfd7657aa6428` (facts only — register map, timing, reset sequence; no code copied).

## Options

| | Option | Assessment |
|---|---|---|
| A | Keep the spec board (7), obtain a 7 and treat the 7B as unsupported | REJECTED by the owner — the 7B is the board in hand and the intended product hardware |
| B | Target the **7B** as the only v1 board; keep every invariant of the spec and translate the board-specific clauses | **CHOSEN** |
| C | Support both boards behind a run-time board detection | REJECTED for v1 — two BSPs and two UI geometries double the HIL matrix; can be revisited by a later ADR |

## Decision

**TECH-085 MUST — target board.** The v1 hardware is the **Waveshare ESP32-S3-Touch-LCD-7B**. Wherever `02`, `04`, `05`
and `AGENTS.md` name the "ESP32-S3-Touch-LCD-7" as the target, read "ESP32-S3-Touch-LCD-7B"; the exclusion of the 7B in
`AGENTS.md` §1 is lifted. The 7C, boards without touch and plain DevKits remain excluded. Every requirement that is not
board-geometry or expander specific (HID, security, `RELEASE_ALL`, ownership, one GT911 reader, USB independent of Wi-Fi)
is unchanged.

**TECH-086 MUST — expander.** The I/O expander is the CH32V003 at I²C `0x24` with this register protocol (write = `{reg, value}`;
read = write `{reg}`, then read one byte):

| Register | Meaning |
|---|---|
| `0x02` | direction, `0xFF` = all outputs |
| `0x03` | output levels |
| `0x04` | input levels (reads back the driven outputs; used for verification) |
| `0x05` | backlight PWM, **inverted**: `0` = full brightness; the vendor caps the duty at 97 % → raw values `0..247` |
| `0x06` | ADC word (battery sense; not used in v1) |

EXIO roles (schematic): EXIO1 = touch reset, EXIO2 = backlight enable, EXIO3 = LCD reset, EXIO4 = SD_CS,
**EXIO5 = USB_SEL (LOW = USB, HIGH = CAN)**, EXIO6 = LCD VDD enable; EXIO0 and EXIO7 are not connected.
Spec 02 §1.1 rows "CH422G EXIOn" map one to one onto these EXIO numbers, plus EXIO6 for the panel supply.

- The expander is a separate MCU and is **not** reset with the ESP32-S3: after a warm reset it keeps driving the previous
  levels. The board HAL writes the safe byte (USB selected, resets asserted, backlight and panel supply off, SD_CS high) before
  and after the direction register, re-applies it a bounded number of times and starts USB **only** after the read-back of
  register `0x04` confirms it (TECH-010 / AT-HW-04 unchanged in intent).
- The vendor demo starts from an all-HIGH shadow byte, which implicitly selects **CAN** on EXIO5. That pattern must not be
  copied; the shadow logic refuses EXIO5 = HIGH (`components/board_hal/src/io_ext.c`, host test `test_io_ext.c`).

**TECH-087 MUST — panel.** 1024×600 RGB565, 16-bit parallel, PCLK 30 MHz, HSYNC pulse/back/front 162/152/48, VSYNC pulse/
back/front 45/13/3, PCLK active on the falling edge (nominal refresh 32.745 Hz). Power-up: LCD reset LOW → panel VDD on →
20 ms → reset HIGH → 100 ms. The frame-buffer budget of 02 §6.1 becomes:

```text
RGB565  = 1024 × 600 × 2 = 1 228 800 B (1.17 MiB)
2 × framebuffer        = 2 457 600 B (2.34 MiB) in PSRAM
2 × 10-line bounce     = 40 960 B internal DMA-capable memory
2 × 20-line bounce     = 81 920 B internal DMA-capable memory
```

**TECH-088 MUST — touch geometry.** GT911 at `0x5D` (address strap with INT LOW during reset; `0x14` still probed as a
fallback), configured maximum 1024×600, mapped to screen pixels `0..1023` × `0..599` by the explicit transform of TECH-011.
The touch contract of 03 §2 is unchanged except for the coordinate range.

## Consequences

- **UI (M3, TASK-060..070):** the 800×480 HTML reference (`reference/kvm-essential-800x480.html`) remains the UX source of
  truth for layout rules, hierarchy and touch targets, but the device canvas is 1024×600 (×1.28 horizontally, ×1.25
  vertically). TASK-060D/070x scale the geometry; minimum touch-target sizes are kept in millimetres, not pixels. The SDL
  simulator runs at 1024×600. This is the only change of UX scope this ADR makes.
- **Memory:** one frame buffer grows by 450 KiB of PSRAM; double buffering (variant C) costs 2.34 MiB. The bounce
  buffers use 40–80 KiB of internal DMA RAM, still within budget.
- **Backlight:** brightness becomes a setting (inverted PWM through the expander); EXIO2 stays the on/off line.
- **Power and HOST status (02 §1.2 item 7):** the VBUS pins of the HID connector (Type_C2) are wired straight to the
  board 5V rail, which is also fed from the UART connector (ideal diode), the battery boost and the 5V header J9. The
  supported configuration is power from the HID port alone. With auxiliary power the board back-feeds VBUS of the HOST
  port, and the firmware cannot tell a removed cable or a HOST that is off from a suspended HOST (the device sees only
  bus idle; VBUS sensing is impossible because the HID-port VBUS is the 5V rail). `usb_hid_device` therefore reports
  `SUSPENDED` in both cases, and the HID core (M3) must treat it as HOST not active (no HID, `RELEASE_ALL`); "HOST off" is
  not a distinct state on this board. Auxiliary power with a HOST that may be off requires a VBUS-blocking cable or adapter.
- **Records:** `hardware/boards.md` names the 7B; TASK-010/020/030 reports cite this ADR for every deviation from 02 §1/§6.
- **Spec manifest:** unchanged — the frozen files are not edited; this ADR is the override.

## Tests

- AT-HW-01..05 as in `04` with the 7B values: I²C scan `0x24` + `0x5D`; EXIO5 read-back LOW before `tud_init`; GT911
  configuration 1024×600; VSYNC at the nominal rate; USB enumeration after cold and warm reset.
- Host unit test `test/host_unit/test_io_ext.c` (EXIO5 = HIGH unreachable, safe default, inverted PWM).
- Evidence: `../evidence/hardware/*` (SHA256 in `hardware/boards.md` and `test/reports/`).

## Migration

- `components/board_hal`: the CH422G driver is replaced by the CH32V003 register driver (`io_ext.*`, `board_hal.c`);
  `board_pins.h` carries the 7B values with source references.
- No data migration (nothing was released for the 7).
