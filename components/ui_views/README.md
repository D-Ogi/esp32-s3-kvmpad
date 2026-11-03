# `ui_views`

> **Status:** `IMPLEMENTED_SW` — the MOUSE, KEYBOARD and SHORTCUTS views, the HOST / CLIENT status details,
> the pairing and control-approval prompts, Settings and the `ui_app` shell composition are in the tree and
> covered by `test/ui_views/` and the desktop simulator. Hardware results stay `NOT_RUN_HW`.

**Role:** 800×480 screens — exclusively composition of components + local view state.

| | |
|---|---|
| Tasks | TASK-070A (Mouse), TASK-070B (Keyboard), TASK-070C (Shortcuts/Settings/status) |
| Specification | 01 (screen, gestures, keys); 07 §2, §7; AGENTS-UI.md |
| Tests | AT-UI-01..08, AT-KEY-01..10, AT-UIFW-01/04/08 |

**Target files (07 §2):** `ui_view_mouse.c`, `ui_view_keyboard.c`, `ui_view_shortcuts.c`, `ui_view_settings.c`

**Rules (MUST):**

- Dependencies: `ui_views → ui_components → ui_theme`, `ui_views → ui_core`.
- No view callback emits HID — only a `UiAction` to the typed queue.
- Code written by hand in C/LVGL — without UI generators and without `ui_generated/` (ADR-014).

**Extensions (`docs/EXTENSIONS.md`):**

- ADR-018: the "Media" group in SHORTCUTS (FR-311).
- ADR-020: Keep awake toggle (FR-711).
- ADR-022: Settings → Remote screens: *Pair client* (DEVICE name, `ddd ddd` code in a large font, countdown, Accept/Reject
  with the client name) and *Paired clients* (list, Remove, Remove all) — FR-722/725/726.
- ADR-023: Settings → Remote: `Require accept when pairing` (ON), `Control approval: Always / Trusted clients / Auto` (Always),
  with a one-sentence description of the effect; Settings → Shortcuts: Alt+F4 confirmation (FR-731..734). The policy is visible in the status (FR-735).
- ADR-024: the keyboard renders legends and geometry from `kbd_layouts` for the selected `Host layout` (FR-741/742) and shows the
  layout label (FR-508) with the "set the same layout on the HOST" hint; composition mode with preview, candidate bar and the
  Japanese flick grid (FR-746); Settings → Keyboard: `Host layout`, `Unicode input on HOST` with a setup hint per mode (FR-747);
  SHORTCUTS: "Switch HOST layout" (FR-744).
- ADR-025: `Settings → About` — firmware version, license + Required Notice, scrollable third-party notices generated from
  `THIRD_PARTY_NOTICES.md` at build time (FR-751); a plain view without HID or network.
- ADR-037: one language key in the KEYBOARD utility row instead of the `KO` / `ZH` / `JA` caps (a tap cycles, a long press
  opens a picker); `Settings → Keyboard → Input languages` turns Korean, Chinese (pinyin) and Japanese on (FR-752).

## MOUSE view (`ui_view_mouse`, TASK-070A)

The touch surface and a footer: `?`, the hint line, Left click, Right click and Drag lock. The surface is not an LVGL input
target: `ui_touch_bridge` routes touches inside the pad rectangle (`ui_router_pad_area()`) to the gesture engine.

| Behavior | Rule |
|---|---|
| Left / Right click | `ui_action_key` caps: down edge → `UI_ACT_MOUSE_BUTTON_DOWN`, release, lost press or deletion → `UI_ACT_MOUSE_BUTTON_UP`; a tap is a click, a hold is a held button (FR-108) |
| Drag lock | a click submits `UI_ACT_DRAG_LOCK_REQUEST` for the opposite of the authoritative state; the checked look, the `Release` label and the live label follow `UI_SUBJ_DRAG_LOCK` only (FR-107/115) |
| HOST not mounted | every button disabled (a held one is released first) and a `Host disconnected` card on the surface |
| `?` | opens `UI_OVERLAY_GESTURE_HELP`, a blocking router overlay that lists the touchpad gestures (move, tap, hold, two-finger tap and scroll, double-tap drag, Drag lock, 3+ fingers cancel). While it is open `ui_router_pad_area()` returns false, so no touch reaches the gesture engine, and navigation is refused. `Close` submits `UI_ACT_DISMISS_OVERLAY` (the shell dismisses it) and falls back to `ui_overlay_dismiss()`. The key sits in the footer, outside the pad it explains. Nothing is sent to the HOST. `ui_view_mouse_bind_router()` binds the router; without one the key is refused and counted |

