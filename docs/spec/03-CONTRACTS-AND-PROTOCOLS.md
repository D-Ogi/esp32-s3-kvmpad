# 03 — Module contracts, CLIENT protocol and state machines

**Contract version:** `kvm-contracts/1.0` · **Status:** normative for firmware v1 · **Principle:** one HID stream, explicit ownership and unconditional release on an epoch change.

## 1. Format and contractual requirements

- **MUST:** the type and enum names are the template for the C implementation. Aliases in the code are possible only with conformance tests.
- **MUST:** `timestamp_us` is monotonic on the DEVICE; `epoch` serves to reject stale events; `sequence` is an incremented counter per source/session.
- **MUST:** all internal interfaces use **typed structs** / sizes bounded in advance. JSON is permitted **only at the CLIENT boundary** (network).
- **MUST:** no API that returns an error leaves an active pressed state without a `RELEASE_ALL` path.
- **MUST:** boundary input untrusted: USB/Touch is treated as a physical source, but I²C can deliver faulty data; the network means fully untrusted messages.

## 2. Internal TouchInput contract

```c
typedef struct {
    uint8_t track_id;
    int16_t x_px;                 // 0..799 after scaling/rotation
    int16_t y_px;                 // 0..479 after scaling/rotation
    uint16_t strength;           // diagnostic only, no threshold required
} kvm_touch_point_t;

typedef struct {
    uint64_t timestamp_us;
    uint32_t sample_seq;
    bool valid;                  // false = controller error -> cancel
    uint8_t point_count;         // 0..5
    kvm_touch_point_t point[5];
} kvm_touch_sample_t;
```

**Validation:** `point_count ∈ [0,5]`, `x/y` within bounds, `track_id` unique within the sample, no older `sample_seq`, a gap >120 ms while a gesture is active = `CANCEL`. Raw `strength` must not cause a click on its own. `point_count=0` is mandatory release information, do not ignore empty frames.

### 2.1 Gesture selection

```text
GEST_IDLE
  ├─ 1 touch ──────────> ONE_PENDING
  │     ├─ movement >6 ─> ONE_MOVING  ──up/cancel──> IDLE
  │     ├─ time >=550 ──> RIGHT_FIRED ──up────────> IDLE
  │     ├─ 2nd touch ───> TWO_PENDING
  │     └─ up <=220 ───> LEFT_TAP_FIRED ────────> IDLE
  └─ 2 near-simultaneous ─> TWO_PENDING
        ├─ centroid travel >10 ─> TWO_SCROLLING ──all up──> IDLE
        └─ both up <=250 ──────> RIGHT_FIRED ────────> IDLE
ANY STATE + invalid/overflow/owner lost/three fingers -> CANCELLED -> IDLE
```

**State machine MUST:** 1 instance per physical panel, timing decisions counted from the start of the specific class. A tap should be sent **exactly once**. The ONE_PENDING→TWO_PENDING transition invalidates the pending LEFT_TAP and does not send the remaining tap. Two-finger scroll does not emit pointer_move.

**`double-tap-and-drag`:** a separate `TapSequenceTracker` joins a completed `LEFT_TAP` with the next `ONE_PENDING` within a window of ≤300 ms and a distance of ≤32 px; if the second contact moves >8 px, it sends `LEFT_DOWN` before the first `MOVE` delta and `LEFT_UP` at the end. If the second contact ends without movement, it sends an ordinary left click (the OS recognizes a double click). Once TWO_PENDING has started, the tracker is cancelled.

## 3. Internal HID intent format

```c
typedef enum {
    KVM_ORIGIN_LOCAL = 1,
    KVM_ORIGIN_REMOTE = 2,
    KVM_ORIGIN_SYSTEM = 3
} kvm_origin_t;

typedef enum {
    KVM_HID_KEY_DOWN,
    KVM_HID_KEY_UP,
    KVM_HID_MOUSE_MOVE,
    KVM_HID_MOUSE_BTN_DOWN,
    KVM_HID_MOUSE_BTN_UP,
    KVM_HID_MOUSE_WHEEL,
    KVM_HID_RELEASE_ALL,
    KVM_HID_CHORD
} kvm_hid_action_type_t;

typedef struct {
    kvm_hid_action_type_t type;
    kvm_origin_t origin;
    uint32_t epoch;
    uint32_t seq;
    uint64_t timestamp_us;
    union {
        struct { uint8_t usage_id; } key;
        struct { int16_t dx, dy; } mouse_move;
        struct { uint8_t button_id; } mouse_button; // 1=L,2=R,3=Middle
        struct { int16_t horizontal, vertical; } wheel;
        struct { uint16_t chord_id; } chord;
    } data;
} kvm_hid_intent_t;
```

