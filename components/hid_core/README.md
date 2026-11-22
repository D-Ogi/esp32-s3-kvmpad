# `hid_core`

**Role:** HID source of truth: keymap, pressed-ledger, `InputArbiter` (owner + epoch + lease), `RELEASE_ALL`, `HidScheduler`,
modifier FSM.

**Tests.** `test/host_unit/test_kbd_fsm.c`, `test_hid_core.c` (AT-HID-03..08, AT-KEY-04/05/10, AT-FAULT-05, AT-EXT-05/09), `test_hid_vectors.c` (VEC-005..009, AT-SEC-02/03 and AT-CONN-05..08 core part), `test_hid_stress.c` (AT-HID-09, 10k `RELEASE_ALL`, seeded random walk; `test_hid_stress.exe <seed>` reproduces), `test_hid_burst.c` (FR-748 text burst: pacing, refusals, cancel order, abort on every release)

| File | Content |
|---|---|
| `include/kvm_hid_intent.h` | 03 §3 intent types; `KVM_HID_CONSUMER_DOWN/UP` and `KVM_HID_KEEPALIVE` appended |
| `include/hid_keymap.h`, `src/hid_keymap.c` | usage constants, valid-usage checks, compiled shortcut allowlist (`kvm_chord_id_t`), Consumer allowlist |
| `include/kbd_fsm.h`, `src/kbd_fsm.c` | OFF → ARMED → LOCKED modifier FSM, one-shot sent atomically with the next key |
| `include/hid_core.h`, `src/hid_core.c` | arbiter, ledger, bounded queue (128, move coalescing), scheduler (one report per change, waits for the HOST read; chord phases; ±127 split; completion timeout), keep-alive, snapshot, counters |
| `include/hid_dispatch.h`, `src/hid_dispatch.c` | firmware owner: producer queue (64, static), never-dropped `RELEASE_ALL` bitmask, 10 ms tick, runs the core only in the TinyUSB task (03 INV-001, TECH-034), publishes the snapshot |

**Delivery model.** Every ledger change becomes one report on its interface; the next change waits for the HOST to read the
previous one, so a short DOWN is never merged with its UP (FR-203) and each chord phase is a real IN transfer. A report not read
within 250 ms ends in `RELEASE_ALL` without repeating a chord. Motion and wheel coalesce; any other overflow is a fail-safe
`RELEASE_ALL` + counter. A queued or interrupted shortcut always ends with `CHORD_DELIVERED` or `CHORD_ABORTED`.

**Rules (MUST):**

- The only path to HID: typed intents → `InputArbiter` → `HidScheduler`.
- DOWN/UP, mode/revoke and `RELEASE_ALL` must not be lost; queue overflow → fail-safe `RELEASE_ALL` + counter.
- No `lvgl.h`. TinyUSB only in `usb_hid_device` (the core must be testable on the host).
- Two implementers never edit `hid_core` in parallel (05 §12).
- Logs carry reasons and counters only, never key usages.

## Product behavior

- Consumer usage in the ledger, allowlist, `RELEASE_ALL` clears Consumer (TECH-074/075); new intents `KVM_HID_CONSUMER_DOWN/UP`;
  a remote CLIENT reaches Consumer only through a shortcut id.
- `KVM_HID_KEEPALIVE` (origin SYSTEM) — the only exception to TECH-064, conditions of FR-713 (TECH-076); default OFF.
- composed text (IME / Unicode delivery) arrives as a cancellable burst of ordinary key intents from `LOCAL`, paced
  within the 03 §6.5 budget; no new HID source, no new report; cancel / mode switch / USB or owner loss → `RELEASE_ALL` (FR-748).
