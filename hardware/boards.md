# Board unit record

Required by ADR-009 (`docs/spec/06-DECISIONS-AND-RISKS.md` §10): the Flash size (8 vs 16 MiB) and PSRAM
must be **measured** on every board unit before the OTA partition table is created.
Enter only values read from the tools — do not fill them in from documentation.

**Model:** Waveshare **ESP32-S3-Touch-LCD-7B** (1024×600, CH32V003 I/O expander) — the v1 target board by
[ADR-026](../docs/ADR/ADR-026-target-board-7b.md). The 7C and the versions without touch remain out of scope.

## Procedure (read-only — never `burn_*`, never `write_flash` as part of a measurement)

```sh
# tools live in the workspace directory ABOVE the repository (../.tools/), never inside the repo — see tools/README.md
../.tools/venv/Scripts/python -m esptool  --chip esp32s3 --port COM3 flash-id    # chip, revision, PSRAM from eFuse, JEDEC flash id, size
../.tools/venv/Scripts/python -m espefuse --chip esp32s3 --port COM3 summary     # READ-ONLY: PSRAM_CAP/VENDOR, secure boot / encryption state
# boot log: reset via RTS (DTR=0, RTS 1→0), read at 115200 for ~8 s → lines boot.esp32s3 / esp_psram / spi_flash
```

Save raw logs outside the repository: `../evidence/hardware/<YYYY-MM-DD>_<tool>_<MAC without colons>.txt`;
enter the key lines and the file's SHA256 into the record.
The board's UART port (CH343, VID 1A86 / PID 55D3) appears in Windows as `USB-Enhanced-SERIAL CH343 (COMx)`.

## Board units

| ID | MAC (eFuse) | Module | Chip | Flash (measured) | PSRAM (measured) | Date | Who / tool |
|---|---|---|---|---|---|---|---|
| **#1** | `44:1b:f6:c9:22:9c` | ESP32-S3-WROOM-1-**N16R8** (inferred from the measurements: 16 MB flash + 8 MB octal PSRAM) | ESP32-S3 QFN56, **rev v0.2**, eFuse block rev v1.4, 40 MHz | **16 MB**, JEDEC `46` / `4018`, quad (4 lines), 3.3 V, boot QIO | **8 MB** octal, vendor AP (`0x0d`, AP_3v3), gen 3, 64 Mbit, 3 V | 2026-11-01 | D-Ogi; esptool 5.6.0 |

### Board unit #1 — details and evidence

| | |
|---|---|
| Port | `COM3` (CH343) |
| `flash-id` | `2026-11-01_esptool-flash-id_441bf6c9229c.txt` — SHA256 `8262faa29a1dbfcff43f2e31da436ef98664a9d480b17145b62e5066d4021e47` |
| `espefuse summary` | `2026-11-01_espefuse-summary_441bf6c9229c.txt` — SHA256 `76419c57f7cdee0287bfecb01538705340fa19cf51a837a75a7fdd63b1120b9b` |
| UART boot log | `2026-11-01_uart-boot-log_441bf6c9229c.txt` — SHA256 `f8539c3db26ef1b794d72af4b9626300b09b4026ef212d0f076c74dfac41104c` |

The raw logs sit **outside the repository**, in the evidence store of the workspace directory (`../evidence/hardware/`);
the key lines and the files' SHA256 sums remain in the record.

**Confirmation from the firmware (boot log, factory Waveshare software, not KVMpad):**
`boot.esp32s3: SPI Flash Size : 16MB` · `esp_psram: Found 8MB PSRAM device` · `esp_psram: Speed: 120MHz` ·
`octal_psram: density 0x03 (64 Mbit)` · `spi_flash: flash io: qio` · `boot: chip revision: v0.2`.

**Security state (eFuse, read):** secure boot disabled (`SECURE_BOOT_EN = False`), flash encryption disabled
(`SPI_BOOT_CRYPT_CNT = Disable`), JTAG enabled (`DIS_PAD_JTAG = False`), `DIS_DOWNLOAD_MANUAL_ENCRYPT = False`. A development board in the
factory state — **burn nothing** (TECH-054, ADR-012).

**Factory firmware (to be replaced):** ESP-IDF bootloader `v5.4-dev-3951-g9106c43acc-dirty`, built 2025-06-20.
Partition table: `nvs` 24 KiB @0x9000, `phy_init` 4 KiB @0xF000, `factory` **8 MiB** @0x10000 — **no OTA**.
Flash HPM: "High performance mode of this flash model hasn't been supported" → this flash does **not** run in 120 MHz mode.
Free internal memory after the factory firmware boots: 236 + 21 + 32 KiB RAM + 7 KiB RTCRAM; the PSRAM pool in the heap is 5120 KiB
(the rest is taken by `.text/.rodata` mapped into PSRAM and a 32 KiB reserve for DMA).

### Takeaways for the project (board unit #1)

1. **ADR-009 resolved: 16 MiB Flash, 8 MiB PSRAM.** TECH-051: a partition table with ≥ 2 OTA slots, `otadata`, NVS
   (encrypted in production) + `nvs_keys`, `phy_init`, `coredump`, room for the device certificate/key. Slot sizes
   only after `idf.py size` in TASK-000 — do not copy the factory table. Do not flash this table onto an 8 MiB board unit,
   should one appear (measurement first).
2. **PSRAM 120 MHz** in the factory demo is an experimental feature in ESP-IDF (it requires explicit consent to experimental options).
   ADR-008 / TASK-020: an A/B benchmark of 80 vs 120 MHz PSRAM under RGB + Wi-Fi + USB; do not assume 120 MHz up front.
