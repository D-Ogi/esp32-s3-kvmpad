# ADR-042 — Optional keyboard language modules
Status: PROPOSED
Date: 2026-11-19
Approver: project owner

The owner allows Chinese, Japanese and Korean input support to be optional modules stored on microSD or in flash. The first implementation uses flash and reads existing `KVMIMEDT` version 1 images. This allows existing boards to retain their dictionaries, partition layout and saved language selections. The magnifier and the built-in recovery screen must also work with every optional language disabled.

| Module | Flash resources | Runtime |
|---|---|---|
| Chinese, Simplified | Pinyin dictionary, Hanzi candidate font and OFL notice. Build a Chinese-only image with the existing `--no-openwnn` and `--hanzi-font` options. | A worker opens PinyinIME when Chinese is enabled and a local Unicode delivery mode is active. |
| Japanese | OpenWnn dictionary, Kanji candidate font and OFL notice. Use `--no-pinyin --kanji-font FILE` for a Japanese-only image. This build does not require the 32-bit pinyin compiler. | A worker opens OpenWnn under the same conditions for Japanese. The module owns its dictionary buffer until the decoder closes. |
| Korean | Standard builds embed the Hangul font with `CONFIG_KVM_IME_KOREAN`. The media build reads the optional `font_hangul_24.bin` from the verified `ime_data` partition. Both fonts cover 51 jamo and 2350 KS X 1001 syllables. | The built-in two-set composer uses no dictionary. When the saved Korean toggle and a local Unicode delivery mode are active, the worker loads the font tables and the picker enables Korean. LCD drawing reads individual glyphs from flash. |

The settings snapshot and existing keyboard delivery check drive the loader. English-only and host IME mode skip the dictionary workers. At most one language load runs at a time. A settings change during loading is reconciled on the LVGL owner's next poll. The owner detaches disabled decoders before closing their dictionaries. Font tables that have been published to LVGL remain cached for the lifetime of existing widgets. Missing files disable the affected conversion layer. The existing combined flash image remains supported.

To build the Korean module, run `tools/fonts/gen_hanzi_font_blob.py --set ko` with the pinned Noto Sans KR font and lv_font_conv 1.5.3. Pass its output as `--hangul-font FILE` to `tools/ime/build_ime_data.py`. A Korean-only image uses `--no-pinyin --no-openwnn`. For all three languages, supply the Korean font alongside the Chinese and Japanese fonts and both dictionaries. The tested combined image occupies 6,922,240 bytes of the 8,060,928-byte data partition. It can be provisioned independently of either OTA application. The data image selected for provisioning must contain every language the operator wants to keep.

The flash modules work with the microSD slot empty. If memory pressure or a flash read interrupts a file's SHA-256 check, the next language load retries that file. A completed digest comparison is reused until reboot, including a mismatch. A temporary registration failure can be retried without rereading verified dictionaries. To retry a failed language from Settings, toggle it off and on after memory becomes available.

A future microSD language backend would require a verified read-only data source and safe removal while a font glyph or candidate list is in use. Executable code is built into trusted firmware. Installable UI profiles cannot introduce native decoder code. [Storage and recovery](ADR-040-extension-platform.md)

Acceptance includes cold boot with no local composition, Chinese only, Japanese only, both, and a firmware build with Korean omitted. Exercise toggle changes during a load, a failed dictionary verification, active composition on disable, repeat enable/disable, a return to English and free/low-water heap with media enabled. Compare firmware builds using the same saved settings. [Test evidence](../../test/reports/2026-11-19_extension-media_022168e4.yaml)
