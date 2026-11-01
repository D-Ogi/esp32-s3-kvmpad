# ADR-023 — Approval policies and similar behaviors configurable from the device UI
Status: ACCEPTED (owner requirement)
Date: 2026-11-01
Approver: project owner (requirement of 2026-11-01: "whether you have to click Accept on the board and similar settings are to be configurable from the board UI")

## Context

- PROC-006 step 5: LOCAL approval for `control.request` "every time by default, **SECURITY decision**" — the spec leaves the policy to be determined.
- ADR-022 FR-725 introduced the `Accept`/`Reject` screen after a correct pairing code. ADR-020 already has a Keep-awake toggle.
  FR-310 (SHOULD) permits a confirmation for `Alt+F4`. FR-502 (natural scrolling) and the settings from TASK-090 (brightness, sensitivity,
  scroll speed) are already configurable.
- The owner wants behaviors of this kind — especially every "click to confirm" — to be settings in the board UI,
  and not constants.
- Threat to be ruled out: if the policies could be changed remotely, a client (or an attacker with a token) would disable the approvals for itself.
  The trust anchor is **physical presence** at the screen (TECH-040, ADR-011).

## Decision

### Principle

- **FR-731 MUST:** every behavior from the *Configurable policies* table is a setting in `Settings` on the DEVICE, saved
  in NVS through `config_store` (TECH-050: versioned schema, validation, write debounce). Every setting has: an id, a safe
  default value, a set of permissible values, a place in the UI and a one-sentence description of its effect visible in the UI.
- **SEC-107 MUST:** settings are changed **only by the local UI**. The CLIENT v1 protocol has no `settings.*` command;
  such a frame → `ERR_BAD_FRAME` + counter. The status snapshot (03 §6.6) does not reveal policy values. Relaxing a policy
  (e.g. `auto`) requires a tap on the DEVICE — that is the evidence of physical presence; additional PINs are not required.
- **FR-735 MUST:** the active control policy is visible in the CLIENT status details (FR-005), e.g. `Controlling · auto-approve`,
  so that the user always knows that a remote client can take over control without asking.
- **FR-736 MUST:** factory reset (PROC-010) restores the safe default values of all policies; NVS schema migration
  never relaxes a policy implicitly (unknown/corrupted value → default).

### Configurable policies (v1)

| ID | Setting (`id`) | Values | Default | Place in UI | Effect |
|---|---|---|---|---|---|
| **FR-732 MUST** | `pair.require_accept` | ON / OFF | **ON** | Settings → Remote | ON: the `Accept`/`Reject` screen with the client name (ADR-022 FR-725). OFF: pairing completes automatically after a correct code; the DEVICE shows for 10 s a toast "Paired: *<name>*" with a `Remove` button |
| **FR-733 MUST** | `control.approval` | `always` / `trusted` / `auto` | **`always`** | Settings → Remote | `always`: every `control.request` requires a tap (PROC-006 step 5). `trusted`: a "Trust this client" toggle is available with the approval — a trusted client gets control without asking, the rest ask; a `trusted` flag in the registry of paired clients (FR-726), `Remove` clears it. `auto`: every paired client gets control without asking |
| **FR-734 SHOULD** | `shortcuts.confirm_alt_f4` | ON / OFF | **OFF** (decision after the FR-310 tests) | Settings → Shortcuts | ON: `Alt+F4` requires a second tap within 2 s |
| (ADR-020) | `keepawake.enabled`, `keepawake.interval_s` | ON/OFF; 10–300 s | OFF; 30 s | Settings → Input | unchanged |
| (FR-502, TASK-090) | `touch.natural_scroll`, brightness, sensitivity, scroll speed | per spec | per spec | Settings → Input / Display | unchanged |

In every `control.approval` mode, **local priority** (ADR-011) applies unchanged: a recognized HID action on the screen
takes control away from the remote client, and the lease and epoch work as before.

### Not configurable (hard invariants — compile-time constants, no toggle in the UI)

| Invariant | Source |
|---|---|
| TLS with pinning, authentication, lease + epoch, `ws://` prohibition | TECH-041/042, 03 §6.1, AT-SEC-01 |
| `RELEASE_ALL` on loss of owner/USB/session/on error, mode change, OTA | AGENTS §2.3, 03 §4.2 |
| Physical opening of the pairing window on the DEVICE; whole window ≤ 180 s; single-use code; 3 attempts/window + lockout | ADR-022 FR-722, SEC-104, SEC-106 |
| Local priority LOCAL > REMOTE | TECH-044, ADR-011 |
| Remote OFF after the first start | TECH-040 |
| No remote change of settings, OTA, reset, exec | TECH-045, SEC-107, AT-SEC-07 |
| Secure boot / flash encryption / eFuse | TECH-054, ADR-012 |

Adding a new policy to the configurable list requires an ADR if it touches any of the items above.

## Consequences

- `config_store`: new NVS keys with safe defaults and validation; a `trusted` flag in the client registry.
- `secure_remote`: the `control.request` logic reads `control.approval`; `pair.require_accept` controls step 5 from ADR-022.
- `ui_views` / `ui_components`: the Settings → Remote screen with two settings and a description of the effect; a policy indicator in the status.
- Security: the default values are the most restrictive; relaxing requires a deliberate tap and is permanently visible (FR-735).

## Tests

| ID | Level | Check | Expected |
|---|---|---|---|
| AT-EXT-18 | unit + HIL | writing/reading policies, restart, corrupted NVS entry, factory reset | values persist; corrupted/unknown → default; defaults after the reset |
| AT-EXT-19 | SEC | `settings.*` frames and other attempts to change policies from the CLIENT (including a trusted one) | `ERR_BAD_FRAME`, counter, zero changes; snapshot without policy values |
| AT-EXT-20 | HIL + LAN | `control.approval` = always / trusted / auto; in every mode a local takeover during a remote drag | as per the table; LOCAL takeover always works, `RELEASE_ALL` in the log |
| AT-EXT-21 | HIL + LAN | `pair.require_accept` ON vs OFF | ON: waits for a tap, 30 s → reject; OFF: paired after the code, toast with `Remove` for 10 s |

## Migration

None — no settings implementation exists.
