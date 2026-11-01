# Third-party notices

KVMpad itself is licensed under PolyForm Noncommercial 1.0.0 (`LICENSE.md`). The code and data listed here come from other
projects, keep their own licenses and must not receive the KVMpad license header (ADR-025). `tools/check_license_headers.py`
reads the path column of the tables below; every vendored path must appear there. `Settings → About` renders this file (FR-751).

## Vendored files (copied into this repository)

| Path (glob) | Component | Upstream, version / commit | License |
|---|---|---|---|
| _none yet_ | | | |

Planned by ADR-024 (TASK-071/072): `components/kbd_layouts/generated/*` (derived from xkeyboard-config, Expat and
Expat-Open-Group; keycodemapdb, BSD-3-Clause), `components/ime_engine/openwnn/*` and `components/ime_engine/pinyinime/*`
(AOSP, Apache-2.0), legend font subsets (Noto, OFL-1.1). Each gets a row here when it is added, with the exact version.

## Dependencies fetched at build time (not copied; managed by the ESP-IDF component manager / `dependencies.lock`)

| Component | License |
|---|---|
| ESP-IDF | Apache-2.0 |
| LVGL | MIT |
| TinyUSB | MIT |
| Espressif managed components (`esp_lvgl_port` or `esp_lvgl_adapter`, `esp_lcd_touch_gt911`, `esp_io_expander_ch422g`, …) | Apache-2.0 (as declared in each component's manifest) |

Exact versions are pinned in `dependencies.lock` from TASK-000 on; this table names licenses, not versions.
