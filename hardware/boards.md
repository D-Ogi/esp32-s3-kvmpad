# Board unit record

Required by ADR-009 (`docs/spec/06-DECISIONS-AND-RISKS.md` §10): the Flash size (8 vs 16 MiB) and PSRAM
must be **measured** on every board unit before the OTA partition table is created.
Enter only values read from the tools — do not fill them in from documentation.

**Model:** Waveshare **ESP32-S3-Touch-LCD-7** (not 7B/7C, not the version without Touch).

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
