# `remote_protocol`

> **Status:** `IMPLEMENTED_SW` — protocol part of TASK-100 (envelope reader, validator, session layer).
> Not yet wired into the firmware: the root `CMakeLists.txt` still lists the component in
> `EXCLUDE_COMPONENTS`, and the transport (WSS), the authentication and the pairing state machine belong to
> `secure_remote`.

**Role:** CLIENT protocol v1 — the only place where untrusted network bytes become typed intents.

| | |
|---|---|
| Tasks | TASK-100 |
| Specification | 03 §6 (6.1–6.6), §7, §8 (VEC-005/007/008/009); 02 §8, §10 |
| Decisions | ADR-011, ADR-018, ADR-019, ADR-021, ADR-022, ADR-023, ADR-024, **ADR-030** |
| Tests | `test/remote_protocol/` — AT-CONN-04..09, AT-SEC-02/03/04/07/08, AT-FAULT-06, AT-EXT-19, fuzz |

## What it does

- `remote_json.*` — bounded JSON reader for the v1 envelope: 1024 B frames, 64 tokens, 3 levels of nesting,
  iterative (no recursion), zero allocation, duplicate keys / bad UTF-8 / lone surrogates / trailing bytes
  refused. The document borrows the caller's frame buffer and never copies it.
- `remote_protocol.*` — `RemoteProtocolValidator` (TECH-043) and the session/command layer: `hello`,
  `status.get`, `control.request`/`release`, `ping`, `input/*`, `session.close`, `pair.*` routing. Replay
  guard, lease epochs, a rolling input rate window, a non-blocking 30 s approval timer, per-session
  idempotence shadow and aggregated counters.
- `remote_wire.*` — the server→client serializer with its own per-session `seq` and optional `reply_to_seq`.

## What it never does (MUST)

- **No HID.** No `tusb.h` / `tinyusb.h`, no pressed-ledger, no report. A validated frame becomes a
  `kvm_hid_intent_t` handed to `kvm_rp_hooks_t::hid_sink`, which the integrator forwards to `hid_dispatch` →
  `InputArbiter` → `HidScheduler` (03 INV-001).
- **Not the source of truth.** `HOST_ACTIVE`, ownership, the epoch and the maintenance flag arrive through
  `kvm_rp_set_host()` / `kvm_rp_set_owner()` from the authoritative `hid_core` snapshot; the protocol mirrors
  them and never decides them optimistically (03 §4.2, FR-403).
- **No TLS, WebSocket, auth or pairing.** The transport hands over complete, already authenticated frames.
  `pair.*` goes to `secure_remote`; with no pairing window the answer is `ERR_PAIRING_EXPIRED` (SEC-103).
- **No logging.** Counters only. No payload, usage id, client name, token or pairing code is logged or echoed
  into an error payload (TECH-046, SEC-105, AT-SEC-08). The only loggable strings are `kvm_rp_err_name()` and
  `kvm_rp_revoke_reason_name()`.
- **No allocation and no unbounded recursion** on any path; all time is injected by the caller (TECH-006).

## Release paths

Every loss of the lease asks the integrator for a revoke plus `RELEASE_ALL` with the matching
`kvm_release_reason_t`, and the reason is counted:

| Event | `control.revoked.reason` | `RELEASE_ALL` reason |
|---|---|---|
| `control.release` | `client_release` | `KVM_RA_REMOTE_RELEASE` |
| no heartbeat for 3 s | `client_stale` | `KVM_RA_LEASE_TIMEOUT` |
| `session.close` / transport drop | `session_closed` | `KVM_RA_REMOTE_DISCONNECT` |
| local input took over (ADR-011) | `local_override` | `KVM_RA_LOCAL_OVERRIDE` |
| USB lost | `host_offline` | `KVM_RA_USB_LOSS` |
| OTA / factory reset | `maintenance` | `KVM_RA_MAINTENANCE` |
| input flood | `rate_flood` | `KVM_RA_REMOTE_DISCONNECT` |
| 8 refused frames | `protocol_abuse` | `KVM_RA_REMOTE_DISCONNECT` |
| client removed from the registry | `unpaired` | `KVM_RA_REMOTE_DISCONNECT` |

## Integration

```c
static kvm_rp_t g_rp; // ~2.9 KiB, statically allocated by the integrator

kvm_rp_hooks_t hooks = { .hid_sink = app_hid_sink, .control_request = app_request,
                         .control_grant = app_grant, .control_release = app_release,
                         .heartbeat = app_heartbeat, .approval_begin = ui_ask,
                         .approval_cancel = ui_cancel, .send = ws_send,
                         .close_session = ws_close, .status_fill = app_status };
kvm_rp_init(&g_rp, NULL, &hooks);               // NULL = KVM_RP_CONFIG_DEFAULT
kvm_rp_set_host(&g_rp, snapshot.host_usb, snapshot.maintenance, wifi_up, now_us);
kvm_rp_set_owner(&g_rp, snapshot.owner_kind, owner_session, snapshot.owner_epoch, now_us);
```

- One instance, one task. The component is not reentrant and no hook may call back into it.
- Call `kvm_rp_poll()` at least every 50 ms: it drives the approval timeout, the heartbeat staleness check
  and the coalesced-movement flush.
- `hid_sink` must return `kvm_rp_err_from_hid()` of the arbiter result, so a refusal reaches the client with
  the right code instead of being invented here.
- `control_release` must perform revoke + `RELEASE_ALL(reason)` + epoch bump and write back the epoch that
  is in force afterwards.
- For the paths the arbiter decides on its own (local override, USB loss, maintenance, client removed), call
  `kvm_rp_set_owner()` and then `kvm_rp_notify_revoked()` so the client learns why.

## Extensions already covered

- ADR-018: media keys only as `input/shortcut` with an allowlisted `command_id` (TECH-075).
- ADR-019: "Type text" is a CLIENT feature over the existing `keyboard.key`; no firmware command.
- ADR-022: `pair.*` whitelisted, validated and fuzzed; routed to `secure_remote`, never to HID (AT-EXT-16/17).
- ADR-023: no `settings.*` in v1 → `ERR_BAD_FRAME` + counter; the snapshot carries no policy values (SEC-107).
- ADR-024: the read-only `host_layout` field in the snapshot, validated, never settable from the CLIENT
  (FR-745, AT-EXT-25).

Governing documents: `AGENTS.md`, `docs/ADR/ADR-030-remote-protocol-component.md`.