## KEYBOARD view (`ui_view_keyboard`, TASK-070B)

US ANSI key field of real tap targets built from `UiActionKey`: a utility row (Esc, the `F1-F12` layer key,
the language key, Insert, Delete, Home, End, PgUp, PgDn), a number row with the shift sub-legends, three letter
rows and a bottom row (Ctrl, Alt, Win, space, AltGr, arrows), plus the KO preedit strip and the ZH / JA candidate strip,
each shown only while its layer is on. Rows are LVGL Flex lines and
each cap's `flex_grow` is the reference key weight × 20; every length is a `ui_tokens.h` token and the 44 px
touch floor comes from `UI_STYLE_BTN_BASE`.

| Behavior | Rule |
|---|---|
| ordinary key | down edge → `UI_ACT_KEY_DOWN(usage)`, up edge or lost press → `UI_ACT_KEY_UP(usage)`; no firmware auto-repeat, at most one held cap (FR-203, FR-208, FR-211) |
| modifier | down edge → `UI_ACT_MODIFIER_TOGGLE(usage)` only; the view never sends a modifier bit with a key (FR-205, ADR-016) |
| modifier look | OFF / ARMED / LOCKED read `UI_SUBJ_MODS_ARMED` and `UI_SUBJ_MODS_LOCKED` alone — ARMED is the shared checked style, LOCKED a local accent style on `LV_STATE_USER_2`; the cap is never checkable (FR-204) |
| CapsLock | an ordinary key (usage `0x39`); the cap sub-legend shows `On` / `Off` / `Unknown` from `UI_SUBJ_CAPS_LED` (FR-206) |
| `F1-F12` | a local layer: the number row and the F-row are both built once and one is hidden, so the switch neither allocates nor covers the main field; a held key is released first (FR-207). The F-caps send a key like any letter, so they wear the enabled `normal` cap, not the grey modifier cap |
| language key (ADR-037 FR-752) | one cap for all input languages: the theme's keyboard symbol and the current code (`EN`, `KO`, `ZH`, `JA`); no globe glyph exists in the theme fonts, so the code is a text label. A **tap** moves to the next language that `Settings → Keyboard → Input languages` has turned on and that is usable now, in the order EN → KO → ZH → JA → EN; a language is skipped while the HOST is not mounted, while the FR-747 delivery mode is `host-ime` or while its decoder is not installed. With English alone the tap shows the toast `Add input languages in Settings > Keyboard.` and changes nothing. A **long press** opens `UI_OVERLAY_KBD_LANG`, a blocking router overlay listing the languages that are on, the current one checked and an unusable one disabled with its reason; a row switches and closes, `Close` submits `UI_ACT_DISMISS_OVERLAY` and falls back to `ui_overlay_dismiss()`. LVGL suppresses the short click after a long press, so one gesture is never both. The cap is disabled while the HOST is not mounted, and the current language is derived from the composer state, never stored twice. `ui_view_keyboard_bind_router()` binds the router; without one the long press is refused and counted. The switch itself is the layer switch below, with its commit and cancel rules; the key sends no HID of its own |
| `KO` (ADR-024 FR-746..748, reached through the language key) | a local Korean 2-set composition layer, enabled only while a HOST is mounted and the FR-747 delivery mode is not `host-ime` (`ui_view_keyboard_set_text_mode()`, driven by `ui_app_set_settings()`; an unpublished record keeps it off). Switching it on submits `UI_ACT_TEXT_CANCEL` first (no armed modifier or queued key reaches the text). Letter caps feed `ime_engine`'s composer; only committed syllables leave, as `UI_ACT_TEXT_CHAR(code point)`; Backspace edits the preedit locally; any other key commits the preedit and goes as `UI_ACT_TEXT_KEY(usage [| Shift])`, in order. Shift is a one-tap local jamo shift (ㄲ ㄸ ㅃ ㅆ ㅉ ㅒ ㅖ); Ctrl, Alt, Win, AltGr and CapsLock are disabled; the layer never sends a KEY_DOWN or a MODIFIER_TOGGLE. Switching it off commits the preedit; leaving the view, a lost HOST or a switch to `host-ime` discards it (`UI_ACT_TEXT_CANCEL`), so nothing half-typed is sent. The strip renders the preedit in `UI_FONT_HANGUL` |
| `ZH` (ADR-024 FR-746..748, TASK-072, reached through the language key) | a local Simplified Chinese pinyin layer with the same enable and `UI_ACT_TEXT_CANCEL` rules as KO, plus a decoder: the cap stays disabled until `ui_app_set_zh_engine()` installs a complete `ui_kbd_conv_engine_t` (an incomplete table is refused). On the device that is `main/ime_glue` over `ime_pinyin`; in the simulator a canned table. Letters and the separator `'` build the pinyin; the strip shows the fixed hanzi, then the letters still being decoded, and six candidates in `UI_FONT_HANZI` with paging. A tap or digit 1..6 fixes a candidate and the sentence is committed when nothing is left; Space commits candidate 1, Enter the fixed hanzi and the remaining letters as typed, Esc discards, Backspace undoes the last choice or letter, any other key commits candidate 1 first. With nothing composed, `, . ; \` and Shift + `; / 1` give full-width punctuation. KO and ZH exclude each other; the decoder is only called from the LVGL owner |
| `JA` (FR-746, TASK-072, reached through the language key) | the same layer for Japanese: `ui_app_set_ja_engine()` installs its decoder (on the device `main/ime_glue` over `ime_wnn`, the OpenWnn engine). The decoder's optional `preedit` call gives the kana reading, which the strip shows after the fixed text and Enter commits instead of the romaji; `-` is input (the long vowel mark). The strip uses `UI_FONT_KANJI` (Noto Sans JP). With nothing composed, `, . / [ ]` and Shift + `/ 1` give `、。・「」？！`. KO, ZH and JA exclude each other; switching between ZH and JA commits what is on screen first |
| JA flick pad (FR-746) | a `Flick` / `QWERTY` toggle in the JA strip swaps the number and letter rows for a 4 × 4 pad built once at create: あかさ / たなは / まやら / ゛゜小 わ 、。？！, each row ending with a function key (Backspace, Space, Enter, `ABC`). A kana key reads its own press: a tap gives the あ-column kana, a slide of at least `UI_FLICK_MIN` (24 px) left / up / right / down gives the い / う / え / お column, and the key shows the kana under the finger while it is held. Each kana adds its romaji to the same decoder input as QWERTY (`ん` = `nn`, `ー` = `-`), so the reading, candidates and commit rules above are unchanged. `゛゜小` cycles the last kana through its plain, voiced, semi-voiced and small forms (つ → づ → っ → つ); Backspace removes one kana. や's left / right slides give 「 」, the punctuation key 、。？！; these commit what is composed first. Nothing on the pad sends a HID key: the pad only feeds the composition, and with nothing composed Space and Backspace leave as `UI_ACT_TEXT_KEY`. `ABC`, leaving JA or leaving the view puts the QWERTY rows back. The utility row and the bottom row stay |
| HOST not mounted | every cap disabled and silent; a HOST lost while a key is held gets its one owed UP. A centered `Host disconnected` card — the same wording and card the MOUSE view shows — says why, because a field of grey caps is not an explanation (FR-604) |

The key field is deliberately wider than the 800 px reference row set: `01` outranks `reference/` (UX only), and
FR-201 with AT-KEY-01 require the punctuation and `[ ]` that the reference had to drop. Host-layout legends and
geometry come from `kbd_layouts` (ADR-024, FR-741/742): `ui_view_keyboard_set_host_layout()` relabels every cap that
owns a key slot with the base, Shift and AltGr legends of the selected layout and shows or hides the geometry caps
(LSGT, the ISO Enter, the JIS Yen / Ro / Muhenkan / Henkan / Kana, the KR Han/Eng and Hanja). A legend is decoration
only: the usage a cap sends is the one the table gives that slot, never one derived from the legend (FR-743). With no
layout selected, or an id the tables do not have, the view keeps its US ANSI legends.

## SHORTCUTS view (`ui_view_shortcuts`, TASK-070C)

The reference commands grid scaled to the content slot: three columns by two rows of command cards (320 × 160,
the secure-attention tile featured), then the ADR-018 Media row (six 154 × 72 `UiActionKey` tiles) and a footer
with the FR-744 "Switch HOST layout" tile. Thirteen targets, none smaller than `UI_TOUCH_COMFORT`, none
overlapping, all inside the slot.

| Behavior | Rule |
|---|---|
| any tile | one `UI_ACT_SHORTCUT_REQUEST` whose value is a chord id of the compiled allowlist, control id `UI_CTRL_SHORTCUT_BASE + chord` — never a usage or a modifier bitmap (FR-209, 03 §3 rule 2) |
| Media row | the six Consumer chords of ADR-018 / FR-311, in the order play/pause, previous, next, mute, volume −, volume + |
| layout tile | the chord comes from `ui_view_shortcuts_init()` (`KVM_CHORD_LAYOUT_WIN` / `_CTRL`), never hard-coded; an unknown value leaves the tile inert and labelled `Not configured` (FR-744) |
| Alt+F4 | asks a blocking `UiModal` whenever the `confirm_alt_f4` policy callback returns true; Cancel sends nothing, Confirm sends once. The view reads the policy on every tap and never writes it (FR-734, ADR-023) |
| feedback | a toast reads `Requested: <name>` — the UI never claims the HOST executed the chord; a refused submit reads `Not sent: <name>` and is not retried (AGENTS-UI §5). The toast shares the footer band, so the static hint fades out (keeping its box, so the layout tile stays put) while a toast is up |
| input gate | every tile — the six cards, the Media row and the layout tile — is disabled, and a pending confirmation is abandoned, unless the HOST is mounted, maintenance is off and the control owner is not `REMOTE` (FR-006, FR-308) |
| disabled look | the view repaints each tile's own labels and chip, because LVGL hands `LV_STATE_DISABLED` to no child object and a label built with `UI_STYLE_TEXT_*` carries its own ink: without it the dark tile would keep white ink and the five plain tiles near-black ink, both on a pale disabled surface. Disabled, every tile wears one palette — the surface alone is not a state (FR-005, FR-604) |
| on leave | the pending confirmation and the toast are dropped, so neither can outlive the view (AGENTS-UI §6) |

The view has no pad (`pad_obj == NULL`): `ui_router_pad_area()` reports no rectangle while SHORTCUTS is active.
Toasts and the confirmation modal live outside the view root, so the integrator may point them at the shell's
layers with `ui_view_shortcuts_set_layers()`; the default is the view root's screen. The router's overlay layer
is **not** a valid modal parent (the router hides it while its own overlay stack is empty).

## HOST / CLIENT status details (`ui_view_status`, TASK-070C)

Two blocking overlay descriptors (`UI_OVERLAY_HOST_DETAILS`, `UI_OVERLAY_CLIENT_DETAILS`) for the router — the
cards FR-003 opens when a top-bar indicator is tapped. `UI_ACT_OPEN_HOST_DETAILS` and
`UI_ACT_OPEN_CLIENT_DETAILS` map to the two ids; the `Close` button submits `UI_ACT_DISMISS_OVERLAY` and falls
back to `ui_overlay_dismiss()` when the shell installed no local sink.

| Card | Rows |
|---|---|
| Host | USB state (`ui_present_host` + severity glyph), control owner, owner epoch, Caps Lock LED (`On` / `Off` / `Unknown`, FR-206), maintenance (FR-006), error code |
| Client | client session (`ui_present_client`; `Off` in this build), Wi-Fi link as a separate row, plus the sentence that a Wi-Fi link is not a client connection (FR-401, FR-403), and the `Pair a client` action |

The CLIENT card carries the one action of this area: **`Pair a client`** (ADR-022 step 1), built only when the
shell installed `ui_view_status_set_pair_action()`. A tap dismisses the card and reports the request; the card
opens no window and learns no outcome. SEC-106 gates it — remote off (`UI_CLIENT_DISABLED`), a state the store
has not received yet, or no Wi-Fi link leave it disabled — and the line above it says which, because a dimmed
button is not an explanation (FR-005, FR-604).

`ui_app` hands the router the theme's scrim (`UI_COLOR_INK` at `UI_SCRIM_OPA`) through `ui_shell_layout_t`, so a
details card and the FR-734 `UiModal` dim the screen by exactly the same amount; `ui_core` keeps no design token
of its own (07 §2) and falls back to black at `LV_OPA_50` when a shell passes none.

Every row is a live projection: the observers are bound to the card object, so they die with the overlay and the
store may even be deleted first (AT-UIFW-09). A value whose producer has not published reads `Unknown` — the
cards never upgrade a status they were not told (07 §12 item 2). No SSID, address, token or pairing secret is
rendered or taken (07 §7). FR-735 (the active control policy in the CLIENT details) waits for the policy store
of TASK-090 and is not implemented here.

## Pairing and control approval (`ui_remote_view`, TASK-100 UI part)

Two more blocking overlay descriptors for the router — `UI_OVERLAY_PAIRING` and `UI_OVERLAY_APPROVAL` — plus the
toast that reports the outcome of a pairing attempt. The domain adapter publishes `ui_pair_view_t` and
`ui_approval_view_t` through `ui_app_set_pairing()` / `ui_app_set_approval()`; a tap comes back as one typed
`ui_remote_action_t`. Nothing here keeps a clock, a policy or an outcome of its own.

| Card | Contents |
|---|---|
| Pairing, code (FR-722) | `Pair a client`, where to type the code, the code as `ddd ddd` in a well at the title size, the code countdown, the window countdown, `Cancel` |
| Pairing, decision (FR-725) | `<client> wants to pair`, what accepting does and does not do, the reject countdown, `Reject` and `Accept` |
| Control approval (FR-733, PROC-006 step 5) | `<client> requests control of the HOST`, what allowing does, the deny countdown, `Deny` and `Allow` |

| Rule | How |
|---|---|
| one decision at a time | an open pairing window outranks a waiting control request: the window was opened by a tap here and dies at a hard 180 s deadline, while a deferred control request is denied by its own timeout — deferring never grants |
| the UI never decides | a card closes only when the published state says so; an expired countdown reads `0 s` and keeps the card up until the adapter closes the window (AGENTS-UI MUST 5 and 6) |
| no accidental consent | `Accept` / `Reject` and `Allow` / `Deny` are the same size and the same variant, neither is pre-selected, and the hidden pair of buttons is made unclickable, not only invisible (FR-603) |
| the code is a secret | it reaches one label and nothing else: no log, no counter, no snapshot (SEC-105). A code that is not exactly six digits is refused and the card says it has none |
| an untrusted name | the client name arrives over the network, so it is copied bounded and printable-ASCII only (`?` for anything else) and an empty name reads `Unknown client` (FR-724) |
| outcome toast | one per `result_seq`: paired / rejected / failed / locked out / expired. A failed or locked-out attempt names nobody — that name was never authenticated (SEC-104) |
| input below | both cards are router overlays with `blocking`, so the touch is cancelled and the HID release requested before the card appears, the pad is gone while it is up and comes back on dismiss. A pending FR-734 confirmation — which has its own scrim outside the overlay stack — is abandoned, not left on top |

The code is rendered at `UI_FONT_TITLE` (Montserrat 24, the largest size compiled into the firmware) with extra
letter spacing, which is as large as `ddd ddd` can be read here until another font size is enabled.

## Settings (`ui_view_settings`, TASK-070C)

Two more blocking overlay descriptors — `UI_OVERLAY_SETTINGS` and `UI_OVERLAY_SETTINGS_CONFIRM` — reached from
the gear in the top bar's trailing slot, which submits `UI_ACT_OPEN_SETTINGS`. **Not a fourth primary view:**
the router rejects `UI_VIEW_SETTINGS` as a primary view and 07 §5 keeps "at most three main views", so Settings
is the blocking card the overlay stack was built for. The two ids nest exactly `UI_OVERLAY_STACK_MAX` deep — the
card plus one confirmation — so a confirmation can never end up behind another card.

The card is a section list (220 px) beside one pane per section. Every pane and all eight client rows are built
once at open and hidden when unused: a section change, a publish and a forgotten client all run through the same
`refresh()`, so nothing is created or deleted while an event is being dispatched and a section change allocates
nothing.

| Section | Contents |
|---|---|
| Remote | `remote.enabled` (FR-506), `pair.require_accept` (FR-732), `control.approval` as Always / Trusted / Auto (FR-733) with the sentence the active value means, and `Pair a client` |
| Paired clients | the FR-726 register: name, the FR-733 `trusted` flag, `Forget` — up to `UI_SETTINGS_CLIENTS_MAX` rows |
| Shortcuts | `shortcuts.confirm_alt_f4` (FR-734) |
| Input | `keepawake.enabled` and the 10–300 s interval stepper (FR-711, ADR-020) |
| Keyboard | `Input languages` — English, always on and without a control, then a toggle each for Korean, Chinese (pinyin) and Japanese (ADR-037 FR-752); `Host layout` (FR-741), which opens the ADR-039 picker inside the pane itself; the Unicode delivery mode (FR-747), read-only. The pane is taller than the card body, so it scrolls |
| About | firmware version, the three ADR-025 license lines and the scrollable third-party notices (FR-751) |

| Rule | How |
|---|---|
| the UI is not the source of truth | every value comes from `ui_settings_view_t`, which the domain adapter publishes through `ui_app_set_settings()`; before the first publish (`seq == 0`) every value reads `Unknown` and every control is disabled (07 §12 item 2) |
| no optimistic state | a tap reports one typed change through `hooks.settings_change` and **the control does not move**. There is no code path from a tap to a value on screen: only `ui_view_settings_set()` renders. A refused or clamped write therefore shows up as "the switch did not move" (AGENTS-UI MUST 5 and 6) |
| relaxing needs a second tap | `pair.require_accept` to OFF, and any `control.approval` step **down** the strictness scale (`always` > `trusted` > `auto`, the `kvm_control_approval_t` order), open the confirmation card; nothing is written until `Change it`. A step up the scale is reported straight away. Marking a client trusted is confirmed too; clearing the flag is not |
| Forget is confirmed | the question names the client, and `Keep as it is` — or any teardown of the stack — writes nothing (fail closed) |
| hard invariants have no control | the ADR-023 invariants (TLS + pinning, lease/epoch, `RELEASE_ALL`, the 180 s pairing window, LOCAL > REMOTE, remote off on first boot, no remote settings change, eFuses) are not in `ui_setting_id_t`, so this screen cannot express them |
| physical presence only | SEC-107: these controls exist only here; the CLIENT has no `settings.*` command and the view takes no network input but the client names |
| the interval clamps | the stepper reports 10–300 s in 10 s steps and reports **nothing** at either end, so no out-of-range value is ever handed to `config_store` |
| pairing | `Pair a client` dismisses the card first — the pairing prompt is a blocking overlay of its own — and then reports `UI_REMOTE_ACT_PAIR_OPEN`. It is disabled while remote is off (SEC-106) and the line below says why |
| an untrusted name | a paired client's name arrives over the network, so it is copied bounded and printable-ASCII only before it reaches a label or a confirmation question (SEC-105) |
| the layout database | a build without the generated tables, or a snapshot whose `layout_db_available` is false, shows `Host layout` as stored with a note that says so and offers no control. No layout id is invented (FR-741, FR-747) |
| the picker | drawn inside the pane, not as a third overlay: the stack is two deep and the confirmation card owns the second level. A pick is reported as the table index of the entry; nothing on screen moves until the domain republishes the record (AGENTS-UI MUST 5) |

### The `Host layout` picker (ADR-039)

581 layouts in 96 groups, so the control opens on a **home** screen rather than on a list. Four screens share one
`Back` button, which walks them back one step at a time and only then closes the picker:

| Screen | What it holds |
|---|---|
| `HOME` | the layout in force, named, with a preview of its home row (`AC01..AC11`) so QWERTY, QWERTZ, AZERTY, Dvorak and Cyrillic tell themselves apart; up to 8 favorite tiles; up to 5 recent chips; `Search` and `All layouts` |
| `LIST` | the 96 layout groups, with the A–Z index strip above the rows; a group with variants opens the next screen, a group of one applies straight away |
| `VARIANTS` | the variants of one group |
| `SEARCH` | the query, the picker's **own** on-screen keyboard, and the matches |

| Rule | How |
|---|---|
| the two lists are the record's | the favorites and the recents come from the snapshot (`host_fav`, `host_recent`). A star on a row, and a long press on a tile, report `UI_SETTING_HOST_FAV_TOGGLE` with the table index; the order, the dedup and the 8 and 5 limits are `kvm_config_pin_toggle()` / `kvm_config_recent_push()` in `config_store`. The screen draws the answer, never its own guess (AGENTS-UI MUST 5) |
| eight is eight | a ninth favorite is refused with one line under the screen (`8 favorites is the limit - unpin one first.`), because the eight were pinned by hand and the device does not throw one of them away |
| recents never repeat a favorite | a recent that is already pinned above is left out of the chips, so the two rows never show the same layout twice |
| an id the tables lost | a favorite or recent the generated tables no longer resolve is skipped in the drawing and kept in the record — a regenerated xkeyboard-config renumbers entries but keeps ids |
| its own keyboard | the search field cannot borrow the KEYBOARD view's keys: those emit USB HID (AGENTS-UI MUST 4). It has 32 keys of its own — letters, `-`, `_`, `(`, `)`, space and `1` — which is everything an xkb id and an English layout name contain |
| matching | case-insensitive, over the id and the name, ranked id-prefix, then name-prefix, then anywhere; the table is walked once per rank, so no result array exists to overflow |
| the page is as long as the pane | the rows are measured before each draw and only those that fit are used (the search keyboard takes most of the height). A row past its parent's edge is clipped out of the drawing *and* out of the touch search, so paging, the page counter and the index jump all step by the number that fits |
| the index strip | nine buckets (`A-C` … `Y-Z`) across the top of the rows, each at least `UI_TOUCH_MIN`; an empty bucket is disabled rather than hidden, so nothing moves under the finger |
| nothing is allocated | every widget — six rows with their stars, nine buckets, 32 keys, 8 tiles, 5 chips — is built once with the pane and only relabelled, hidden or shown. Paging through all 581 entries adds no object (AT-UIFW-09/10) |

### The quick switch in the KEYBOARD view (ADR-039 section 8)

The layout strip over the field (`ui_view_keyboard_layout_strip()`) is a quiet button at least `UI_TOUCH_MIN`
tall holding `LV_SYMBOL_KEYBOARD`, the short id of the layout in force (`fr(oss)` reads `FR`) and
`LV_SYMBOL_DOWN`. A **tap or a long press** opens `UI_OVERLAY_KBD_LAYOUT`, a blocking card of tiles: the
favorites in their stored order, then the recents that are not pinned. Neither gesture changes a layout by
itself. The FR-742 sentence that used to sit there as text (and ran off the right edge of the panel) is still
`ui_view_keyboard_hint_text()` and is shown in full by the card's note.

| Rule | How |
|---|---|
| it is not the language key | the ADR-037 key changes what the device composes; this one changes which layout the device believes the HOST is set to. Two questions, two controls |
| one write path | a tap reports `UI_SETTING_HOST_LAYOUT` with the table index through `hooks.settings_change`, exactly as the Settings picker does; `ui_app` installs the sink only when that hook exists |
| nothing is applied here | the field is relabeled when the record comes back, never on the tap (AGENTS-UI MUST 5). The layout in force is marked and stays tappable |
| nothing to offer, nothing offered | no sink, an empty list, or ids this build does not ship: the long press opens no card and is counted (`c.quick_refused`) |
| no stale tiles | a list that changes while the card is up dismisses it rather than redraw it under the finger |
| blocking | the card owns the overlay level, so a tap meant for a tile can never reach a cap underneath it |

**About is generated (ADR-025 FR-751).** `tools/gen_third_party_notices.py` turns `THIRD_PARTY_NOTICES.md` into
`ui_notices_generated.{c,h}` (`ui_notices_license()`, `ui_notices_third_party()`) at CMake configure time, so
the screen cannot drift from the repository record and no notice is retyped in C. `notices.cmake` holds the one
function the three builds that compile `ui_views` call — the ESP-IDF component build, `test/ui_views` and the
simulator — and a change to the Markdown file triggers a reconfigure through `CMAKE_CONFIGURE_DEPENDS`. The
renderer folds the record's typographic characters to ASCII, because the About pane uses the built-in
Montserrat subsets; the output carries the ADR-025 three-line header and no timestamp, path or host name
(reproducible build, TECH-083).

FR-735 (the active control policy in the CLIENT status details) still waits for TASK-090 and is not implemented.

Governing documents: `AGENTS.md`, `AGENTS-UI.md`.