3. **Flash stays ≤ 80 MHz** (no HPM support in this flash model) — calculate the throughput budget without 120 MHz flash.
4. **Chip rev v0.2:** set the minimum revision in `sdkconfig.defaults` in line with this board unit (TASK-000); with further
   board units, check that the revision matches.
5. **Memory reference point:** ~289 KiB of free internal RAM after the factory firmware boots — measure our 64 KiB soft floor
   (02 §11.2) against this, bearing in mind that KVMpad will add TinyUSB, TLS and LVGL.

### Board unit #1 — identification as a 7B and KVMpad bring-up (2026-11-02, TASK-010/020/030/050)

Measured with KVMpad firmware (`kvmpad d36ab0a` + uncommitted M0–M2 work, ESP-IDF v6.1) through the console on `COM3`
(`test/hil/kvm_serial.py`); raw logs in `../evidence/hardware/`.

| Item | Measured | Evidence (SHA256) |
|---|---|---|
| I²C devices (GPIO8/9, 400 kHz) after the 7B init | **`0x24`, `0x5D`** — stable | `20261102-021315-cmd.log` `b3e15835ee7113036a2c1938edfe9037f3784bb33ab1af0337c12dd913827709` |
| `0x24` is not a CH422G | CH422G output/input command addresses `0x38` / `0x26` do not ACK; reads of `0x24` return register bytes | `20261102-015609-ch422g-probe.log` `9867da2dcf3334201331179f6f9ece86e7688c802697ca569f1882f434fa3613`, `20261102-015756-ch422g-probe-2.log` `0675646a824a13b18576807406a0cab54848fe6332ece8480618f915c862fda4` |
| Expander safe state | output `0x10` written, read-back of register `0x04` = `0x10`, EXIO5 = 0 (USB) before `tud_init`; after init `0x5E` = `0x5E` | `20261102-021301-boot.log` `c9e296aca77fcbfaf75d85a349d9e3072730321e4c84de2154f57654c8e4b1ae` |
| Expander keeps state over an ESP32-S3 reset | after a watchdog reset the first read-back was `0x58` (previous levels) — hence the bounded re-apply loop (ADR-026 TECH-086) | `20261102-020951-boot.log` `f07c1534d2a57581cd6a187d548c11d1ce4b771619590f57353e45ebfb57ee94` |
| GT911 | `0x5D`, product `"911"`, fw `0x1060`, info 1024×600, config version 91, config max 1024×600, 5 points, INT mode 1 | `20261102-021301-boot.log` (above) |
| RGB panel | 1024×600 RGB565, PCLK 30 MHz, VSYNC 32.746–32.747 Hz measured vs 32.745 Hz nominal, 0 late frames idle (ADR-038, rejected: at PCLK 25 MHz VSYNC 27.288 Hz measured; 70 s finger-hold sequence 2 missed frames vs 25 at 30 MHz, `20261102-var2-hold.log`, but the scrim under modals flickered; back to 30 MHz) | `20261102-021315-cmd.log` (above) |
| USB (Windows 11 Pro 10.0.26200) | composite `VID_303A&PID_400C`, serial `441BF6C9229C`; MI_00 `Class_03&SubClass_01&Prot_01`, MI_01 `…&Prot_02`, MI_02 `Class_03&SubClass_00&Prot_00`; Keyboard, Mouse and HID consumer nodes status OK | `20261102-021406-usb-enum-windows.log` `73902d3d9b86d40a1dcf836061874bbc61d695b3d3721c51c29eaec9f6f18e6a` |
| Connectors | `Type_C2` ("USB") = HID to the HOST, starts the board alone; `Type_C1` ("UART") = CH343 debug (`COM3`), no HID on it | `20261102-032916-usb-enum-windows.log` `d73f181eb288b40ae3587f7d7b436daef87114077106d4dbe786610caa44d27b`, `20261102-033011-cmd.log` `a065616f22fea1128a8349709cc67cd5326c0b80df1b49d6bdbd17daebcfa800` |
| Power path (schematic) | `Type_C2` VBUS = board 5V rail (TVS only, no diode); rail also fed from `Type_C1` (AO3401 ideal diode Q10), battery boost CS8501 and header J9 → back-feeds the HOST port under auxiliary power; firmware cannot sense HID-port VBUS (ADR-026 consequences) | `ESP32-S3-Touch-LCD-7B-Schematic.pdf` `60d31638254809aa39af0c5cc774ae4d2ebb73bdc4e57f68593cc4f559dec76c` |
| Cable removed under auxiliary power | USB state `SUSPENDED`, `umounts=0` (not `DETACHED`); re-plug → new epoch 2, neutral reports, enumeration PASS | `20261102-033132-cmd.log` `b43a28382e8899743cbe1e0501b448d0807b8df79dff37ad60da58f7f18ceda1`, `20261102-033302-cmd.log` `ce18adbc3f27a9492b212acbaccdc743dcef6a4bcfd5e71298afa4b45f6b464b` |

Before the correct expander init, scans also returned changing phantom addresses (`0x0E`, `0x30`, `0x46`, `0x66`, `0x68`)
next to `0x24`/`0x5D` (`20261102-015837-board-identity.log`
`605cb779d86b8bee8fd7214d425e2b077d0b9b63bc31c88d5ce4a88f859136d3`). They did not appear in any scan after the 7B init; the
cause (bus state while the expander drove unknown levels) is not proven — re-check if they return.

