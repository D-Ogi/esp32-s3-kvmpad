# ADR-034 — No permanent flash mappings: IME fonts streamed, OpenWnn dictionaries in PSRAM

Status: ACCEPTED (owner, 2026-11-02)
Date: 2026-11-02
Approver: project owner (decision of 2026-11-02, accepted as proposed after the board verification)
Amends: ADR-033 item 1 ("memory-maps and LVGL reads in place"), TECH-081 "read in place" as applied to the fonts

## Context

The first signed-OTA run on board #1 (ADR-031 plan, row 1, image `ota-b` received at 300 kB/s) ended in an
unexpected reset at about 50 % of the download. The panic was `Cache disabled but cached memory region accessed`. The
decoded backtrace was `lcd_rgb_panel_fill_bounce_buffer` ← `lcd_rgb_panel_eof_handler` ← GDMA ISR, while the OTA task
was inside `spi_flash_op_block_func` (evidence `evidence/hardware/20261102-185147-ota-row1.log`, SHA256
`584e6db7e5534fe2f16cf8fe2ecbbdfbe23d86354807e1a1a4fdf1b395301651`).

Cause, from the pinned ESP-IDF v6.1 sources:

- The application executes from PSRAM (`CONFIG_SPIRAM_XIP_FROM_PSRAM=y`). Then `spi1_start()` in
  `components/spi_flash/spi_flash_os_func_app.c` keeps the cache enabled during a flash write **only while no flash
  region is memory-mapped** (`flash_mmap_remain()`, the count of live `spi_flash_mmap` / `esp_partition_mmap` blocks).
  Otherwise it disables the cache for the write.
- `ime_glue` kept three permanent mappings of the `ime_data` partition (`ime_data_mmap()`): the hanzi font, the kanji
  font and `wnn_ja.bin`. LVGL read the fonts and OpenWnn read its dictionaries in place.
- So every flash write after the IME loaded disabled the cache. The RGB panel's bounce-buffer interrupt is IRAM-safe,
  but it copies from the frame buffer in PSRAM, so it faulted. This hit OTA writes, and the same path is taken by NVS
  writes (Settings, the pairing register, the Wi-Fi BSSID cache). Any of them could reset the device.

Options that keep the mappings do not work:

- `CONFIG_SPI_FLASH_AUTO_SUSPEND`: the ESP-IDF documentation excludes it when interrupts are frequent (the panel's
  end-of-frame interrupt is).
- An RGB interrupt that is not IRAM-safe: the panel is restarted outside VSYNC
  (`LCD_RGB_RESTART_IN_VSYNC` stays off because of a permanent 10-line shift found in M1), so a missed bounce buffer
  shifts the image.
- Mappings with `SPI_FLASH_MMAP_FLAG_BLOCKS_WRITE`: a write then waits until the mapping is released, so a permanent
  mapping blocks OTA for ever, and a mapping per lookup has to rebuild every pointer into the blob on each call.

## Decision

1. **Nothing of `ime_data` is memory-mapped.** `ime_data_mmap()` is removed. `ime_data_file()` /
   `ime_data_file_read()` read a verified file with `esp_partition_read()` (a flash read keeps the cache on), and
   `ime_data_load()` copies a verified file into PSRAM. The SHA-256 check at mount is unchanged; an unverified file is
   still never handed out.
2. **Candidate fonts are streamed** (`ui_font_blob_open()`). The loader validates the blob exactly as before (the
   same checks and the same statuses, through a 16 KiB read window). It copies the glyph tables and cmap lists into
   one PSRAM block: 156,468 B for the hanzi font, measured on the host against the real blob. It gives each part
   LVGL 9.6's own on-demand glyph loader (`lv_font_fmt_txt_dsc_t.are_glyphs_dynamic_loaded`, the mechanism of LVGL's
   binfont loader). When LVGL draws a glyph, the loader reads that glyph's bitmap stream from flash into a per-part
   scratch buffer under the part's mutex. The length read is the longest stream of the part, which the bounded RLE
   walk measures at load. A glyph stream longer than 1,024 B is refused; a 24 px 4 bpp glyph is at most 288 B plain.
3. **The OpenWnn dictionaries are copied into PSRAM** (889,841 B) and stay there for the lifetime of the JA layer.
   The engine reads them in place, as before.
4. The in-place mode (`ui_font_blob_load()`) stays for the host tests and the simulator, where the blob is in memory.

## Consequences

- PSRAM: about 1.1 MB more is used once both languages are loaded (WNN 0.89 MB, hanzi tables 0.16 MB, kanji tables
  and loaders). Before the change, board #1 had 1.77 MB of PSRAM free with the IME loaded. The figure after the
  change is measured on the board (report `test/reports/2026-11-02_M8_TASK-110-ota_d36ab0a.yaml`).
- Drawing a CJK glyph costs one flash read of at most a few hundred bytes. During a flash erase or write, the read
  waits for the flash driver, so the candidate strip can lag behind a running OTA download by up to one sector
  erase. HID, touch and other text do not wait.
- With no mapping, a flash write keeps the cache on. The RGB interrupt keeps working during OTA and NVS writes,
  which is what ADR-031's plan needs.
- Rule for later code: no component keeps a flash mapping. A new `esp_partition_mmap` / `spi_flash_mmap` needs its
  own ADR.
- Tests: `test/ui_components/test_ui_font_blob.c` runs every blob of the suite through both modes. This covers the
  damage table, every truncation and 6,000 random corruptions. Each blob must give the same status in both modes and,
  when it loads, the same pixels for every sample glyph; every glyph read must stay inside the blob; nothing may stay
  allocated after `ui_font_blob_close()`. Every read and allocation of an open fails once in turn and is refused
  cleanly. The real hanzi blob decodes the same in both modes over 4,096 sampled code points (U+3000..U+9FFF, every 7th).
