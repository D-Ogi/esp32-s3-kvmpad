# ADR-021 — Native CLIENT for Windows (resolution of ADR-002 → variant A)
Status: ACCEPTED
Date: 2026-11-01
Approver: project owner — approved 2026-11-01. ADR-002 is protected (spec 06 §16); this approval resolves it.

## Context

- ADR-002 (spec 06 §3) has status PENDING-PRODUCT; variant **A, "native CLIENT with certificate pinning"**, is marked as
  preferred if we control the application. A browser CLIENT requires a gateway/CA (02 §8.1, risk R-07 = BLOCKER).
- Prior art: a Windows application captures the keyboard/mouse (`WH_KEYBOARD_LL`, `WH_MOUSE_LL`, Raw Input without acceleration)
  and switches mode with the Scroll Lock key. There, however, the transport is plaintext UDP — we do not adopt that.
- Naming: in the prior art, "Host PC" is the computer with the application, i.e. our **CLIENT**. Here, **HOST** is the computer connected to KVMpad over USB.

## Decision

- ADR-002 → **variant A**: a Windows desktop application as the first and only CLIENT in v1; browser CLIENT outside v1.
- Unchanged: the safe default **Remote OFF** (ADR-002) and the 05 §4 gate — no remote control before working
  `release_all`, `epoch` and USB HIL.
- Transport and trust unchanged relative to the spec: WSS, protocol v1 (03 §6), device certificate pinning established during
  physical pairing (procedure: **ADR-022** — mDNS + 6-digit code + PAKE), lease + epoch, local priority.
- Features: keyboard/mouse capture with a toggle (Scroll Lock by default, configurable), Raw Input for the mouse,
  "Type text" (ADR-019), connection/owner status.
- Application security: hooks are active only in capture mode; no key logging; disabling capture sends a release of all pressed
  keys. The device performs `RELEASE_ALL` anyway after the lease expires.
- Repo: `clients/windows/` is created in **TASK-105** (after TASK-100). Language/stack to be chosen in that task, with its own ADR.

## Consequences

- The browser certificate problem (R-07) goes away for v1.
- No macOS/Linux/mobile client in v1.
- An application with global hooks may be flagged by AV; code signing of the application is part of the release (TASK-130).

## Tests

AT-SEC-* and AT-CONN-* from spec 04 for this CLIENT; AT-EXT-12; additionally: the capture toggle with a key pressed → no stuck keys.

## Migration

None — no CLIENT implementation exists.
