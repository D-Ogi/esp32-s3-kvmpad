# ADR-022 — CLIENT pairing: 6-digit code + PAKE, nothing else typed in
Status: ACCEPTED (owner requirement), PENDING-IMPL — final choice of the PAKE algorithm after interop evidence in TASK-100
Date: 2026-11-01
Approver: project owner (requirement of 2026-11-01: "pairing must be easy — no more than 6 typed-in characters")

## Context

- Spec: PROC-006 (physical opening on the DEVICE, a single short-lived code, 3 failed attempts → refusal), TECH-040/041/042,
  03 §6.1 (WSS with pinning; a token per paired client), 03 §7 (`ERR_PAIRING_EXPIRED`), AT-SEC-06, FR-603 (approve with one
  button), ADR-011 (LOCAL can always cancel pairing), ADR-021 (native Windows CLIENT, pinning after an out-of-band pair).
- Problem: a certificate fingerprint has 64 hex characters, an IP address up to 15 characters. The user cannot type in either of them.
  Trust (the DEVICE certificate) and identity (the client token) must therefore be conveyed over a channel authenticated by a **short** code,
  resistant to MITM on the local network.
- Owner requirement (2026-11-01): the pairing window must not be long — **at most 3 minutes**.
- Naming: DEVICE = KVMpad, CLIENT = the Windows application (ADR-021), HOST = the computer on USB.

## Options

| | Variant | Assessment |
|---|---|---|
| A | Typing in the fingerprint / IP | REJECTED — breaks the requirement |
| B | TOFU (trust the first certificate), without a code | REJECTED — MITM on the LAN at the moment of pairing, no evidence of physical presence |
| **C** | **6-digit code as the PAKE password + channel binding to the TLS certificate** | **CHOSEN** — 6 digits, one attempt per protocol run, a standard model (Bluetooth SSP Passkey) |
| D | Comparison of a 6-digit SAS on both screens (0 typing in) | deferred — two screens to compare and confirmation on both sides; may complement C after v1 |
| E | QR code on the DEVICE screen scanned with a camera | outside v1 — a PC rarely has a camera pointed at the device; an option for a mobile client |

## Decision — flow (elaboration of PROC-006)

1. **DEVICE:** `Settings → Remote → Pair client` (1 tap). The DEVICE generates a code, opens the pairing window and shows:
   the device name (e.g. `KVMpad-7F3A`), the code `482 913`, a countdown.
2. **CLIENT:** discovers devices via mDNS/DNS-SD and shows a list of names. The user clicks the name visible on the DEVICE screen. *0 characters.*
3. **CLIENT:** asks for the code. The user enters **6 digits**. *This is the only thing typed in during the whole process.*
4. **CLIENT ↔ DEVICE:** TLS to the pairing endpoint (the DEVICE certificate is not yet trusted — pairing-only mode, SEC-103),
   with PAKE inside, using the code as the password. The PAKE transcript contains the hash of the DEVICE TLS certificate and the client identifier (SEC-102).
5. **DEVICE:** "Pair with *<client name>*?" — `Accept` / `Reject` (1 tap; 30 s → reject).
6. **After Accept:** the DEVICE sends a token (32 random bytes) encrypted with the key from PAKE. The CLIENT stores the certificate fingerprint
   (pinning) and the token; the DEVICE stores the client name + SHA-256 of the token. The window closes, the code is invalidated.
7. Control still requires `control.request` and LOCAL approval (PROC-006 step 5). **Pairing ≠ control.**

In total for the user: 2 taps on the DEVICE, 2–3 clicks in the CLIENT, 6 digits.

## Requirements

### Functional

- **FR-721 MUST:** in the normal path the user types in **exactly 6 digits and nothing more** — no IP, fingerprint,
  password or name. The rest is clicks/taps.
- **FR-722 MUST:** code = 6 decimal digits from `esp_random()`, the full range `000000–999999` (without excluding "ugly"
  patterns — that lowers entropy), displayed as `ddd ddd` in a large font; valid for **120 s**; single-use.
  **The whole pairing window** — from the tap on *Pair client* to closing the endpoint, including the Accept screen — lasts **≤ 180 s**.
  This is a hard limit from the owner requirement: a compile-time constant, not a setting; by default 120 s for the code + max. 30 s for Accept.
- **FR-723 MUST:** discovery via mDNS/DNS-SD `_kvmpad._tcp.local`, TXT record: `name`, `v=1`, `pair=1|0`
  (`pair=1` only within an open window). The CLIENT shows a list of names. A manual `IP:port` only as an emergency fallback
  (mDNS blocked on the network), marked in the UI as advanced — a path outside FR-721.
- **FR-724 MUST:** client name = the Windows host name (nothing to type in); optionally editable.
- **FR-725 MUST:** after a successful PAKE the DEVICE screen shows the client name and `Accept`/`Reject` (FR-603); LOCAL can cancel
  at any moment (ADR-011); 30 s without a decision → reject. **Configurable (ADR-023, `pair.require_accept`, ON by default):**
  with OFF, pairing completes automatically after a correct code, and the DEVICE shows a toast for 10 s with the client name and `Remove`.
- **FR-726 MUST:** `Settings → Remote → Paired clients`: a list (name, date), `Remove` per client, `Remove all`;
  factory reset removes all of them (PROC-010, AT-FAULT-10). A removed client loses its session immediately (revoke + `RELEASE_ALL`).
- **FR-727 SHOULD:** from opening the screen to "Paired" ≤ 60 s for a user who sees both screens.

### Security

