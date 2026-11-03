# `ui_platform`

> **Status:** `VERIFIED_HW` for the platform itself (TASK-060A, board #1, 2026-11-02): LVGL 9.6.0 renders the 1024×600
> scene through both backends; numbers in `test/reports/2026-11-02_M5_TASK-060A_d36ab0a.yaml`, decision in
> `docs/ADR/ADR-027-lvgl-integration-backend.md` (adapter 0.7.4, `DOUBLE_DIRECT` default; ACCEPTED 2026-11-02).
> Touch is not routed yet (TASK-060E) and nothing here emits HID.

**Role:** LVGL 9.6 ↔ `esp_lcd` integration on the `board_hal` RGB panel: display registration, the LVGL task and
tick, the recursive lock, render metrics and a screen snapshot. **The only owner of the LVGL context** (AGENTS-UI
MUST 3): everything else calls LVGL only between `ui_platform_lock()` / `ui_platform_unlock()`.

| | |
|---|---|
| Tasks | TASK-060A |
| Specification | 07 §0 (ADR-013), §9; 02 §2, §6; ADR-026 (1024×600) |
| Tests | `ui bench <scene> [s]`, `ui demo` (LVGL benchmark demo), `ui snap` + `test/hil/ui_snapshot.py`; HIL on board #1 |

| File | Content |
|---|---|
| `include/ui_platform.h` | `ui_platform_required_fbs / start / lock / unlock / display / metrics_* / fps_x100 / snapshot` |
| `include/ui_bench.h` | TASK-060A scenes (`idle`, `small`, `full`, `scroll`) and the LVGL benchmark demo wrapper — test code, removed when `ui_views` exist |
| `src/ui_platform.c` | backend-independent part: start order, display event metrics (`REFR/RENDER/FLUSH` start→ready), snapshot |
| `src/ui_backend_adapter.c` | `espressif/esp_lvgl_adapter` 0.7.4 (candidate): tearing mode from Kconfig, panel frame buffers 1..3 |
| `src/ui_backend_port.c` | `espressif/esp_lvgl_port` 2.9.0 (controlled fallback): `avoid_tearing` direct mode on 2 panel fbs or a partial DMA buffer |
| `src/ui_bench.c` | the KVMpad-like 1024×600 scene (status bar, rail, pad, button bar) and its animations |
| `Kconfig` | `KVM_UI_PLATFORM`, backend choice, adapter tearing mode, draw-buffer lines, LVGL task priority/core/stack, `KVM_UI_BENCH` |

**Backend selection (ADR-013 spike):** `CONFIG_KVM_UI_BACKEND_ADAPTER` (default) or `CONFIG_KVM_UI_BACKEND_PORT`.
Exactly one backend source is compiled and linked; both components are pinned in `idf_component.yml` because a
Kconfig-conditional dependency rule cannot be evaluated before `sdkconfig` exists on a fresh configure. The losing
backend is removed from the manifest when ADR-013 is decided. One binary per backend; the report names both.

**Frame buffers:** `ui_platform_required_fbs()` is asked by `app_main` before `board_lcd_init()` — TRIPLE_PARTIAL
needs 3 panel frame buffers (3 × 1.2 MB PSRAM), DOUBLE_* and the port's `avoid_tearing` 2, NONE 1. Bounce buffers stay
at the TECH-020 variant A (2 × 10 lines). Both backends register their own RGB panel callbacks; while LVGL owns the panel
the `board_lcd` VSYNC statistics of M1 are no longer updated and the raw `lcd <screen>` commands are refused.

**Metrics (`ui`):** render FPS = `RENDER_READY` events per second over the window; `render_avg/max_us`
(`RENDER_START→READY`), `flush_avg/max_us` (`FLUSH_START→FINISH`, includes waiting for the panel), `cycle_max_us`,
the adapter's own FPS counter when built with `ESP_LVGL_ADAPTER_ENABLE_FPS_STATS`, per-core CPU load from the idle
tasks, free internal / PSRAM heap and the LVGL task stack high-water mark.

**Rules (MUST):**

- No GT911 access here (one read path: `touch_input`; the bridge is TASK-060E). No `tusb.h` / `tinyusb.h`.
- The LVGL lock covers LVGL only — never I²C, TLS, USB or NVS.
- The LVGL task runs below the touch sampler and the TinyUSB task (`KVM_UI_TASK_PRIORITY` 4) and on core 1 by default.
- Do not change the backend "on the fly": the choice belongs to ADR-013 with HIL numbers.

Governing documents: `AGENTS.md`, `AGENTS-UI.md`.
