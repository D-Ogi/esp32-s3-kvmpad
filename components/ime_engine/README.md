# `ime_engine`

> **Status:** `IN PROGRESS` (TASK-072) — the Korean 2-set composer (`include/ime_hangul.h`) is implemented and
> host-tested (`test/ime`, AT-EXT-26 Hangul part). The PinyinIME decoder (`include/ime_pinyin.h`) runs on board #1 from the
> `ime_data` partition (footprint spike, `test/reports/2026-11-02_M5_TASK-072-pinyin_d36ab0a.yaml`); only the `ime bench`
> console command calls it so far. The OpenWnn Japanese conversion (`include/ime_wnn.h`) is implemented and host-tested on
> the real dictionaries (`test/ime_wnn`, `test/reports/2026-11-02_M5_TASK-072-openwnn_d36ab0a.yaml`), `NOT_RUN_HW`. The
> candidate bar and the flick keyboard are not done yet.

**Role:** on-device text composition (ADR-024 FR-746): Japanese (OpenWnn — flick kana / romaji → kana → kanji with candidates),
Simplified Chinese (PinyinIME — pinyin → hanzi candidates), Korean (in-house algorithmic Hangul composer). Produces Unicode text
that `kbd_layouts` turns into keystrokes or Unicode delivery sequences. Never touches HID or LVGL.

