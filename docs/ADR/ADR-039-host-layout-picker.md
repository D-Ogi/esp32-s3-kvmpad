# ADR-039 — Choosing one of 581 HOST layouts: current card, favorites, recents, search, indexed list

Status: PROPOSED
Date: 2026-11-03
Approver: project owner
Amends: ADR-024 (keyboard layouts from xkeyboard-config) and the FR-741 presentation; no requirement is withdrawn.
Depends on: ADR-037 (the input-language key, which this decision deliberately keeps separate)

## Context

ADR-024 put the whole xkeyboard-config catalog on the device: the generated tables hold **581 entries**
(`layout(variant)`) in **96 layout groups**, sorted by id in byte order, so every variant follows its base layout.
FR-741 says the user chooses one of them as the HOST layout and FR-742 says the KEYBOARD view then draws that
layout's legends and geometry.

The first implementation of FR-741 was a two-level list inside the `Settings → Keyboard` pane: six rows a page,
Prev / Next, layout level then variant level. It is correct and cheap, and it is also 16 pages of paging to reach
`us`, with no way to come back to a layout the user already uses. Three things are missing for a device whose
owner realistically switches between two or three layouts and visits the rest once:

- no way to get to a layout in one or two taps once it is known;
- no way to search, although the user knows the word ("german", "polish") or the id (`de`, `fr(azerty)`);
- no way to see, before committing, that the layout about to be chosen is the one meant — the id and the English
  name are not enough for someone who recognizes a layout by its key caps.

The screen is 1024x600 and the Settings card body gives the pane roughly 631x443 px. The overlay stack is two
deep by ADR-023 and the second level belongs to the confirmation card, so the picker may not be a card of its own
while Settings is open. AT-UIFW-09 and AT-UIFW-10 forbid a widget count or a live heap that grows with use.

## Options

**A. Keep the paged two-level list.** Cheapest, already written, already tested. Rejected: it does not solve any
of the three problems above, and the owner asked for a picker that is comfortable with 581 entries.

**B. One long scrolling list of all 581 entries with a scrollbar.** One screen, no modes. Rejected: 581 rows is
either 581 objects (AT-UIFW-09) or a virtualized list, and a 443 px viewport over 581 rows means a 0.8 px scroll
thumb — a touch target nobody can aim. It also still has no favorites and no search.

**C. Current card + favorites + recents + search + indexed two-level list, as four screens inside the pane.**
Chosen. Each screen fits the pane, the widgets are built once with the pane and only relabelled, and the common
case (a layout the user already uses) is one or two taps from anywhere.

**D. As C, but with the picker as its own full-screen view outside Settings.** Rejected: a HOST layout is a
setting, and FR-741 puts it in Settings; a fifth top-level view would also need a tab in the mode switcher, which
FR-004 fixes at three.

## Decision

### 1. Four screens inside the Keyboard pane

The picker replaces the Keyboard pane's rows while it is open (the rows are hidden, not destroyed) and shows one
of four screens. `Back` always goes one step up: `VARIANTS -> LIST`, `LIST | SEARCH -> HOME`, `HOME -> the pane`.

| Screen | Content |
|---|---|
| `HOME` | the current-layout card, the favorites tiles, the recents row, and the two buttons `Search` and `All layouts` |
| `SEARCH` | the query field, the on-screen keyboard and the matching rows |
| `LIST` | the 96 layout groups, six rows a page, with the index strip |
| `VARIANTS` | the variants of one group, six rows a page |

### 2. Current-layout card

The top of `HOME` names the layout in force (`German — de(neo)`) and, under it, a **preview of one letter row**
of that layout: the base legends of the home-row slots `AC01..AC11`, drawn as inert chips in the keyboard font.
It is the cheapest honest answer to "is this the layout I mean": the row that differs between QWERTY, QWERTZ,
AZERTY, Dvorak and Cyrillic. The preview is decoration and never emits anything.

### 3. Favorites — up to 8, pinned by hand

- Up to **8** layouts are pinned as large tiles on `HOME`, two rows of four, each tile naming the layout and its
  id. A tap applies the layout; a **long press** unpins it.
- Every row of `LIST`, `VARIANTS` and `SEARCH` carries a **star** on its right edge, which pins or unpins that
  entry. The star is a control of its own, so a tap on the row still picks the layout.
- Pinning a ninth layout is refused, and the screen says so instead of silently dropping the oldest: a favorite
  is something the user put there by hand, so the device does not throw one away on its own.
- Default: `us` is pinned on a factory-fresh device. The first layout the user picks is pinned as well, so the
  device that has been used once already has the two layouts its owner switches between.

