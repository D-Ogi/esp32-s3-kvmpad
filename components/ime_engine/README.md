# `ime_engine`

> **Status:** `DESIGNED` — directory created in the repository skeleton; no implementation yet.
> Build files (`CMakeLists.txt`, `idf_component.yml`) are created in TASK-000 from a working
> build — do not write them "from memory".

**Role:** on-device text composition (ADR-024 FR-746): Japanese (OpenWnn — flick kana / romaji → kana → kanji with candidates),
Simplified Chinese (PinyinIME — pinyin → hanzi candidates), Korean (in-house algorithmic Hangul composer). Produces Unicode text
that `kbd_layouts` turns into keystrokes or Unicode delivery sequences. Never touches HID or LVGL.

| | |
|---|---|
| Tasks | TASK-072 (footprint spike on board #1, engines, `ime_data` partition, candidate bar / flick keyboard with `ui_views`) |
| Specification | ADR-024 (FR-746..748, TECH-080/081); 01 §4; 03 §6.5 (rate budget); 02 §9 (partitions, OTA) |
| Tests | AT-EXT-26/27, AT-FAULT-07 |

**Rules (MUST):**

- Engines under Apache-2.0 only (OpenWnn, PinyinIME from AOSP), with their own license headers and entries in
  `THIRD_PARTY_NOTICES.md`; no GPL/LGPL engine or dictionary (TECH-080).
- Dictionaries are read from the read-only `ime_data` partition (shared by both OTA slots) or from the microSD slot; never
  downloaded at runtime (TECH-081).
- Memory inside the engine's own arena; no heap growth per keystroke; conversion latency target ≤ 50 ms per keystroke —
  `NOT_RUN_HW` until measured in the spike.
- Composed text leaves only as a cancellable burst through typed intents → `InputArbiter` (FR-748); the engine never calls the
  HID layer and holds no key state.
- No `tusb.h`, no `lvgl.h`; the UI (preview, candidates, flick grid) lives in `ui_views` / `ui_components`.

Governing documents: `AGENTS.md`.
