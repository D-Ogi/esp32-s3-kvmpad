# `secure_remote`

> **Status:** `IMPLEMENTED_SW` — pairing core, transport cores and the ESP-IDF adapters are in place
> and covered by host tests. Nothing in this component has run on hardware yet; every `AT-SEC-*` /
> `AT-CONN-*` item that needs the board is `NOT_RUN_HW`.

**Role:** TLS/WSS, pairing with physical acceptance, permissions, leases.

| | |
|---|---|
| Tasks | TASK-100 |
| Specification | 02 §8, §8.1; 03 §6–§8; ADR-002, ADR-012, ADR-021, ADR-022, ADR-023, ADR-028, ADR-029, ADR-030, ADR-032 |
| Tests | AT-SEC-*, AT-CONN-*, AT-EXT-14..17 |

## Layout

| Path | What it is | Where it is tested |
|---|---|---|
| `pairing/` | window state machine, EC-JPAKE P-256 with channel binding, token issue, paired-client register | `test/pairing` |
| `src/kvm_srt_auth.c` | `Authorization: Bearer` parsing and strict base64url for the token | `test/secure_remote/test_srt_auth.c` |
| `src/kvm_srt_pair_json.c` | the `pair.*` envelope and payload codec, kept out of `remote_protocol` on purpose | `test/secure_remote/test_srt_pair_json.c` |
| `src/kvm_srt.c` | session glue: admission, connections, loss paths, approval, pairing routing | `test/secure_remote/test_srt_session.c`, `fuzz_srt_pair.c` |
| `port/kvm_srt_identity.c` | per-device ECDSA P-256 key and self-signed certificate, generated once and persisted | compile/link only; behavior is HIL |
| `port/kvm_srt_server.c` | one `esp_https_server` instance, two WebSocket endpoints, the tick | compile/link only; behavior is HIL |
| `port/kvm_srt_mdns.c` | optional `_kvmpad._tcp` advertisement; stubs when the mDNS component is absent | compile/link only |

The split is the one the rest of the repository uses: pure C with injected platform hooks for
everything that can be decided, thin adapters for everything that cannot. The cores have no
ESP-IDF dependency beyond PSA crypto and are built by the host suites; `port/` is the only place
that knows about TLS, sockets, tasks or the MAC address.

## The two endpoints (ADR-028 §2)

| | `/v1/ws` | `/v1/pair` |
|---|---|---|
| Subprotocol | `kvmpad.v1` | `kvmpad.pair.v1` |
| Credential | `Authorization: Bearer <43-char base64url token>` | none |
| Open when | Remote on, Wi-Fi up, not in maintenance | only inside a pairing window |
| Carries | protocol v1 (03 §6): input, control, status | `pair.*` only |

Both are `wss`. The server runs in `HTTPD_SSL_TRANSPORT_SECURE` mode, so the build contains no
plaintext listener at all. Authentication happens in the WebSocket **pre-handshake** callback: a
peer without a usable token is answered `401` and the socket is closed before any WebSocket state
machine or JSON parser exists.

The pairing endpoint never becomes a `remote_protocol` session. It has no session slot, no owner
mirror and no intent sink — `kvm_rp_hooks_t::pair_frame` is deliberately `NULL` — so "unverified
TLS only for `pair.*`" is a structural property of the wiring rather than a check somewhere
(SEC-103).

## Rules (MUST)

- No "temporary ws://" in the release build.
- No secrets/tokens in the repository or in logs; per-device keys (TECH-056). The register stores
  the SHA-256 of a token, never the token.
- The certificate is generated once and then only read back. Regenerating it invalidates every
  CLIENT pin and costs every user a re-pairing, so a stored blob that does not parse is reported
  rather than silently replaced (ADR-028 §3).
- Nobody changes ADR-002/012 without the owner's consent. ADR-002 was resolved by ADR-021:
  CLIENT = a native Windows application with pinning of the device certificate established during
  physical pairing; a browser CLIENT is outside v1.
- ADR-022: pairing — mDNS `_kvmpad._tcp`, a 6-digit one-time code (120 s; the whole window ≤ 180 s,
  a compile-time constant), PAKE (EC-JPAKE P-256 from mbedTLS) with channel binding to the TLS
  certificate, Accept/Reject on the DEVICE, 3 attempts/window + lockout (FR-721..727, SEC-101..106).
  Unverified TLS **only** for `pair.*` within an open window — never status, HID or tokens.
- ADR-023: `control.approval` (always/trusted/auto) and `pair.require_accept` are read from
  `config_store`; no CLIENT changes the settings (SEC-107); local priority (ADR-011), lease and
  epoch work independently of the policy.
- The UI gets callbacks, never LVGL calls from here. Nothing in this component includes `tusb.h`,
  `tinyusb.h` or `lvgl.h`.

## Integration

`port/kvm_srt_server.h` is the whole surface the firmware uses. In short:

1. `kvm_srt_identity_init()` with a blob store mapped onto NVS.
2. `kvm_srt_server_start()` with that identity, the pairing platform and register, and the hooks
   that forward intents to `hid_dispatch` and pairing events to the UI.
3. Push authoritative state in as it changes: `kvm_srt_server_set_host()`,
   `_set_remote_enabled()`, `_set_owner()`, `_set_policy()`, `_notify_revoked()`.
4. Answer the Accept screen with `_pair_accept()` / `_pair_reject()` and a control request with
   `_approval_result()`.

Required `sdkconfig` additions, the managed dependency line for mDNS and the start order are listed
in `docs/ADR/ADR-032-remote-transport.md`.

**Subdirectories:**

- `pairing/` — the pairing core of TASK-100: window state machine, EC-JPAKE P-256 with channel
  binding, token issue and the paired-client register. The normative wire format is
  `docs/ADR/ADR-029-pairing-wire-format-and-pake.md`. See `pairing/README.md`.

Governing documents: `AGENTS.md`.
