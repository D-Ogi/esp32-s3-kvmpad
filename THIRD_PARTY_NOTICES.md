# Third-party notices

KVMpad itself is licensed under PolyForm Noncommercial 1.0.0 (`LICENSE.md`). The code and data listed here come from other
projects, keep their own licenses and must not receive the KVMpad license header (ADR-025). `tools/check_license_headers.py`
reads the path column of the tables below; every vendored path must appear there. `Settings → About` renders this file (FR-751).

## Vendored files (copied into this repository)

| Path (glob) | Component | Upstream, version / commit | License |
|---|---|---|---|
| `components/ui_theme/fonts/kvm_font_hangul_24.c` | Hangul legend font of the KO composition strip: a 24 px 4 bpp subset (compatibility jamo + the 2350 KS X 1001 syllables) derived from Noto Sans KR Regular by `tools/fonts/gen_hangul_font.py` with lv_font_conv 1.5.3 (MIT; a build tool, not shipped). Copyright 2014-2021 Adobe (http://www.adobe.com/). Noto is a trademark of Google Inc. | notofonts/noto-cjk, tag `Sans2.004`, `Sans/SubsetOTF/KR/NotoSansKR-Regular.otf` (SHA256 `69975a0a…148d68`) | OFL-1.1 (`components/ui_theme/fonts/OFL-NotoSansKR.txt`) |
| `components/ime_engine/openwnn/*` | OpenWnn (iWnn) dictionary search engine (Japanese, FR-746) — the whole directory; the two rows after this one cover its dictionary data and the files derived from the upstream Java. This row covers `include/*`, `engine/*` and `NOTICE`: the freestanding C engine (`ndapi`, `ndbdic`, `ndcommon`, `ndfdic`, `ndldic`, `ndrdic`, `neapi`, `necode`, `nj_str`) and its headers, copied unchanged from `jni/` of the upstream tree; the upstream `Android.mk`/`Android.bp`, the Java sources and the JNI glue are omitted. Copyright (C) 2008-2012 OMRON SOFTWARE Co., Ltd.; Copyright (C) 2008-2012 The Android Open Source Project (`NOTICE`). | AOSP `platform/packages/inputmethods/OpenWnn` at commit `5067ec8ec5a80ab12218c961f9b083fbccc71b93` (archive SHA256 `b0c9b076…00163`) | Apache-2.0 (`components/ime_engine/openwnn/NOTICE`) |
| `components/ime_engine/openwnn/data/WnnJpnDic.c` | OpenWnn Japanese dictionaries as C byte arrays (single-kanji, independent-word, ancillary-word, four prediction dictionaries, connection matrix), copied unchanged. **Not compiled into the firmware:** `tools/ime/build_wnn_blob.py` extracts the four arrays the clause conversion needs into the `wnn_ja.bin` file of the `ime_data` image. | as the OpenWnn row above | Apache-2.0 |
| `components/ime_engine/openwnn/kvm/wnn_romkan.*`, `components/ime_engine/openwnn/kvm/wnn_convert.*` | **Derived work:** C translations by KVMpad (2026-11-02) of the upstream Java and JNI sources — `src/jp/co/omronsoft/openwnn/Romkan.java` (romaji → kana table and matcher), `src/jp/co/omronsoft/openwnn/OpenWnnClauseConverterJAJP.java` (clause conversion), `src/jp/co/omronsoft/openwnn/OpenWnnDictionaryImpl.java` and `jni/OpenWnnDictionaryImplJni.c` (dictionary driver). Each file names its source and the deliberate differences (fixed memory instead of unbounded maps, no user or learning dictionary). They keep the upstream Apache-2.0 header, so they take no KVMpad header. Copyright (C) 2008-2012 OMRON SOFTWARE Co., Ltd.; Copyright (C) 2008-2012 The Android Open Source Project. | as the OpenWnn row above | Apache-2.0 |
| `components/ime_engine/pinyinime/*` | PinyinIME decoder (Simplified Chinese, FR-746): the engine sources (`include/`, `share/`), the dictionary builder (`command/`, a build tool: it makes `dict_pinyin.dat` for the `ime_data` partition image, `tools/ime/build_ime_data.py`) and the raw dictionary data (`data/`). Copied unchanged except two leak fixes in `share/dicttrie.cpp` and `share/spellingtrie.cpp`, each marked `KVMpad local change (2026-11-02)`; the upstream `Android.mk`/`Makefile` are omitted. Copyright (C) 2009 The Android Open Source Project (`NOTICE`). | AOSP `platform/packages/inputmethods/PinyinIME`, `jni/` at commit `49aebad1c1cfbbcaa9288ffed5161e79e57c3679` (archive SHA256 `f25215bb…ee302`) | Apache-2.0 (`components/ime_engine/pinyinime/NOTICE`) |

## Data in the `ime_data` partition image (built by `tools/ime/build_ime_data.py` outside the repository, flashed separately)

| File in the image | Component | Upstream, version / commit | License |
|---|---|---|---|
| `dict_pinyin.dat` | PinyinIME system dictionary, built from `components/ime_engine/pinyinime/data/` by the vendored builder | as the PinyinIME row above | Apache-2.0 |
| `font_hanzi_24.bin` | Hanzi font of the ZH candidate strip (TASK-072): every character of the pinyin dictionary plus CJK and full-width punctuation, 24 px 4 bpp (16 580 glyphs, LVGL-compressed, KVMFONT1 version 2 container read in place), derived from Noto Sans SC Regular by `tools/fonts/gen_hanzi_font_blob.py` with lv_font_conv 1.5.3 (MIT; a build tool, not shipped). Copyright 2014-2021 Adobe (http://www.adobe.com/). Noto is a trademark of Google Inc. | notofonts/noto-cjk, tag `Sans2.004`, `Sans/SubsetOTF/SC/NotoSansSC-Regular.otf` (SHA256 `faa6c9df…6d5ea9`) | OFL-1.1 (shipped next to it in the image as `ofl_noto_cjk.txt`) |
| `wnn_ja.bin` | Japanese dictionaries of the OpenWnn clause conversion (TASK-072): the single-kanji, independent-word and ancillary-word dictionaries and the connection matrix, extracted byte for byte from `components/ime_engine/openwnn/data/WnnJpnDic.c` by `tools/ime/build_wnn_blob.py` into the `KVMWNNJA` container that `components/ime_engine/include/ime_wnn.h` documents, and read in place by the engine | as the OpenWnn row above | Apache-2.0 |
| `font_kanji_24.bin` | Kanji / kana font of the JA candidate strip (TASK-072, ADR-033): the JIS X 0208 repertoire plus Japanese punctuation, 24 px 4 bpp (6 655 glyphs, LVGL-compressed, KVMFONT1 version 2), derived from Noto Sans JP Regular by `tools/fonts/gen_hanzi_font_blob.py --set ja` with lv_font_conv 1.5.3. Copyright 2014-2021 Adobe (http://www.adobe.com/). Noto is a trademark of Google Inc. | notofonts/noto-cjk, tag `Sans2.004`, `Sans/SubsetOTF/JP/NotoSansJP-Regular.otf` (SHA256 `dff723ba…5073`) | OFL-1.1 (the same `ofl_noto_cjk.txt`) |

## Data derived into generated KVMpad files (TASK-071, ADR-024)

These files are written by `tools/kbd_layouts/gen_layouts.py` and carry the KVMpad header plus a provenance header; the
layout data inside them comes from the projects below. Legend font subsets for the layouts (Noto, OFL-1.1, TECH-078) get a
row when they are added.

| Generated path | Data taken | Upstream, version / commit | License |
|---|---|---|---|
| `components/kbd_layouts/generated/*`, `clients/windows/layouts/kbd_layouts.json` | Layout ids and English names (`rules/base.xml`) and the keysyms of each key per level (`symbols/*`, resolved by libxkbcommon), stored as UTF-8 legends. Copyright the xkeyboard-config contributors (see the upstream `COPYING`). `symbols/lv` (HPND-sell-variant) is not used | xkeyboard-config 2.48 (tarball SHA256 `b7704132…7648fd5`) | Expat and Expat-Open-Group |
| `components/kbd_layouts/generated/*`, `clients/windows/layouts/kbd_layouts.json` | XKB key name → USB HID usage (`data/keymaps.csv`). Copyright the keycodemapdb authors | keycodemapdb commit `54788039…0177af29` | BSD-3-Clause (chosen of GPL-2.0-or-later OR BSD-3-Clause) |
| (build tool, not shipped) | libxkbcommon compiles the keymaps on the Linux tooling host; nothing of it is copied | libxkbcommon 1.13.2, commit `d1442aaa…c118c962` | MIT and MIT-style |

## Dependencies fetched at build time (not copied; managed by the ESP-IDF component manager / `dependencies.lock`)

| Component | License |
|---|---|
| ESP-IDF | Apache-2.0 |
| LVGL | MIT |
| TinyUSB | MIT |
| Espressif managed components pinned by `dependencies.lock` (2026-11-02): `esp_tinyusb`, `esp_lcd_touch`, `esp_lcd_touch_gt911`, `esp_lvgl_adapter`, `esp_lvgl_port` (until ADR-027 item 1), and the adapter's transitive `button`, `knob`, `cmake_utilities`, `esp_lv_decoder`, `esp_lv_fs`, `esp_mmap_assets`, `esp_new_jpeg` | Apache-2.0 (as declared in each component's manifest) |
| `espressif/freetype` (transitive, adapter) | FreeType License (FTL) / GPL-2.0 dual |
| `espressif/libpng`, `espressif/zlib`, `espressif/qoi` (transitive, adapter) | libpng-2.0 / Zlib / MIT |

Exact versions are pinned in `dependencies.lock` from TASK-000 on; this table names licenses, not versions.

## Built from the pinned ESP-IDF tree into other KVMpad binaries (not copied)

| Component | Used by | License |
|---|---|---|
| Mbed TLS 4.x / TF-PSA-Crypto 1.x (`components/mbedtls` of ESP-IDF v6.1) | firmware (TLS, PSA, EC-JPAKE pairing); Windows CLIENT `kvmpad_pairing.dll` (`clients/windows/native`, ADR-028/029) | Apache-2.0 (chosen of Apache-2.0 OR GPL-2.0-or-later) |
| lodepng (inside LVGL, `LV_USE_LODEPNG`) | desktop simulator only (`simulator/lvgl_sdl`), never the firmware | Zlib |

The Windows CLIENT's managed projects take no NuGet package at run time; the test project's packages (xunit, the VSTest SDK) are
pinned in `clients/windows/Directory.Packages.props` and are not shipped.
