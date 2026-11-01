# `test/`

Tests according to `docs/spec/04-TEST-PLAN-AND-ACCEPTANCE-CRITERIA.md`.

| Directory | Layer | Contents |
|---|---|---|
| `host_unit/` | L1 | deterministic FSM/HID/protocol/NVS tests (native C, sanitizers) |
| `fuzz/` | L2 | touch sequences + remote frames; record the seed and the reproducer |
| `hil/` | L3–L4 | pyusb/hidapi/uinput harness, enumeration, traces |
| `ui_store/` | L1 | TASK-060B |
| `ui_nav/` | L1/SDL | TASK-060C |
| `ui_touch/` | L1/L2 | TASK-060E |
| `reports/` | — | test reports (04 §12 format) |

Without a physical board, L3–L6 results are always `NOT_RUN_HW` — never `PASS_HW`.