### 4. Recents — the last 5, newest first

Every applied layout goes to the head of the recents list, whichever screen applied it. An entry already in the
list moves to the head instead of being duplicated, the list is truncated to **5**, and an entry that is also a
favorite is left out of the display: the tiles already show it, and repeating it wastes the row. Recents are
empty on a factory-fresh device.

### 5. Search

The query field has **its own on-screen keyboard** (letters, digits, `(`, `)`, `-`, `_`, space, backspace,
clear): the KEYBOARD view's keyboard emits USB HID to the HOST and must never be reused to type into the device
(AGENTS-UI MUST 4). The query matches **case-insensitively** against both the English name and the id, as a
substring anywhere: `pol` finds `pl Polish`, `german` finds every `de*`, `de` finds `de` and also `Swedish
(Dvorak)` through its id; entries whose **id starts with** the query sort first, then entries whose **name
starts with** it, then the rest, each group in table order. The result list uses the same rows as `LIST`.

### 6. Rows are recycled, never 581 objects

`LIST`, `VARIANTS` and `SEARCH` share **six row widgets**. A page is drawn by relabelling those six rows from the
table; paging, entering a group and searching change text only. Nothing is allocated per page and the widget
count does not depend on the number of layouts (AT-UIFW-09, AT-UIFW-10).

A page is also **as long as the pane allows**, which is not always six: the search screen gives most of its
height to its own keyboard, and the Clients register can squeeze the pane. Before each draw the view measures
the row column and uses only the rows that fit, and the paging, the page counter and the index jump all step by
that number. This is not cosmetic. A row laid out past its parent's edge is clipped out of the drawing *and*
out of the touch search — LVGL walks the tree and never enters a parent the point lies outside of — so an
overflowing row is an invisible, dead control. The host test probe therefore checks every clickable control of
every picker screen for containment in each of its ancestors, not only in the card.

### 7. Index strip — nine buckets, not 26 letters

`LIST` carries an index strip across the top of the rows for jumping into the alphabet. It has **nine** buttons
(`A-C D-F G-I J-L M-O P-R S-U V-X Y-Z`), not 26: a 26-row strip over a 443 px pane gives each letter 17 px, far
under the `UI_TOUCH_MIN` of 44 px that AT-UIFW-01 requires. A bucket with no layout is disabled rather than
hidden, so the strip does not move under the finger.

The strip lies **horizontally** above the rows, not down the side: nine buttons at the 44 px floor are 396 px
tall, more than the pane has, and the ones past the edge would be unreachable (section 6). Across the top the
same nine share the pane's width, which is the larger dimension.

### 8. Quick switch from the KEYBOARD view

The layout strip of the KEYBOARD view is a **control, not a line of text**: a keyboard icon, the short id of
the layout in force (the base of the xkb id in upper case — `fr(oss)` reads `FR`) and a chevron, in a quiet
button at least `UI_TOUCH_MIN` tall at the end of the utility row. Both a **tap** and a **long press** open a
small blocking overlay listing the favorites and the recents; one tap on a tile applies a layout and closes
it. The chevron promises a list, so the ordinary tap has to deliver one; neither gesture can change a layout
by itself, so nothing moves by accident while typing.

The strip replaces the FR-742 sentence that used to sit there as plain text and ran off the right edge of the
panel. The sentence remains the view's own `hint_text` (and `ui_view_keyboard_hint_text()`) and is read out in
full by the card's note, where there is room for it; the strip is a bounded box whose children stay inside it.
`LV_SYMBOL_KEYBOARD` and `LV_SYMBOL_DOWN` are in every built-in Montserrat subset the theme ships, so unlike
the star of section 3 no glyph had to be drawn.

This is **not** the ADR-037 language key and is never merged with it. The language key changes what the device
composes locally (English / Korean / Chinese / Japanese, FR-746); this overlay changes which layout the device
believes the **HOST** has been set to, which only re-labels the caps and re-maps the one-keystroke characters.
They are different questions with different answers, and a single control that did both would make it impossible
to say which of the two a user meant.

Only the KEYBOARD view offers the quick switch, and only while no other overlay is up: the stack is two deep, and
with Settings closed the first level is free, so the overlay never competes with a confirmation card.

The card offers the favorites in their stored order, then the recents that are not already pinned, so no layout
appears twice; the layout in force is marked but stays tappable. An id the generated tables no longer contain
gets no tile and stays in the record. The strip itself is a full-height touch target (`UI_TOUCH_MIN`), and when
there is nothing to offer — no list, or a build without the tables — or no settings sink, the long press opens
nothing rather than a card that could not change anything.