HID rules:

1. `usage_id` is a **USB HID usage Page 0x07 / keyboard usage**, not Unicode / an ASCII keycode / a Windows VK code. `0xE0..0xE7` are the modifiers, they have their own bitmap; `AltGr` → Right Alt 0xE6.
2. `CHORD` uses a `chord_id` from a compiled allowlist, **never** an array of arbitrary keycodes from the network.
3. `mouse_move` values are signed int16 in relative units; when sending HID with an 8-bit report, split the deltas into separate reports within the range `[-127,127]` (or `[-128,127]` according to the descriptor/report path). Always preserve the sum (`∑dx_out = dx_in`), do not overflow.
4. `wheel` has a separate fractional accumulator (pre-engine or translator). If the REPORT does not support a horizontal wheel, an explicit fallback without generating a keyboard shortcut "by eye".
5. `KEY_DOWN` a second time for the same usage in the same epoch is idempotent; `KEY_UP` of a pressed state that does not exist does not send random bitmap changes. All short clicks are a sequence of the proper two reports, not a "pulse high" without monitoring USB readiness.
6. When USB is `SUSPENDED` or `DISCONNECTED`, the system accepts only `RELEASE_ALL` **logically** and service operations; it rejects ordinary HID.

## 4. InputArbiter — ownership, epoch and invariants

### 4.1 Model

```c
typedef enum {
    OWNER_NONE = 0,
    OWNER_LOCAL,
    OWNER_REMOTE
} kvm_owner_kind_t;

typedef struct {
    kvm_owner_kind_t kind;
    uint64_t session_id;          // 0 when local/none
    uint32_t epoch;               // +1 on every ownership change
    uint64_t lease_expires_us;    // only REMOTE; clock monotonic
} kvm_control_owner_t;
```

### 4.2 Unconditional invariants

- **INV-001:** only `hid_dispatch` modifies the actual pressed-ledger and calls the TinyUSB HID API.
- **INV-002:** every `epoch` change is preceded or concluded by an atomically guaranteed neutralization of the logical state; old queued intents are rejected.
- **INV-003:** `LOCAL` has priority. Remote commands cannot take over control while local input is active, nor after a local override without a new assignment.
- **INV-004:** only one REMOTE session holds a lease. Read-only sessions cannot send input.
- **INV-005:** `USB_MOUNTED` is required to send a report. No `USB_MOUNTED` = no "simulated" confirmation of an action.
- **INV-006:** after boot / restart / suspend / Wi-Fi drop / OTA / config-reset the pressed-ledger state is empty.
- **INV-007:** no `SYSTEM` action may acquire the ability to press a key by itself outside the explicitly approved release procedures and offline diagnostic tests.

### 4.3 Arbiter pseudocode (normative algorithm)

```c
on_local_input(local_action):
    if (owner.kind == OWNER_REMOTE):
        revoke_remote("local_override")
        increase_epoch()
        release_all_high_priority()
    if (owner.kind != OWNER_LOCAL):
        increase_epoch()
        owner = LOCAL
    apply_with_current_epoch(local_action)

on_control_request(session):
    require session.authenticated && session.permission == OPERATOR
    require owner.kind != OWNER_LOCAL || local_idle && physical_approval
    physical_approval_on_device()
    increase_epoch(); release_all_high_priority()
    owner = REMOTE(session_id, epoch, now + 4s)
    send_granted(lease_epoch = epoch)

on_remote_input(frame):
    require USB_MOUNTED && !MAINTENANCE
    require owner == REMOTE(frame.session) && frame.lease_epoch == owner.epoch
    require authenticated && valid_sequence && permission == OPERATOR
    validate_rate_payload_and_usage(frame)
    dispatch(frame.action)

on_heartbeat(session):
    if valid(session) && owner.session == session:
        owner.lease_expires = monotonic_now + 4s

on_timeout_or_disconnect(session):
    if owner == REMOTE(session):
        increase_epoch(); release_all_high_priority(); owner = NONE
```

**Important:** `on_local_input` should perform `revoke + release + local action` in a logically ordered `hid_dispatch` transaction: there must be no race in which a `REMOTE keyDown` falls between the release and the local event. `LOCAL` events while a remote session exists may trigger an `override`, even if it is the first mouse movement.

**Lease model (v1):** a heartbeat from the CLIENT every 1 s; lease validity 4 s from the last correct heartbeat; after 3 s without a heartbeat the CLIENT state is `STALE` and the ability to send is lost immediately. The values are configured **only by the firmware**, not by the client.

