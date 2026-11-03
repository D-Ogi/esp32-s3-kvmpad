# ADR-037 — One language key on the KEYBOARD view; the active input languages are chosen in Settings

Status: ACCEPTED (owner, 2026-11-02, direction: "tak, przerób" — one language key as on Android, active languages
chosen in Settings)
Date: 2026-11-02
Approver: project owner
Amends: ADR-024 (keyboard layouts and on-device IME), FR-746 / FR-747 presentation; no requirement is withdrawn

## Context

ADR-024 put on-device composition for Korean, Simplified Chinese and Japanese into the main release (FR-746), and
TASK-072 implemented it as three extra keyboard layers. Each layer had its own cap in the utility row of the
KEYBOARD view: `KO`, `ZH`, `JA` next to `Esc`, `F1-F12` and the navigation block.

Three caps cost three of the nine slots of that row and are dead weight on a device whose owner types in one
language: they are always visible, always occupy touch area, and say nothing about which of them is usable right
now (a layer needs its decoder installed and a Unicode delivery mode other than `host-ime`, FR-747). Phones solve
the same problem with one language key and a list of active languages in the system settings.

## Decision

1. **One language key.** The `KO`, `ZH` and `JA` caps are replaced by a single cap in the utility row. It carries
   the code of the current language (`EN`, `KO`, `ZH`, `JA`) next to the keyboard symbol of the theme, and the
   freed width is given back to the row. No font asset is added: the theme fonts carry no globe glyph, so the cap
   is a symbol plus a text label, not a pictogram.
2. **The active languages are a setting.** `Settings → Keyboard → Input languages` lists English, which is always
   on and has no control, and one toggle each for Korean, Chinese (pinyin) and Japanese. The three toggles are
   stored in the `config_store` record as `kbd.lang_ko`, `kbd.lang_zh`, `kbd.lang_ja`, each `0..1` with default
   `0`: a factory-fresh device and every record migrated from an earlier schema start English-only.
3. **A tap cycles.** A tap on the key moves to the next language that is both turned on in Settings and usable
   now, in the order `EN → KO → ZH → JA → EN`. A language is not usable while the HOST is not mounted, while the
   delivery mode is `host-ime` (the device cannot deliver the composed text itself, FR-747), or while its decoder
   is not installed. With English alone the tap shows a toast that names the place to add languages, and changes
   nothing.
4. **A long press picks.** A long press opens a blocking picker through the router overlay mechanism, like the
   other cards of the UI. It lists the languages that are turned on, marks the current one and disables the
   unusable ones with the reason. A tap on a row switches and closes the card. The long press is not also a tap:
   the cycle runs on the short click, which LVGL suppresses once the long press has fired.
5. **The composition contract is unchanged.** Switching through the key is the same operation TASK-072 already
   performs for a layer change: a held key is released first, the preedit is committed when leaving a composing
   layer, `TEXT_CANCEL` is sent when entering one, and the composition is discarded on HOST loss, on a view
   change and when the delivery mode becomes `host-ime`. The current language is derived from the composer state
   rather than stored beside it, so the cap and the composition cannot disagree. A language turned off in
   Settings while it is active falls back to English under the same rule.
6. **Nothing new reaches the HOST.** The key produces no HID of its own; the switch travels as the existing typed
   actions through the arbiter (FR-748, AGENTS-UI MUST 4).

## Consequences

- The `config_store` record moves to schema 3 (payload 44 bytes). A schema-2 record is accepted and migrated with
  the three languages off; the alignment bytes before the pairing table are named and zeroed so a validated
  record stays byte-for-byte canonical.
- The utility row keeps nine caps; the smallest cap stays above the 44 px touch floor (measured 64×48).
- The Keyboard pane of Settings grows past the card body and scrolls, like the Paired clients pane.
- FR-746 and FR-747 keep their meaning; FR-752 records the list and the key. No test id is withdrawn: the
  AT-EXT-26/27 runs now reach the layers through the language key.
- A device whose owner types only English sees one cap more than a plain keyboard and no layer it cannot use.
