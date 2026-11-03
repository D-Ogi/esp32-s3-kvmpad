# ADR-016 — Placement of the keyboard modifier state machine (`kbd_fsm`) and the `Fn` layer
Status: ACCEPTED (owner, 2026-11-02)
Date: 2026-11-02
Approver: project owner (decision of 2026-11-02, accepted as proposed)

## Context

- FR-204/205 define one-shot (`ARMED`) and `LOCKED` modifiers; FR-206 the Caps Lock LED; FR-207 `Fn` as a local layer, not a
  HID key; FR-209 shortcuts that do not inherit keyboard modifiers; 05 TASK-070 lists `kbd_map` and `kbd_fsm` as deliverables
  but names no component. `docs/REPO-LAYOUT.md` records the gap.
- AGENTS-UI #5: the UI is not the source of truth for DOWN keys; 07 §3 `ui_state_snapshot_t.modifiers_down` is an
  "authoritative keyboard ledger projection". 03 INV-001: only `hid_dispatch` modifies the pressed-ledger.
- FR-302 relies on the Alt lock to step through windows with repeated Tab, so a `LOCKED` modifier must stay pressed on USB
  between keys.

## Options

- **A — `kbd_fsm` in `ui_views`.** The view keeps OFF/ARMED/LOCKED and emits plain key intents. Simple, but the UI becomes the
  source of truth for held modifiers; a UI stall could leave a locked modifier pressed with no owner to release it; the
  snapshot cannot show the real state. Violates AGENTS-UI #5.
- **B — `kbd_fsm` in `hid_core`, run by `hid_dispatch`.** The UI sends typed key/modifier actions; the FSM turns them into key
  intents inside the same transaction as the ledger; `RELEASE_ALL` clears it; the snapshot publishes its state.
- **C — a separate `kbd_fsm` component between UI and arbiter.** Same safety as B with an extra queue hop and a second
  place that must be cleared on every loss path.

## Decision

Option **B**.

- `components/hid_core/kbd_fsm.{c,h}`: pure C, host-testable, executed only in `hid_dispatch` context. Inputs:
  `modifier_tap(usage 0xE0..0xE7, t_us)`, `key_down(usage, t_us)`, `key_up(usage, t_us)`, `cancel()`. Outputs: ordinary
  `KVM_HID_KEY_DOWN/UP` intents with origin `LOCAL`, through the arbiter (no second HID path).
- States per modifier: `OFF → ARMED` (one tap), `ARMED → LOCKED` (second tap within 300 ms, FR-204), `ARMED → OFF` (a tap
  after the window), `LOCKED → OFF` (one tap). `ARMED` modifiers are not on USB until the next key: the key DOWN report
  carries the armed modifiers and the key together (FR-205); on key UP the key is released first, then the armed modifiers,
  and they return to `OFF`. `LOCKED` modifiers are pressed on USB when locked and released when unlocked (FR-302).
- `cancel()` — called by `RELEASE_ALL`, owner change, USB loss, view change and shortcut start — returns every modifier to
  `OFF`; the ledger release sends the neutral report. Nothing is restored afterwards.
- Shortcuts (FR-209): a chord from SHORTCUTS first cancels the FSM (armed and locked modifiers up), then runs its own
  allowlisted usage set. A locked Ctrl is never inherited.
- `Fn` (FR-207) is a view state of `ui_view_keyboard` only: it swaps which usages the utility row emits. It never reaches
  `hid_core` and has no HID usage.
- The UI snapshot carries `mods_armed` and `mods_locked` bitmaps (0xE0 → bit 0 … 0xE7 → bit 7) next to
  `modifiers_down`; the view renders OFF/ARMED/LOCKED from them, never from its own guess.

## Consequences

- One more piece of state inside `hid_core`; it is covered by the same release paths as the ledger, so no new stuck-key path.
- The UI loses any local modifier logic: a view rebuild or a UI stall cannot leave a modifier held (07 §1 third invariant).
- Remote `keyboard.key` (03 §6.3) bypasses `kbd_fsm`: a CLIENT sends explicit DOWN/UP and gets no one-shot behaviour.

## Tests

- Host unit (`test/host_unit/test_kbd_fsm.c`): AT-KEY-04 (modifier bits), AT-KEY-05 (tap Ctrl → C gives Ctrl+C once, the next
  key has no Ctrl), AT-KEY-06 (double tap locks, tap unlocks, no stuck), FR-209 (locked Ctrl not inherited by a chord),
  cancel on every loss path leaves no bit set.
- HIL key regression with a person at the panel: `NOT_RUN_HW` until then.

## Migration

None (no keyboard implementation existed).