- **SEC-101 MUST — PAKE:** candidate **EC-JPAKE on P-256** (mbedTLS `MBEDTLS_ECJPAKE_C`, present in ESP-IDF, used by Thread).
  Alternative, if the client stack does not have J-PAKE: SPAKE2 (RFC 9382) / SPAKE2+ (RFC 9383). Choice in TASK-100 with interop evidence:
  identical test vectors on the firmware and client sides. **Prohibited:** a custom cryptographic construction; transmitting the code
  in any form (in the clear, hash, HMAC) outside PAKE.
- **SEC-102 MUST — channel binding:** the PAKE transcript contains the SHA-256 of the DEVICE TLS certificate from the current handshake and
  the client identifier. On success the CLIENT pins **exactly that** certificate. A mismatch = abort without storing anything.
- **SEC-103 MUST — the only exception to verified TLS:** a connection with an unverified certificate is permissible only
  for the pairing endpoint, only while the window is open, only with `pair.*` messages. No status, HID, tokens,
  snapshots. Outside the window: `ERR_PAIRING_EXPIRED` (03 §7). A normal session (03 §6.1) always over pinned TLS. This does not affect the
  `ws://` prohibition (TECH-041, AT-SEC-01) — pairing also goes over TLS.
- **SEC-104 MUST — limits (AT-SEC-06):** 3 failed PAKEs within a window → window closed, code invalidated, a message on the DEVICE;
  3 such closures within 10 min → pairing locked out for 10 min + a "possible intruder on LAN" warning. Counters in
  `diagnostics`: `pair_attempts`, `pair_failures`, `pair_lockouts`. Chance of guessing within a window: 3·10⁻⁶ (as with Bluetooth Passkey).
- **SEC-105 MUST — secrets:** the code, the PAKE key and the token never in logs, telemetry or snapshots (TECH-046, AT-SEC-08);
  the code is erased from RAM after the window closes. The DEVICE stores only the SHA-256 of the token (NVS, encrypted in production);
  the CLIENT keeps the token in Windows Credential Manager / DPAPI, not in a configuration file.
- **SEC-106 MUST — preconditions:** pairing available only when Remote is enabled in Settings (`CLIENT_DISABLED` → no Pair
  screen) and `wifi:CONNECTED` (TECH-040). The pairing endpoint listens only within an open window.

### Protocol (extension of 03 §6.3; 03 §9 item 3 applies)

| `type` | Direction | Data | Notes |
|---|---|---|---|
| `pair.hello` | C→D | `client_name`, `pake` (e.g. `"ecjpake-p256"`), `v:1` | rejected outside the window |
| `pair.round1` / `pair.round2` | C↔D | PAKE messages (base64) | P-256 J-PAKE round 1 ≈ 430 B base64 — fits within 1024 B (03 §6.5) |
| `pair.confirm` | C↔D | key-confirmation MAC | error → SEC-104 counter |
| `pair.accepted` | D→C | `token_enc` (AEAD with the key from PAKE), `device_name` | after `Accept` on the DEVICE |
| `pair.rejected` | D→C | `reason` (`timeout`, `local_reject`, `locked`) | without details that would facilitate an attack |

New remote events require (03 §9 item 3): a whitelist of types, server-side validation, a threat model (below), an NFR budget,
fuzzing (AT-EXT-17) and a "no USB without owner" test (pairing does not touch HID — AT-EXT-16).

## Threat model (summary)

| Attack | Mitigation |
|---|---|
| MITM on the LAN during pairing (certificate substitution) | channel binding SEC-102 → abort; PAKE without the code gives no key |
| Online code guessing | 1 attempt per run; SEC-104: 3 attempts/window, lockout |
| Shoulder surfing of the code and getting ahead of the legitimate client | the `Accept` screen shows the client name (FR-725); the legitimate client gets "code used" → the user sees the anomaly |
| Using the code after it expires / reusing it | 120 s, single-use; `ERR_PAIRING_EXPIRED` |
| Token theft from the client | DPAPI/Credential Manager; `Remove` on the DEVICE invalidates it immediately (FR-726) |
| Leak through logs | SEC-105, AT-SEC-08 |

## Consequences

- Firmware: the `mdns` component (Espressif), PAKE from mbedTLS, a pairing state machine in `secure_remote`, two screens in `ui_views`
  (Settings), counters in `diagnostics`, a client registry in `config_store`.
- CLIENT (TASK-105): mDNS browsing (`DnsServiceBrowse`, Windows 10 1809+), a PAKE implementation compatible with the firmware, DPAPI.
- To check in TASK-000/100: `MBEDTLS_ECJPAKE_C` enabled in the mbedTLS configuration of the pinned ESP-IDF; the RAM/time cost of
  J-PAKE on the ESP32-S3 (roughly hundreds of ms — acceptable as a one-off, but **measure** it, do not assume).
- If PAKE turns out to be unattainable on both sides, variant D (SAS) is an alternative without changing FR-721.

## Tests

| ID | Level | Check | Expected |
|---|---|---|---|
| AT-EXT-13 | unit/host | PAKE test vectors on both sides; wrong code; mismatched certificate hash | keys match / abort without leaking material |
| AT-EXT-14 | HIL + LAN | full run with a character-counting protocol | exactly 6 typed-in characters; ≤ 60 s (FR-727) |
| AT-EXT-15 | SEC | MITM proxy with certificate substitution; 3 wrong codes; 3 windows; code reused; code after 120 s; window without a decision | abort; window closed; 10 min lockout; rejected; `ERR_PAIRING_EXPIRED`; endpoint closed no later than at 180 s |
| AT-EXT-16 | SEC | `pair.*` outside the window; status/HID over unpinned TLS | only `ERR_PAIRING_EXPIRED`; 0 status, 0 HID |
| AT-EXT-17 | fuzz | malformed / oversized `pair.*` frames | fail closed, no crash (extends AT-SEC-04/10) |

AT-SEC-06 from the spec is covered by AT-EXT-15.

## Migration

None — no pairing implementation exists.