## 5. USB HID report model

### 5.1 Recommended descriptor layout

| Interface | Class/subclass/protocol | Contents | Fallback |
|---|---|---|---|
| HID 0 | HID / Boot / Keyboard | 8-byte boot report: modifier bitmap, reserved byte, 6 usage bytes | Pre-OS/UEFI |
| HID 1 | HID / Boot / Mouse | relative X/Y, buttons | Boot Mouse |
| Optional HID 2 / report ID | HID Report | enhanced wheel horizontal / media keys, if the tests require it | the plain boot mouse remains functional |

**MUST:** if a custom mouse report with a wheel works only in the OS, the documentation states which functions do not work in the BIOS. On POST/UEFI the `HID boot-compatible` assessment is carried out on real computers. After `SET_PROTOCOL(BOOT)` the reports must have the correct boot format; do not assume that a single composite report-ID HID gives the same support.

### 5.2 Keyboard map (excerpt of USB HID Usage 0x07)

| Key | Usage hex | Key | Usage hex |
|---|---|---|---|
| A | 0x04 | Z | 0x1D |
| 1 | 0x1E | 0 | 0x27 |
| Enter | 0x28 | Escape | 0x29 |
| Backspace | 0x2A | Tab | 0x2B |
| Space | 0x2C | Caps Lock | 0x39 |
| F1 | 0x3A | F12 | 0x45 |
| Print Screen | 0x46 | Insert | 0x49 |
| Home | 0x4A | PageUp | 0x4B |
| Delete Forward | 0x4C | End | 0x4D |
| PageDown | 0x4E | RightArrow | 0x4F |
| LeftArrow | 0x50 | DownArrow | 0x51 |
| UpArrow | 0x52 | Left Ctrl (modifier) | 0xE0 |
| Left Shift | 0xE1 | Left Alt | 0xE2 |
| Left GUI | 0xE3 | Right Alt (AltGr) | 0xE6 |

**MUST:** the full table in the mapping code is based on the official USB HID Usage Tables; a static test for 100% of the UI keys and shortcuts. Do not assign the keycode `ASCII('A')` as a usage. Tests in `04`.

### 5.3 Mouse/wheel

- Button mask bit0 = Left, bit1 = Right, bit2 = Middle.
- `button_down/up` always updates the bitmask; a change of X/Y sends the same mask, it does not zero it by accident (drag).
- The send frequency depends on the readiness report and host polling; target 100–125 Hz input processing, not a claim about a guaranteed USB poll interval.
- Sum the `dx/dy` movements with overflow control; if the queue is clogged, `RELEASE_ALL` has absolute priority, and pure movement may be compressed.
- After `USB_SUSPENDED` do not automatically generate a remote wake on network activity alone. If a remote wake feature is added, it requires `host-enabled remote wakeup` and a USB spec test.

## 6. Protocol CLIENT v1 — over WSS

### 6.1 Transport and trust

**Transport MUST:** WebSocket over TLS (`wss`) with a certificate verified by the client (trusted CA or pinning), with the client session authenticated separately. By default remote is OFF, to be activated on the DEVICE. The browser client variant requires a trusted domain/certificate or a gateway; a self-signed cert for a local IP does not meet the zero-config requirement, do not work around it with plaintext `ws`.

**MIME protocols:** textual UTF-8 JSON for control/status/input v1 (easier independent tests) with a `max_frame_size=1024 bytes` limit. If a benchmark shows parser costs, version **2** may introduce binary mouse reports (CBOR/packed) through negotiated capabilities; do not change v1 implicitly.

**Auth for the native client:** a token per paired device stored securely, not sent in a query string. **Auth for the browser client:** a secure session over HTTPS (HttpOnly+Secure+SameSite cookie), strict `Origin` checking and an anti-CSRF token in the agreed handshake scheme. Do not smuggle tokens in the WebSocket URL or in unencrypted telemetry.

### 6.2 Envelope for all messages

```json
{
  "v": 1,
  "type": "input",
  "seq": 203,
  "session_id": "c-815a",
  "lease_epoch": 7,
  "payload": {"kind": "mouse.move", "dx": 14, "dy": -5}
}
```

- `v`: integer literal 1. An unknown v → `ERR_VERSION` and closing the session in accordance with the policy.
- `type`: one from the allowlist; no silent ignoring of unknown names.
- `seq`: uint64 monotonic per session (in v1 a textual JSON number is safe ≤2^53−1); reset only with a new session.
- `session_id`: a non-secret server label; do not use it as an authorization token.
- `lease_epoch`: uint32 from `control.granted`, mandatory only for `input`.
- `payload`: typed, nowhere nested recursively.
- `server_timestamp_us`: only in status/ack in responses from the DEVICE; the client `timestamp` is diagnostic, it does not authorize input.

