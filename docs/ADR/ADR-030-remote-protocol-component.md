# ADR-030 — Protocol v1 details the specification leaves open (`remote_protocol`)
Status: ACCEPTED (owner, 2026-11-02)
Date: 2026-11-02
Approver: project owner (decision of 2026-11-02, accepted as proposed)

## Context

TASK-100 implements the CLIENT protocol v1 validator and session layer from `03-CONTRACTS-AND-PROTOCOLS.md`
§6 and §7. The specification fixes the envelope, the command table, the validation table and the error codes,
but a working server still has to answer a dozen questions the documents do not settle. Each one below is a
place where an implementer would otherwise guess, and a wrong guess is either a security hole or an
interoperability break with the CLIENT of TASK-105.

The decisions are listed with the alternative that was rejected, because the CLIENT must implement the same
choices and `03 §9 item 2` forbids changing the wire protocol as a side effect of a later task.

## Decision

### 1. Two error codes that 03 §6.5 needs but 03 §7 does not name

| Code | Condition (03 §6.5 row) | `retryable` |
|---|---|---|
| `ERR_AUTH` | "Invalid auth → deny; delay/rate-limit; no token in the log" | `false` |
| `ERR_RATE_LIMIT` | ">120 input msg/s in a rolling window → … reject the non-idempotent excess" | `true` |

Rejected: reusing `ERR_BAD_FRAME` for both, which would make the counters useless and tell a correct client
nothing about whether to slow down or to stop.

`ERR_HID_QUEUE_OVERFLOW` (03 §7) is also reachable from this layer, because the arbiter can refuse an intent
the validator already accepted; it is forwarded verbatim.

### 2. `seq` starts at 1 and the server assigns the `session_id` label

- `seq` must be an integer in `1 .. 2^53-1`. `seq = 0` is `ERR_BAD_FRAME`, so the first frame establishes the
  replay baseline unambiguously. Afterwards `seq` must be strictly increasing; gaps are legal (WSS is ordered,
  the application may skip numbers), repeats and regressions are `ERR_REPLAY`.
- `session_id` is the server's label (03 §6.2: "a non-secret server label"). The client cannot know it before
  `hello.ack`, so in `hello` the member must be absent or an empty string, and every later frame must carry
  exactly the label from `hello.ack`. Anything else is `ERR_SESSION`.
- `hello` is mandatory and must come first; any other type on a session that has not said `hello` is
  `ERR_SESSION`. A second `hello` on the same session is `ERR_BAD_FRAME`.

Rejected: letting the client choose its own `session_id`, which would turn a label into a de facto identifier
and invite collisions between connections.

### 3. `input/shortcut` carries the allowlist **name**, not a number

`command_id` is a string matched against the `name` field of the compiled chord table in
`components/hid_core/include/hid_keymap.h` (`ctrl_alt_del`, `alt_tab`, `task_manager`, `show_desktop`,
`close_window`, `screenshot`, `layout_switch_win`, `layout_switch_ctrl`, `media_play_pause`, `media_next`,
`media_prev`, `media_mute`, `media_volume_up`, `media_volume_down`). An unknown name is `ERR_USAGE` and emits
no HID. A numeric `command_id` is `ERR_BAD_FRAME`.

Why: the numeric ids are a build-time enum. If they were the wire form, inserting a chord would silently
remap every client's buttons. ADR-018 already routes media keys through `input/shortcut`, so the table is
shared with the CLIENT as text.

### 4. Ranges the specification bounds only for `dx`/`dy`

`mouse.wheel` `vertical` and `horizontal` take the same per-event bound as the pointer deltas,
`[-4096, 4096]`; outside it, `ERR_RANGE`. `usage_id` must be `0..255` (`ERR_USAGE` outside) and must then pass
the usage allowlist. `lease_epoch` must fit `uint32`. A fractional or exponential JSON number is never an
integer field: in a range-checked field it is `ERR_RANGE`, in a structural field (`v`, `seq`, `lease_epoch`)
it is `ERR_BAD_FRAME`.

Rejected: leaving the wheel unbounded up to `int16`, which lets one 2-byte field ask for 32767 wheel reports.

### 5. The throttle never drops a release

03 §6.5 says to "reject the non-idempotent excess" above 120 input msg/s. Applied literally to a `down:false`
frame, that leaves a key or a mouse button pressed on the HOST, which `AGENTS.md` §7 priority 2 forbids. So:

- `mouse.move` above the limit is accumulated and flushed later, with the sum preserved (03 §3 rule 3);
- `keyboard.key`/`mouse.button` with `down:false` is **always** admitted, whatever the rate;
- every other input above the limit is `ERR_RATE_LIMIT` and no HID;
- above `input_flood_limit` (default 240 msg/s, twice the limit) the lease is revoked with
  `control.revoked.reason = "rate_flood"`, `RELEASE_ALL` is requested and the session is closed, which is the
  "revoke on flood" of the same row.

