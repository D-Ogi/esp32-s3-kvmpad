# ADR-019 — Typing text in (paste-as-keystrokes) on the CLIENT side
Status: ACCEPTED (scope: remote-enabled, after M7)
Date: 2026-11-01
Approver: project owner (2026-11-01)

## Context

- Prior art: pasting the clipboard as a sequence of keystrokes, including Polish characters through AltGr (Polish Programmer layout).
- Protocol v1 (03 §6.3) already allows an operator with a lease to send `input/keyboard.key` (`usage_id`, `down`).
- FR-309: the CLIENT must not transmit arbitrary HID scripts. FR-202: characters follow from the HOST layout, HID does not carry Unicode.
- On the device itself there is no source of text (no clipboard) — we already have the local on-screen keyboard.

## Options

- **A.** A new `type_text` command in the firmware → breaks FR-309, a text parser on the MCU, a new attack surface.
- **B.** A function in the CLIENT application: text → a sequence of existing `keyboard.key` DOWN/UP. Firmware unchanged.
- **C.** Local "snippets" stored on the device → no convenient source of text, a risk of storing passwords in NVS.

## Decision

**B.** A and C — REJECTED.

- **FR-701 MUST:** "Type text" is a CLIENT function; it sends only `keyboard.key` from protocol v1. No new firmware commands.
- **FR-702 MUST:** character → (usage, modifiers) mapping from an explicit table per HOST layout (start: `US`, `PL-Programmer`).
  The user selects the layout matching the HOST. Characters outside the table are skipped, and the CLIENT shows their count (not "silently").
- **FR-703 MUST:** interruption at any moment: Esc in the CLIENT, loss of connection or a local takeover on the device
  (ADR-011). On an owner/lease change the device performs `RELEASE_ALL` anyway.
- **FR-704 MUST:** the protocol limit is **120 input msg/s** (03 §6.5; excess = throttle, and on a flood, revoke).
  A character without a modifier = 2 messages (DOWN/UP), a character with Shift or AltGr = 4. By default the CLIENT sends ≤ 60 msg/s
  (≈ 15–30 characters/s), never more than 100 msg/s; a length limit (4096 characters by default) and confirmation for long texts.
- **FR-705 MUST:** the CLIENT does not log or store the text being typed in (it is often passwords).

## Consequences

- Works only with a secure CLIENT (ADR-002, M7). The `local-only` release does not have this function.
- A wrong layout choice gives wrong characters on the HOST — this is a HID limitation, not a bug; the CLIENT UI shows the active layout.

## Tests

- AT-EXT-10 (unit, CLIENT): the US and PL-Programmer tables: every character → the expected usage + modifiers; characters outside the table are counted.
- AT-EXT-11 (HIL): pasting a test ASCII text + `ąćęłńóśźż` on a HOST with PL-Programmer; a 1:1 match.
- AT-EXT-12 (HIL): interruption halfway through (Esc / a local tap / a network break) → no stuck keys, `RELEASE_ALL` in the log.