A tap reports `UI_SETTING_HOST_LAYOUT` with the table index through the same `hooks.settings_change` the
Settings picker uses, so there is one write path for one record field; the view applies nothing itself and the
field is relabeled only when the record comes back (AGENTS-UI MUST 5). A list that changes while the card is up
takes the card down instead of leaving stale tiles.

### 9. Storage and the authority boundary

The favorites and the recents live in the `config_store` record as **xkeyboard-config ids**, not table indices: a
regenerated table (a new xkeyboard-config release) renumbers entries but keeps the ids. Schema 3 becomes
**schema 4**, adding `host_fav[8][16]` with its count and `host_recent[5][16]` with its count. Records from older
schemas migrate with the defaults of section 3 and 4. An id in the record that the tables no longer contain is
kept in the record (it may come back with the next table) but is skipped when the lists are drawn; if the
**current** layout id cannot be resolved, the KEYBOARD view falls back to US ANSI legends and the record stays
valid and untouched.

The UI reports two things and stores nothing: `UI_SETTING_HOST_LAYOUT` with the table index of the chosen entry,
and `UI_SETTING_HOST_FAV_TOGGLE` with the table index to pin or unpin. The UI redraws only what comes back in the
next snapshot (AGENTS-UI MUST 5); nothing on screen moves on the tap itself.

The list algebra — the order, the removal of a duplicate, the move to the head, the 8 and 5 limits — is
`kvm_config_pin_toggle()` and `kvm_config_recent_push()` in `config_store`, next to the record they write and
within reach of the host tests. `main/ui_shell.c` maps the two reported settings onto them through
`app_config_pin_layout()` and `app_config_note_layout_use()`, and applying a layout also pushes it onto the
recents. Two rules are the record's, not the screen's: the favorites never go empty (the last one cannot be
unpinned), and a ninth favorite is refused rather than pushing one of the eight out — the eight were pinned by
hand, and the device does not discard the user's own choices. The picker says so in one line instead.

## Consequences

- **Memory.** The record grows by 210 bytes (schema 4) and the serialization buffer with it. The picker adds
  about a hundred widgets to the Settings card, all built once with the pane and freed with it; no screen, page,
  search or scroll allocates.
- **Flash.** No new asset. The preview and the search keyboard use the fonts TECH-078 already ships.
- **Security.** Unchanged. The picker emits no HID: it reports typed intents only, the search keyboard is a
  device-local text field, and the quick-switch overlay orders the same `RELEASE_ALL` on a layout change that
  Settings does.
- **UX.** The common case is one tap (a favorite tile), two from the KEYBOARD view (long press, tap). The worst
  case — an unknown layout, searched by name — is the field plus one tap.
- **Risk.** The Keyboard pane now carries four screens' worth of widgets; a regression there costs the whole
  Settings section. Mitigated by the golden images of all four screens and by AT-UIFW-10 over 200 open/close
  cycles.

## Tests

- `AT-UIFW-01` on every screen (no target under 44 px, nothing off the panel, no overlap), `AT-UIFW-09` and
  `AT-UIFW-10` (constant widget count and live heap across paging, searching and 200 open/close cycles).
- Host tests: favorite ordering, the 8 limit and its refusal, unpinning from a tile and from a row star, recents
  ordering, the move-to-head on a repeat, the 5 limit, a favorite left out of the recents display, search
  matching by name and by id with the documented sort, the index strip jumping into the right page, an id the
  tables no longer have, and the schema-3 to schema-4 migration.
- Host tests of the quick switch: a tap and a long press both open it, the strip's short id and name, the
  strip and its children inside the right edge of the panel, the order and the absence
  of a duplicate, one tap reports the right table index and closes without applying anything, Close and a
  router dismiss change nothing, no sink / no list / an unshipped id offer nothing, and `AT-UIFW-01` on the
  card (44 px tiles, inside the card and the panel, every tile's center inside each of its ancestors).
- Simulator goldens: `HOME` with favorites and recents filled, `SEARCH` with results, `VARIANTS`, and
  `keyboard_quick_layout` — the quick switch held open over the KEYBOARD view.
- `NOT_RUN_HW` until the owner picks a layout on the panel and types into a prepared HOST.

## Migration

No user-visible migration. A device running schema 3 keeps its `host_layout`; the two new lists appear with `us`
pinned and no recents. The paged two-level list of the first FR-741 implementation survives as the `LIST` and
`VARIANTS` screens of this one, so no behavior is withdrawn.
