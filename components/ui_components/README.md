# `ui_components`

> **Status:** `IMPLEMENTED_SW` (TASK-060D) — host tests pass against real LVGL 9.6; not yet built
> into the firmware image and not yet seen on the panel (`NOT_RUN_HW`).

**Role:** Ready-made LVGL components of the product (without USB/network logic).

| | |
|---|---|
| Tasks | TASK-060D |
| Specification | 07 §7 |
| Tests | `test/ui_components` (host, real LVGL): observer lifecycle, geometry, hit boxes, snapshots |

**Files:** `include/ui_components.h` (umbrella) and one header plus one source per component:
`ui_status_item`, `ui_topbar`, `ui_action_key`, `ui_segmented`, `ui_modal`, `ui_toast`.

## The six components

| Component | 07 §7 row | What it is |
|---|---|---|
| `UiStatusItem` | "text, color, state and details" | severity glyph + caption over value, fixed width, clips with dots |
| `UiTopBar` | "navigation + two independent HOST/CLIENT statuses" | fixed slots: brand, nav, spacer, keep-awake, 2 statuses, trailing button |
| `UiActionKey` | "DOWN/UP state, variants" | the only widget that reports a key edge; exactly one UP per DOWN |
| `UiSegmented` | the navigation half of the `UiTopBar` row | the mode tab bar: Mouse / Keyboard / Shortcuts |
| `UiModal` | "overlay + scrim" | scrim blocks everything below it; card, title, body, buttons |
| `UiToast` | "transient message" | self-deleting message strip, never clickable |

`UiSegmented` is the sixth component. 07 §7 gives the `UiTopBar` row as "navigation + two
independent HOST/CLIENT statuses", so navigation is a named part of the product chrome, and
FR-003 makes each tab a touch target in its own right. It is a separate component rather than
code inside `ui_topbar` because the router, not the bar, owns the mode (07 §5): a tap only
reports `control_id` and index, and the selection moves when the bound subject changes.

## Contracts that the tests enforce

- **No HID, ever.** A component reports a typed `control_id` through a caller-owned sink. It does
  not include `tusb.h` or `tinyusb.h` and it does not decide what the HOST receives
  (AGENTS.md prohibition 2, AGENTS-UI MUST 4).
- **No stuck keys.** `ui_action_key` emits exactly one `pressed=false` for every `pressed=true`,
  on release, on press-lost (a finger that slides off), on disable and on delete
  (AGENTS.md prohibition 3).
- **Not the source of truth.** Every state comes from a bound `lv_subject_t`. A zero-initialized
  or unbound projection reads "Unknown", never "ok" (AGENTS-UI MUST 5, 07 §12 item 2).
- **The bar never grows.** Each topbar slot has a fixed width from `ui_tokens.h` and every label
  clips with `LV_LABEL_LONG_MODE_DOTS` pinned to one line, so a 200-character status string
  changes nothing about the layout (07 §7).
- **No allocation of our own.** Descriptors are caller-owned and live in the widget's user data;
  observers are attached with `lv_subject_add_observer_obj`, so they die with the widget.
  10 000 build/teardown cycles of a full shell end at the same live allocation count.

## Lifetime note

`ui_modal_close()` and a button whose row has `closes = true` delete the overlay asynchronously.
After a click that closes a modal, the `lv_obj_t *` the caller holds is dangling; compare pointers
if you must, but do not dereference. The headers state this per function.

**Rules (MUST):**

- Dependency exclusively: `ui_components → ui_theme` (+ LVGL).
- Factory functions of 10–50 lines, descriptor tables.
- Code written by hand in C/LVGL — without EEZ Studio, LVGL Pro Editor, XML/JSON DSL (ADR-014).

**Extensions (`docs/EXTENSIONS.md`):**

- ADR-020: Keep awake indicator in the topbar — text + icon (FR-712). It is hidden unless its
  subject reads exactly 1, so an unknown value never claims the feature is on.

Governing documents: `AGENTS.md`, `AGENTS-UI.md`.
