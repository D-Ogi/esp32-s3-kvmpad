# `tools/`

Helper scripts: flash, statistics export, checking of requirement IDs (02 §3.1).

- `verify_spec_manifest.py` — verifies the SHA256 sums of the v1.1.0 specification package at its target paths in the repository.
- `check_license_headers.py` (ADR-025) — verifies the three-line license header in every authored source file and that files
  with a foreign SPDX id are listed in `THIRD_PARTY_NOTICES.md`; `--fix` inserts missing headers, `--list` shows every verdict.
  Blocking in CI from TASK-000.
- `kbd_layouts/gen_layouts.py` (TASK-071, ADR-024) — generates `components/kbd_layouts/generated/` (DEVICE tables, a
  report of shipped / rejected layouts, the legend code points) and `clients/windows/layouts/kbd_layouts.json` (CLIENT tables
  with the character → keystroke index) from xkeyboard-config 2.48, libxkbcommon 1.13.2 and keycodemapdb, all pinned in
  the script (`--print-pins`). It needs libxkbcommon, so it runs on a Linux host: the workflow
  `.github/workflows/kbd-layouts.yml` builds the pinned sources, runs the generator twice (determinism), runs
  `test/kbd_layouts` over the result, uploads the regenerated tree as an artifact and requires an empty diff against the
  committed outputs. `kbd_layouts/test_gen_layouts.py` tests the generator on any host with a fake keymap compiler.

- `fonts/gen_hangul_font.py` (ADR-024 TECH-078) — regenerates `components/ui_theme/fonts/kvm_font_hangul_24.c` from Noto Sans
  KR (notofonts/noto-cjk tag `Sans2.004`, pinned by SHA256) with lv_font_conv 1.5.3 under Node.js. Both inputs live in
  `../.tools/` (`npm install lv_font_conv@1.5.3` with the npm cache there too); the output is reproducible byte for byte.
- `fonts/gen_hanzi_font_blob.py` (TASK-072) — rasterizes every character of the PinyinIME dictionary plus CJK and
  full-width punctuation from Noto Sans SC (noto-cjk `Sans2.004`, `SubsetOTF/SC`, pinned by SHA256) with lv_font_conv
  1.5.3, in runs that keep each part's bitmap under the 1 MiB `bitmap_index` limit of LVGL 9.6, and packs them into a
  KVMFONT1 blob (`components/ui_theme/include/ui_font_blob.h`) for the `ime_data` image. Version 2 of the format:
  the glyphs are LVGL-compressed (RLE with the prefilter, `LV_USE_FONT_COMPRESSED`), about 27% smaller (ADR-033).
  `--set ja` takes Noto Sans JP (`SubsetOTF/JP`, pinned by SHA256) and the JIS X 0208 repertoire instead, for the JA
  candidate strip (`font_kanji_24.bin`). `--work` and `--out` must be outside the repository; both outputs are
  reproducible — `--set zh`: 3,330,280 B, 4 parts, 16,580 glyphs, SHA256 `59aaa4a4…3923`; `--set ja`: 1,292,412 B,
  2 parts, 6,655 glyphs, SHA256 `363c4200…6f81`.
- `ime/build_wnn_blob.py` (ADR-024 TECH-081, TASK-072) — extracts the Japanese dictionaries the OpenWnn clause conversion
  needs from the vendored `components/ime_engine/openwnn/data/WnnJpnDic.c` (the single-kanji, independent-word and
  ancillary-word dictionaries plus the connection matrix; the four prediction dictionaries of the same file are left out,
  the conversion never sets them) and packs them into the `wnn_ja.bin` container that
  `components/ime_engine/include/ime_wnn.h` documents. Checks every array against the file's own `dic_size[]` table and
  every header against the dictionary type the engine must see. Needs no compiler and is reproducible: 889,841 B,
  SHA256 `d816222a…9d2a8`. `build_ime_data.py` calls it, so it rarely needs running on its own.
- `ime/build_ime_data.py` (ADR-024 TECH-081, TASK-072) — builds the vendored PinyinIME dictionary builder with a **32-bit**
  compiler (the dictionary layout of the ESP32-S3; a 64-bit compiler is refused), runs it on the vendored raw dictionary and
  packs `dict_pinyin.dat` into the `ime_data` partition image (`components/ime_engine/include/ime_data.h`). `--work` and
  `--out` must be outside the repository. `--hanzi-font` adds the ZH candidate font blob and the OFL text that travels with
  it (`ofl_noto_cjk.txt`), `--kanji-font` the JA one, and `wnn_ja.bin` goes in unless `--no-openwnn` is given. Every
  combination is reproducible byte for byte:

  | Files | Size | SHA256 |
  |---|---|---|
  | `dict_pinyin.dat` | 1,073,152 B | `2c75a717…c3cb` |
  | + `--hanzi-font` (format 1) | 5,668,864 B | `c407c7bd…2ec6d` |
  | + `--hanzi-font` (format 2) + `wnn_ja.bin` | 5,308,416 B | `3d1d786b…0472` |
  | + `--hanzi-font` + `--kanji-font` (format 2) + `wnn_ja.bin` — the release set | 6,602,752 B | `1f9f0775…b2fe` |

  The release set does not fit the 6 MiB partition of TASK-000; ADR-033 grows `ime_data` to `0x7B0000` (the builder's
  default `--partition-size`), which leaves 1,458,176 B free. A merged CJK blob was rejected there: Simplified Chinese
  and Japanese need different glyph forms for the same code points.

  Flash the image once to the `ime_data` partition; app flashes do not touch it:

  ```sh
  node=<abs path>/node; conv=../.tools/lv_font_conv/node_modules/lv_font_conv/lv_font_conv.js
  python -I tools/fonts/gen_hanzi_font_blob.py --font ../.tools/dl/fonts/sc/NotoSansSC-Regular.otf \
      --lv-font-conv $conv --node $node --work ../build-ime-data/hanzi --out ../build-ime-data/font_hanzi_24.bin
  python -I tools/fonts/gen_hanzi_font_blob.py --set ja --font ../.tools/dl/fonts/jp/NotoSansJP-Regular.otf \
      --lv-font-conv $conv --node $node --work ../build-ime-data/kanji --out ../build-ime-data/font_kanji_24.bin
  python -I tools/ime/build_ime_data.py --cxx <abs path>/i686-w64-mingw32-clang++.exe --cxx-flag=-static \
      --hanzi-font ../build-ime-data/font_hanzi_24.bin --kanji-font ../build-ime-data/font_kanji_24.bin \
      --work ../build-ime-data --out ../evidence/ime/ime_data4.bin
  ../.tools/venv/Scripts/python -m esptool --chip esp32s3 --port COM3 write-flash 0x820000 ../evidence/ime/ime_data4.bin
  ```

## Local tooling — never inside the repo

The tools live in the workspace directory **above** the repository (`../.tools/`), never inside the repo. A venv with esptool (v5) for hardware measurements:

```sh
python -m venv ../.tools/venv
../.tools/venv/Scripts/pip install esptool
../.tools/venv/Scripts/python -m esptool --chip esp32s3 --port COM3 flash-id
```

The board's UART port (CH343) reports itself as `USB-Enhanced-SERIAL CH343 (COMx)`.
