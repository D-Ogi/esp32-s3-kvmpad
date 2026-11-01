# `hid_core`

> **Status:** `DESIGNED` — directory created in the repository skeleton; no implementation yet.
> Build files (`CMakeLists.txt`, `idf_component.yml`) are created in TASK-000 from a working
> build — do not write them "from memory".

**Role:** HID source of truth: keymap, pressed-ledger, `InputArbiter` (owner + epoch), `RELEASE_ALL`, `HidTranslator`/`HidScheduler`.

| | |
|---|---|
| Tasks | TASK-050 (host-unit part) |
| Specification | 02 §7.2, §7.3; 03 §3, §4 (normative arbiter pseudocode), §5; ADR-010, ADR-011 |
| Tests | AT-HID-01..09, AT-KEY-04/05/10, VEC-005..009 |

**Rules (MUST):**

- The only path to HID: typed intents → `InputArbiter` → `HidScheduler`.
- DOWN/UP, mode/revoke and `RELEASE_ALL` must not be lost; queue overflow → fail-safe `RELEASE_ALL` + counter.
- No `lvgl.h`. TinyUSB only in `usb_hid_device` (the core must be testable on the host).
- Two implementers never edit `hid_core` in parallel (05 §12).

**Extensions (`docs/EXTENSIONS.md`):**

- ADR-018: Consumer usage in the ledger, allowlist, `RELEASE_ALL` clears Consumer (TECH-074/075); new intents `KVM_HID_CONSUMER_DOWN/UP`.
- ADR-020: `KVM_HID_KEEPALIVE` (origin SYSTEM) — the only exception to TECH-064, conditions of FR-713 (TECH-076).

Governing documents: `AGENTS.md`.