### 6.3 Commands and example payloads

| `type` / `payload.kind` | Requires control | Data | Response |
|---|---|---|---|
| `hello` | no, but TLS/session auth | `{ "client_name": "Desk1", "capabilities": ["hid_v1"] }` | `hello.ack` with version/caps |
| `status.get` | session auth | `{}` | `status.snapshot` |
| `control.request` | OPERATOR session | `{ "reason": "remote_support" }` | `control.pending` / `control.granted` or a refusal |
| `control.release` | the current owner | `{}` | `control.revoked` + release |
| `ping` | auth session | `{ "nonce": 101 }` | `pong` and lease renewal if owner |
| `input`/`mouse.move` | the current owner + epoch | signed int16 `dx`, `dy` | `input.ack` optionally |
| `input`/`mouse.button` | as above | `button:"left|right|middle", down:true|false` | as above |
| `input`/`mouse.wheel` | as above | integer `vertical`, `horizontal` | as above |
| `input`/`keyboard.key` | as above | integer `usage_id` (0..255), `down` bool | as above |
| `input`/`shortcut` | as above | allowlisted `command_id` | as above |
| `session.close` | auth session | `{}` | disconnect/release |

**Important:** `keyboard.key` enables ordinary HID presses (this is the assumption for a full remote keyboard), but it does **not** give the ability to issue arbitrary DEVICE system commands or a firmware update on its own.

### 6.4 Server → client messages (patterns)

```json
{"v":1,"type":"hello.ack","seq":1,"session_id":"c-815a","payload":{"protocol":1,"device_id":"kvm-xxxx","capabilities":["keyboard","mouse","shortcuts"],"host":"USB_MOUNTED"}}
{"v":1,"type":"control.pending","seq":2,"session_id":"c-815a","payload":{"approval_required":true,"expires_in_s":30}}
{"v":1,"type":"control.granted","seq":3,"session_id":"c-815a","lease_epoch":7,"payload":{"expires_in_ms":4000}}
{"v":1,"type":"control.revoked","seq":4,"session_id":"c-815a","payload":{"reason":"local_override","new_epoch":8}}
{"v":1,"type":"error","seq":5,"session_id":"c-815a","payload":{"code":"ERR_STALE_EPOCH","retryable":false}}
```

**Principle:** `seq` in the responses is its own server-to-client counter, not a copy of the request seq. If correlation is needed, the server adds `reply_to_seq` to the envelope. In the tests add cases with out-of-order `seq` and replays; WebSocket provides ordered transport, but the application can resend an old event after a reconnect, which is forbidden.

### 6.5 Security limits and validation

| Condition | Server action |
|---|---|
| Untrusted cert/TLS error | do not open WSS; no downgrade |
| Invalid auth | deny; delay/rate-limit; no token in the log |
| `Origin` not matching the web allowlist | reject handshake |
| `v != 1` | `ERR_VERSION`, disconnect or require an upgrade |
| >1024 B, malformed JSON, large depth | `ERR_BAD_FRAME`, count + close on abuse |
| `dx/dy` outside [-4096,4096] per event | `ERR_RANGE`, no hidden clipping |
| `usage_id` outside 0..255 / unknown key | `ERR_USAGE`, no HID |
| `seq <= last_seq` | `ERR_REPLAY`, no HID |
| `lease_epoch != owner.epoch` | `ERR_STALE_EPOCH`, no HID |
| `session_id` not matching the connection context | `ERR_SESSION`, no HID |
| no `USB_MOUNTED` | `ERR_HOST_OFFLINE`, no HID |
| heartbeat/session stale | `CONTROL_REVOKED`, release, zero trust for old events |
| >120 input msg/s in a rolling window | coalesce mouse movements, throttle, reject the non-idempotent excess, revoke on flood |
| `keyboard.key` already down | idempotent no-op, do not repeat to USB |
| `keyboard.key` up without down | no-op + metric; no uncontrolled reset of the whole keyboard |

**Note:** `control.request` may wait 30 s for physical confirmation, but it must not block render/USB; timeout → reject. A limit of max 1 active operator; the other CLIENTs are read-only after pairing is approved.

### 6.6 State snapshot (read-only)

```json
{
  "v": 1,
  "type": "status.snapshot",
  "seq": 16,
  "session_id": "c-815a",
  "payload": {
    "host": {"state":"USB_MOUNTED", "transport":"usb_hid"},
    "client": {"state":"CLIENT_CONNECTED", "role":"operator"},
    "control": {"owner":"REMOTE", "lease_epoch":7},
    "device": {"wifi":"CONNECTED", "maintenance":false, "fw":"1.0.0"}
  }
}
```