### 6. Idempotence is tracked per session, but hid_core stays authoritative

The validator keeps a per-session shadow bitmap of the usages and buttons that this session pressed in the
current epoch, so that a repeated `down:true` is the "idempotent no-op, do not repeat to USB" of 03 §6.5 and a
`down:false` without a `down:true` is the "no-op + metric". The shadow is cleared on every grant, revoke,
release and epoch change, and a key is marked pressed only after the arbiter accepted the intent. It is a
filter and a metric, never a source of truth: the pressed-ledger in `hid_core` remains the only authority
(03 INV-001).

### 7. Strict member allowlists, both levels

An unknown member in the envelope or in a payload is `ERR_BAD_FRAME`, and a duplicate member name anywhere
in the frame is rejected by the reader (compared after unescaping, so `"v"` and `"v"` collide as they
should). 03 §6.3 defines a closed command table and 03 §6.2 a closed envelope, so nothing is gained by
tolerating extra fields and a smuggled duplicate is a classic parser-differential attack.

Consequence for later versions: adding a field to v1 is a protocol change, not a compatible extension
(03 §9 item 2 already says so).

### 8. Pairing and settings scope

- `pair.*` frames are accepted **only** on a session the transport marked `pairing_only`, and are handed to
  `secure_remote` (ADR-022). Without an open window, or on an ordinary session, the answer is
  `ERR_PAIRING_EXPIRED` (SEC-103). A `pairing_only` session gets `ERR_PAIRING_EXPIRED` for every non-`pair.*`
  type, so it can reach no status, no snapshot and no HID.
- There is no `settings.*` command, so such a frame is simply an unknown type: `ERR_BAD_FRAME` plus the
  counter (SEC-107, ADR-023 AT-EXT-19).

### 9. Wire spellings and defaults

- HOST state strings in `hello.ack` and `status.snapshot` share one prefix: `USB_MOUNTED` (as in 03 §6.6),
  `USB_SUSPENDED`, `USB_DETACHED`, `USB_ENUMERATING`, `USB_ERROR`, `USB_UNINITIALIZED`.
- `control.revoked.reason` is an enumerated string, never free text: `client_release`, `client_stale`,
  `session_closed`, `local_override`, `host_offline`, `maintenance`, `rate_flood`, `protocol_abuse`,
  `unpaired`.
- `input.ack` (03 §6.3 "optionally") is **off** by default: one ack per input frame doubles the message rate
  for no benefit to a client that already has `error` and `status.snapshot`.
- `reply_to_seq` (03 §6.4) is added to every direct reply by default.
- `status.snapshot` carries the read-only `host_layout` of ADR-024 / FR-745. The enumerated allowlist lives in
  `kbd_layouts`; the serializer additionally refuses anything outside `[a-z0-9_()-]{1,23}` so no free-form
  value can reach the wire.
- At most one physical approval prompt is open at a time; a second `control.request` while one is pending is
  `ERR_CONTROL_DENIED`. A `control.request` from the session that already holds the lease is answered with
  `control.granted` for the current epoch, without a new epoch and without a release.
- Protocol abuse (`ERR_BAD_FRAME`, `ERR_VERSION`, `ERR_SESSION`, `ERR_REPLAY`, `ERR_AUTH`,
  `ERR_PAIRING_EXPIRED`) is counted per session; 8 of them close the connection ("count + close on abuse").
  `v != 1` closes immediately.

### 10. No JSON library

ESP-IDF v6.1 no longer ships cJSON as a bundled component (checked in the pinned tree at
`.tools/esp-idf-v6.1`), and a registry dependency for a 1024-byte envelope is not worth the supply-chain
surface. The component carries its own iterative reader: 1024 B frames, 64 tokens, 3 levels of nesting, no
recursion, no allocation, duplicate keys and bad UTF-8 refused.

## Consequences

- The CLIENT of TASK-105 must implement items 2, 3, 4 and 9; they are the observable wire contract.
- `03 §6.5` is implemented row for row; the two new codes and the throttle refinement are the only places
  where this component does more than the table says, and both are in the direction of refusing more.
- If the owner rejects any item, the change belongs in this ADR and in the validator, not in the frozen
  specification.

## Tests

`test/remote_protocol/` (host, sanitized): `test_remote_json` (reader grammar and limits),
`test_remote_validate` (one case per 03 §6.5 row, AT-SEC-02/03/04/07, AT-EXT-19),
`test_remote_session` (03 §6.3/§6.4/§6.6, AT-CONN-04/05/06/08), `test_remote_vectors`
(VEC-005, VEC-007, VEC-008, VEC-009, AT-CONN-07, AT-CONN-09, AT-FAULT-06) and `fuzz_remote_frame`
(seeded mutation fuzzer, AT-SEC-04, AT-EXT-17).

## Migration

None — no CLIENT implementation exists yet (ADR-021, TASK-105).
