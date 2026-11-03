# ADR-033 — Candidate fonts live in `ime_data`, compressed; the partition grows to 7.7 MiB

Status: ACCEPTED (owner, 2026-11-02)
Date: 2026-11-02
Approver: project owner (decision of 2026-11-02, accepted as proposed)

## Context

ADR-024 puts on-device composition for Japanese, Simplified Chinese and Korean in the main release (FR-746) and the
IME dictionaries in a read-only `ime_data` partition shared by both OTA slots (TECH-081). It does not say where the
glyphs of the candidates come from. TECH-078 budgets the legend fonts inside the application image, and that budget
cannot hold a CJK candidate font:

- Simplified Chinese: the PinyinIME dictionary uses 16,463 distinct characters; with CJK and full-width punctuation
  that is 16,580 glyphs. At 24 px and 4 bpp the plain bitmaps are 4.58 MB.
- Japanese: kana and the 6,355 kanji of JIS X 0208 (what the OpenWnn dictionaries produce), 6,655 glyphs.
  Japanese text must be drawn with Japanese glyph forms (Noto Sans JP), not with the Chinese forms of Noto Sans SC,
  and Noto Sans SC's subset has no kana, so the two languages cannot share one font.

The table of TASK-000 gave `ime_data` 6 MiB at `0x820000`, followed by `coredump` (64 KiB) and an unused `spare`
partition of 1.8 MiB that only the M1 flash-stress diagnostic writes to.

Measured contents of the image with both languages (2026-11-02):

| File | Size |
|---|---|
| `dict_pinyin.dat` (PinyinIME) | 1,068,442 B |
| `font_hanzi_24.bin` (Noto Sans SC, compressed) | 3,330,280 B |
| `font_kanji_24.bin` (Noto Sans JP, compressed) | 1,292,412 B |
| `wnn_ja.bin` (OpenWnn conversion dictionaries) | 889,841 B |
| OFL text | 4,301 B |

6,602,752 B in all as a packed image (SHA256 `1f9f0775…b2fe`, reproducible): more than 6 MiB, even with the fonts compressed.

## Decision

1. **Candidate fonts ship in `ime_data`, not in the app image.** Each is a KVMFONT1 blob
   (`components/ui_theme/include/ui_font_blob.h`) that the firmware memory-maps and LVGL reads in place. There is no
   copy to RAM. The blob is SHA-256-verified at mount like every file of the image. The loader also checks every
   table and glyph before LVGL sees them. The fonts are derived from Noto Sans SC / JP (OFL-1.1, notofonts/noto-cjk
   tag `Sans2.004`, pinned by SHA-256) by `tools/fonts/gen_hanzi_font_blob.py` (`--set zh|ja`), reproducibly; the OFL
   text travels in the same image.
2. **The glyphs are stored in LVGL's own RLE compression with the line prefilter** (blob version 2,
   `LV_FONT_FMT_TXT_COMPRESSED`, `CONFIG_LV_USE_FONT_COMPRESSED=y`). This is two thirds of the plain size, and the
   decoder is LVGL's own, so there is nothing to vendor. LVGL's bit reader has no end bound, so the loader runs a
   bounded copy of the same state machine over every glyph. It refuses any stream that would make LVGL read past its
   part. A glyph is decoded per draw; the candidate strip draws at most a few dozen characters.
3. **`ime_data` grows to `0x7B0000` (7.7 MiB) at the same offset.** `coredump` moves to `0xFD0000` (64 KiB, unchanged
   size) and `spare` shrinks to 128 KiB at `0xFE0000`, which is all the flash-stress diagnostic needs. The OTA slots,
   NVS, `otadata`, `phy_init` and `nvs_keys` do not move, so an existing device keeps its settings, pairing register
   and keys. Re-flashing the table moves only the core dump area and the spare area. Free space in the image after
   both languages: about 1 MB.
4. **A language whose image file is missing or fails its check is simply unavailable.** Its layer cap stays disabled
   (fail closed). Nothing else depends on `ime_data`.

## Alternatives

| Option | Verdict |
|---|---|
| Plain (uncompressed) fonts | REJECTED — 4.58 MB for Chinese alone; with Japanese and the dictionaries it needs 8.4 MB |
| One shared CJK font from Noto Sans SC | REJECTED — no kana in the SC subset, and Japanese drawn with Chinese glyph forms |
| A smaller character set (GB 2312 / JIS level 1 only) | REJECTED — a candidate the decoder offers could not be shown |
| 2 bpp glyphs | REJECTED — visibly worse CJK strokes at 24 px for a saving the partition change makes unnecessary |
| Fonts in the app image | REJECTED — TECH-078 budget; both OTA slots would carry them twice |
| Keep the 6 MiB table, drop the Japanese dictionary to microSD | REJECTED — FR-746 needs Japanese on the device without extra hardware |

## Consequences

- `partitions.csv` changes (`ime_data` size, `coredump` and `spare` offsets). Board #1 needs the table and a new
  `ime_data` image flashed once. No user data moves.
- The image no longer fits a future 8 MiB unit's layout. That was already true of two 4 MiB OTA slots plus any
  `ime_data` (TECH-051, ADR-009).
- ADR-031 item 4 (`ime_data` updates through the signed OTA path) covers the larger image unchanged. The text of ADR-031
  that says "6 MiB" describes the table at the time it was written.
- Tests: `test/ui_components/test_ui_font_blob.c` (plain and compressed blobs, damage, every truncation, random
  corruption drawn under AddressSanitizer, the real blobs); the simulator renders the candidate strips from the real
  blobs.
