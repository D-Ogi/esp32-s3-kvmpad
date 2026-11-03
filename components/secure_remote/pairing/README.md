# `secure_remote/pairing`

> **Status:** `IMPLEMENTED_SW` — host tests pass, no hardware run yet. Normative format: ADR-029.

The pairing core of TASK-100: the 6-digit-code window, EC-JPAKE on P-256 with channel binding, explicit key
confirmation, token issue and the paired-client register (FR-721..727, SEC-101..106, ADR-022, ADR-023,
ADR-029).

## Why it has no ESP-IDF dependency

The Windows CLIENT has to reproduce the DEVICE bit for bit (ADR-021, ADR-029 §1). Rather than write the
protocol twice, these sources compile unchanged for three targets:

| Target | Random | Clock | Paired-client store |
|---|---|---|---|
| firmware | `esp_random` / `esp_fill_random` | `esp_timer_get_time() / 1000` | `config_store` pairing register |
| host tests | seeded fixture (`test/pairing/det_rng.c`) | hand-driven counter | in-RAM fake |
| CLIENT (DLL) | `BCryptGenRandom` | `QueryPerformanceCounter` | DPAPI / Credential Manager |

Everything platform-specific arrives through `kvm_pair_platform_t` and `kvm_pair_store_t`
(`include/kvm_pairing_platform.h`). The only external interface the core itself uses is the PSA crypto API.

## Files

| File | Contents |
|---|---|
| `include/kvm_pairing_types.h` | constants, timings, result codes and their wire mapping |
| `include/kvm_pairing_platform.h` | the two injected interfaces |
| `include/kvm_pairing_pake.h` | the PAKE layer: binding, rounds, confirmation, token sealing |
| `include/kvm_pairing_wire.h` | base64, hex, the length-prefixed round blobs, the mDNS TXT record |
| `include/kvm_pairing.h` | the DEVICE window state machine and the paired-client register |
| `include/kvm_pairing_client.h` | the CLIENT driver |
| `src/kvm_pairing_pake.c` | the only file that includes `psa/crypto.h` |
| `src/kvm_pairing_session.c` | the state machine |
| `src/kvm_pairing_client.c` | the CLIENT driver |
| `src/kvm_pairing_wire.c` | the encoding helpers |

## Message flow

The transport decodes the envelope of `03 §6.2` and calls one handler per `pair.*` message; each handler
returns the message to send in `kvm_pair_out_t`. The UI calls `kvm_pair_window_open`,
`kvm_pair_local_accept`, `kvm_pair_local_reject` and `kvm_pair_local_cancel`, and drains
`kvm_pair_take_event` on every tick. `kvm_pair_tick` has to run at least every 250 ms while a window is open;
it owns the code expiry, the 180 s cap, the Accept timeout, the toast and the end of a lockout.

`kvm_pair_endpoint_open` is what the transport gates SEC-103 on, and what sets `pair=1` in the TXT record.

## Rules (MUST)

- The core never logs. Nothing in it can print the code, the derived keys or the token (TECH-046, SEC-105).
- The code is zeroized the moment the window closes, in every path, including a failure.
- Nothing is stored before key confirmation passed and — with `pair.require_accept` ON — LOCAL accepted.
- A removed client (`kvm_pair_clients_remove`, `kvm_pair_clients_remove_all`) must be followed by the caller
  revoking that client's session and raising `RELEASE_ALL` (FR-726). The core cannot do that itself; it owns
  no session and no HID path.
- `kvm_pair_clients_verify_token` compares in constant time and walks the whole register without an early
  exit. Do not "optimize" the loop.
- A random draw that fails is a hard failure, never a fallback to a weaker source.

## Tests

`test/pairing/` builds the pinned TF-PSA-Crypto from source and runs four host suites plus the interop
vectors. See `test/pairing/CMakeLists.txt` for the exact commands.

Governing documents: `AGENTS.md`, `docs/ADR/ADR-022-pairing-6-digit-pake.md`,
`docs/ADR/ADR-029-pairing-wire-format-and-pake.md`.