Status dumps do not contain tokens, the SSID password, the pairing secret, key events or raw touch data. `wifi:CONNECTED` is a separate state, it does not replace `CLIENT_CONNECTED`.

## 7. Error codes and UI reactions

| Code | Local UI | HID behavior |
|---|---|---|
| `ERR_HOST_OFFLINE` | `Host disconnected` | no ordinary input |
| `ERR_USB_SUSPENDED` | `Host suspended` | logical neutralization, no reports |
| `ERR_TOUCH_IO` | `Touch temporarily unavailable` | cancel gesture + release |
| `ERR_CLIENT_STALE` | `Client disconnected` | revoke remote + release |
| `ERR_CONTROL_DENIED` | `Control request denied` | no remote input |
| `ERR_PAIRING_EXPIRED` | `Pairing expired` | no ownership change |
| `ERR_STALE_EPOCH` | usually diagnostics only | reject the event, do not generate HID |
| `ERR_HID_QUEUE_OVERFLOW` | `Input temporarily paused` | critical release, stay in a safe state |
| `ERR_OTA_IN_PROGRESS` | `Updating…` | HID blocked until the health-check |

## 8. Hard test sequences (normative vectors)

### VEC-001: One finger tap

```text
0ms   [p1(400,200)]
70ms  [p1(402,201)]
130ms []
Expected: exactly LEFT_DOWN, LEFT_UP; NO MOVE, NO RIGHT, NO SCROLL
```

### VEC-002: Two-finger scroll, staggered arrival

```text
0ms   [p1(200,180)]
45ms  [p1(200,180), p9(400,190)]
75ms  [p1(200,201), p9(400,211)]
95ms  [p1(200,225), p9(400,235)]
140ms []
Expected: WHEEL(+/- per settings), no pointer MOVE, no click
```

### VEC-003: Track IDs swap array order

```text
0ms   [(id2,x100,y100), (id9,x300,y100)]
15ms  [(id9,x301,y110), (id2,x101,y110)]
30ms  [(id2,x101,y125), (id9,x301,y125)]
Expected: coherent centroid scroll, no 200px jump
```

### VEC-004: Transition two fingers then one

```text
0..100ms TWO_SCROLLING; 120ms only id2 remains; 180ms []
Expected: no extra LEFT_CLICK on last-finger release
```

### VEC-005: Disconnect during Ctrl DOWN

```text
host=mounted; owner=remote(epoch=7)
input(seq=1,usage=0xE0,down=true) -> report Ctrl DOWN
session timeouts -> epoch=8; RELEASE_ALL -> report neutral Ctrl UP
input(seq=2,epoch=7,usage=0x04,down=true) -> ERR_STALE_EPOCH
Expected: no output 'Ctrl+A', no stuck Ctrl
```

### VEC-006: Mode switch under drag lock

```text
Local Mouse->Drag Lock ON -> 1 move -> switch Keyboard
Expected: exactly one LEFT DOWN, zero or more MOVE with LEFT bit, LEFT UP before screen swap completes; no pressed flags in new mode
```

### VEC-007: Saturated queue

```text
Inject 300 pointer moves + key Ctrl DOWN + release_all + key Ctrl UP
Expected: pointer deltas coalesced; release_all cannot be dropped; final ledger empty; overflow counted
```

### VEC-008: Host unplugged and reconnected

```text
active Ctrl DOWN; USB umount -> ledger zero; USB mount -> neutral report only
Expected: no replayed Ctrl DOWN, no phantom shortcut
```

### VEC-009: Local takeover while remote dragging

```text
REMOTE LEFT DOWN, REMOTE MOVE; LOCAL touch input
Expected: REMOTE revoke + LEFT UP, then local input; no REMOTE post-override events; epoch increment
```

## 9. Compatibility rules for API changes

1. A change to the `kvm_touch_sample_t` format requires a new unit test and an update of `touch_input` and `gesture_engine` in the same PR.
2. A change to the report descriptor / wire protocol does not pass as a "minor UI PR"; it requires an ADR, a HOST compatibility matrix and a USB enumeration test.
3. Every new remote event requires: a whitelist, server-side validation, a threat-model, an NFR budget, a fuzz test and a `no USB without owner` test.
4. Do not add a new third HID source without updating `CONTROL_OWNER` and the concurrency tests.
5. `RELEASE_ALL` must not be extended with options that allow skipping the release of modifiers / buttons.
