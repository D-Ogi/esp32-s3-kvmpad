# `gesture_engine`

> **Status:** `IMPLEMENTED_SW` (M4, 2026-11-02) — pure-C engine with host suites `PASS_SW` (VEC-001..004, AT-GEST-01..12,
> 100k-sequence property run on four seeds) and a mutation check (five hand-made defects, all caught). On the board the
> engine is compiled in and the normative vectors replay on target (`gesture replay`, no HID emitted). Gestures from a real
> finger are `NOT_RUN_HW` until the touch bridge (TASK-060E) connects the GT911 sampler and someone is at the bench.
> Report: `test/reports/2026-11-02_M4_d36ab0a.yaml`.

**Role:** A pure, deterministic gesture state machine: `TouchSample` → `HidIntent` (tap, long-press, two-finger scroll,
two-finger tap, double-tap-drag, drag lock, cancellation paths).

| | |
|---|---|
| Tasks | TASK-040 |
| Specification | 01 §3 (FR-101..116); 03 §2, §2.1, §8 (VEC-001..004); ADR-007 |
| Tests | `test/host_unit/gesture_test_vectors.c` (AT-GEST-01..12, VEC-001..004, FR-112 curve, FR-116 fallback), `gesture_test_property.c` (≥100k random sequences; `gesture_test_property.exe <seed> [n]` reproduces) |

| File | Content |
|---|---|
| `include/gesture_engine.h` | `kvm_gesture_cfg_t` (01 §3.1 starting thresholds 6/8/10 px, 100/220/250/300/550 ms, 120 ms gap, 10 s drag lock capped at 60 s), state enum, `kvm_gesture_feed / tick / cancel / set_drag_lock`, `kvm_gesture_gain` |
| `src/gesture_engine.c` | the classifier of 03 §2.1 with the 01 §3.2 ambiguity rules |

**Behavior decisions (all testable, none hardware-tuned yet):**

- A click is emitted only when the classification is final: tap on UP (≤ 220 ms, ≤ 8 px), long press on the threshold
  (≥ 550 ms, ≤ 10 px, nothing on UP), two-finger tap on the last UP (join ≤ 100 ms, both ≤ 10 px, both up ≤ 250 ms).
- Fingers are followed by `track_id`; a replaced finger re-bases the centroid instead of jumping (VEC-003).
- Two-finger scroll starts after 10 px of centroid travel, locks an axis with hysteresis (a reversal never flips it),
  emits wheel steps from fractional accumulators (`scroll_px_per_step`, natural direction configurable) and blocks every
  click of the same sequence; 2 → 1 fingers waits for `touch_count = 0` (VEC-004).
- Double-tap-and-drag: a tap followed within 300 ms / 32 px by a contact that moves > 8 px sends LEFT DOWN before the
  first MOVE and LEFT UP at the end; without movement it is an ordinary second click.
- Pointer deltas: same-finger differences, `gain = clamp(base + 0.35·speed, 0.7, 2.2)` with Slow/Normal/Fast bases,
  fractional accumulators reset on every new gesture (FR-111..113).
- Every path that leaves LEFT held has a release: finger up, extra finger (01 §3.2 item 3), 3+ fingers, invalid or
  malformed sample, > 120 ms gap (tick or next sample), external cancel (mode switch, owner lost), drag lock off, drag
  lock inactivity. `FR-116`: with `two_finger_enabled = false` two fingers cancel instead of pretending to scroll.

**Rules (MUST):**

- Pure C: without FreeRTOS, ESP-IDF, LVGL, TinyUSB — compiles and is tested on the host.
- Bounded state, zero dynamic allocations; at most `KVM_GESTURE_MAX_OUT` intents per call (overflow counted, must stay 0).
- Monotonic time only, injected from the outside (TECH-006); the caller ticks every ~10 ms.
- One instance per physical panel; the touch bridge decides which samples belong to the PAD area.
- Thresholds change only through an ADR with HIL results (01 §3.1); a change > 20 % updates the contract tests.

Governing documents: `AGENTS.md`.