| | |
|---|---|
| Tasks | TASK-072 (footprint spike on board #1, engines, `ime_data` partition, candidate bar / flick keyboard with `ui_views`) |
| Specification | ADR-024 (FR-746..748, TECH-080/081); 01 §4; 03 §6.5 (rate budget); 02 §9 (partitions, OTA) |
| Tests | AT-EXT-26/27, AT-FAULT-07; host suites `test/ime`, `test/ime_pinyin` (32-bit) |

## Parts

| Path | What |
|---|---|
| `include/ime_hangul.h`, `src/ime_hangul.c` | Korean 2-set composer, fixed state, no heap |
| `include/ime_data.h`, `src/ime_data.c` | `ime_data` image directory parser (magic `KVMIMEDT`, version 1, ≤ 8 files of 4096-aligned data with SHA256), pure C, host-tested |
| `include/ime_data_vfs.h`, `src/ime_data_vfs.c` | finds the `ime_data` partition, verifies every file's SHA256 (PSA) once and mounts a read-only VFS at `/ime` (2 descriptors; writes fail with `EROFS`); `ime_data_file()` / `ime_data_file_read()` read a verified file directly (the streamed candidate fonts read one glyph per call) and `ime_data_load()` copies one into PSRAM (the OpenWnn dictionaries), never an unverified one. Nothing is memory-mapped (ADR-034): a permanent mapping makes every flash write disable the cache, which the RGB panel's IRAM interrupt cannot survive |
| `include/ime_pinyin.h`, `src/ime_pinyin.cpp` | bounded C API over PinyinIME: input only `a`–`z` and `'`, ≤ 26 characters; candidate ids checked against the last count; the user dictionary is always disabled (no learned text is stored) |
| `include/ime_wnn.h`, `src/ime_wnn.c` | bounded C API over OpenWnn, the same semantics as `ime_pinyin` so one function table drives both: input only `a`–`z`, `-` and `'` (a syllable separator), ≤ 50 letters; candidate 0 is the whole sentence, candidates 1..31 are alternatives for the first clause not yet fixed; one work area allocated at open, nothing per keystroke; no user and no learning dictionary |
| `openwnn/engine/`, `openwnn/include/` | vendored AOSP OpenWnn dictionary engine (Apache-2.0, `openwnn/NOTICE`), compiled unchanged. Freestanding C: no libc, no heap of its own — it works inside the `NJ_CLASS` / `NJ_DIC_SET` the caller owns |
| `openwnn/data/WnnJpnDic.c` | vendored Japanese dictionaries as C byte arrays. **Not compiled**: `tools/ime/build_wnn_blob.py` extracts the four the conversion needs into the `wnn_ja.bin` container of the `ime_data` image, and the firmware reads them in place |
| `openwnn/kvm/wnn_romkan.*` | C translation of the romaji → hiragana table and its incremental matcher (`Romkan.java`), with the input-letter range of every kana so the API can map kana positions back to letters |
| `openwnn/kvm/wnn_convert.*` | C translation of the clause conversion (`OpenWnnClauseConverterJAJP.java`) and of the dictionary driver (`OpenWnnDictionaryImpl.java`, `OpenWnnDictionaryImplJni.c`): single-clause and consecutive-clause conversion with the original scores and search order, on fixed memory (the unbounded `HashMap` caches of the original become ranges of one word arena indexed by the kana positions of the reading, the sub-sentence objects become one best cell per end position with a back pointer) |
| `src/ime_alloc.cpp`, `port/ime_alloc.h` | heap routing: the engine's C heap calls are renamed to `ime_engine_*` at compile time and prefer PSRAM; `operator new` prefers PSRAM only inside an `ImeAllocScope` on the calling task, every other task keeps the default heap |
| `linker.lf` | places the engine's static `.bss` in PSRAM (`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`) |
| `pinyinime/` | vendored AOSP PinyinIME (Apache-2.0, `NOTICE`), see `THIRD_PARTY_NOTICES.md`; two marked local leak fixes |

The dictionary image is built by `tools/ime/build_ime_data.py` and flashed once to the `ime_data` partition (`tools/README.md`).

## Measured on board #1 (2026-11-02)

PinyinIME with the pinned dictionary (1,068,442 B): open 121 ms (+ 225 ms SHA256 check at mount); search mean 2–5 ms,
worst 9.3 ms, worst while typing a 19-letter input one letter at a time 7.9 ms (target ≤ 50 ms); about 1.04 MiB of PSRAM
while open, ~80 KiB kept by the engine's singletons after the first close, no further growth over open/close cycles;
internal heap unchanged; worker stack 10 KiB with 7.5 KiB unused.

With the hanzi font in the image (5,668,864 B, three files) the mount check takes 1166 ms. The firmware does it off the
boot path: `main/ime_glue` mounts, maps the font, validates it and opens the decoder in a priority-1 worker, then the
LVGL owner installs the font and the decoder into the KEYBOARD view (`IME glue installed` in `ime status`; PSRAM free
about 1.96 MB afterwards). From then on only the LVGL task calls the decoder, and `ime bench` refuses to run.

## Measured on the host (OpenWnn, 2026-11-02)

The Japanese conversion has no board numbers yet (`NOT_RUN_HW`). On the host, with the four dictionaries of `wnn_ja.bin`
(889,841 B) mapped in place: one allocation per `ime_wnn_open()` and nothing afterwards — 103,408 B of work area on a
64-bit host, 91,316 B on a 32-bit one (the board's layout), 0 B live after `ime_wnn_close()` and no growth over
open/close cycles or over 50 keystrokes; `ime_wnn_search()` 0.00–0.21 ms mean for inputs of 5 to 50 letters (`-O2`),
against the ≤ 50 ms budget. The firmware image grew 41,392 B with the engine compiled in (the measurement also contains
the parallel UI work of the same build).

**Rules (MUST):**

- Engines under Apache-2.0 only (OpenWnn, PinyinIME from AOSP), with their own license headers and entries in
  `THIRD_PARTY_NOTICES.md`; no GPL/LGPL engine or dictionary (TECH-080).
- Dictionaries are read from the read-only `ime_data` partition (shared by both OTA slots) or from the microSD slot; never
  downloaded at runtime (TECH-081).
- Memory inside the engine's own arena; no heap growth per keystroke; conversion latency target ≤ 50 ms per keystroke.
- Composed text leaves only as a cancellable burst through typed intents → `InputArbiter` (FR-748); the engine never calls the
  HID layer and holds no key state.
- No `tusb.h`, no `lvgl.h`; the UI (preview, candidates, flick grid) lives in `ui_views` / `ui_components`.
- Call the engine from one task at a time (it is not reentrant); logs never contain the input or the candidates.

Governing documents: `AGENTS.md`.
