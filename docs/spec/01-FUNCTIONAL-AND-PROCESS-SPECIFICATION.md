# 01 — Functional and process specification

**Project:** KVM Essential · **version:** 1.0.0 · **status:** normative · **resolution:** 800 × 480 · **platform:** Waveshare ESP32-S3-Touch-LCD-7

## 1. Product purpose and boundaries

The product is a **local input surface** for the HOST computer: the 7-inch panel provides a touchpad, a full on-screen keyboard and system shortcuts. Over the USB cable the ESP32 emulates a standard keyboard and mouse. An optional remote CLIENT session allows an authorized program to request the same HID operations over the network. The display does **not** show the HOST image — it is a controller, not a monitor. Do not call information about the cable or Wi-Fi "KVM connected" if there is no real operational state.

### 1.1 Glossary

| Term | Unambiguous meaning |
|---|---|
| `DEVICE` | Our board with the display and the firmware |
| `HOST` | The computer that physically receives the **USB HID device** reports from the DEVICE |
| `CLIENT` | A paired remote application that connects securely to the DEVICE over the network; not to be confused with the USB host |
| `LOCAL` | The person touching the physical LCD |
| `CONTROL_OWNER` | The source whose reports may reach the HOST at a given moment: `LOCAL`, `REMOTE(sessionId)`, `NONE` |
| `MOUNTED` | USB HID configured by the HOST; it does not mean that the OS accepted every particular key |
| `REMOTE_CONNECTED` | An authenticated, live CLIENT session; it does not yet mean holding control |
| `RELEASE_ALL` | Emptying the entire state of held keys and buttons in the arbiter/HID; sending neutral reports if USB is available |

### 1.2 Goals and non-goals

**MUST goals:** HID reliability, no accidental actions, predictable multitouch, fast access to functions without learning menus, legible information about real connections, offline operation, secure remote access, resilience to a restart and to the loss of the cable.

**Out of scope for v1:** HDMI/DP capture, RDP/VNC/video streaming, USB host for a connected physical keyboard, simultaneous BLE HID + USB HID emulation, automatic macros/scripting arriving from the Internet, a multi-host KVM switch, arbitrary code executed from the panel, an on-screen keyboard with IME/Unicode, the screen as a second monitor.

## 2. UX invariants — binding in all modes

- **FR-001 MUST:** only one large work area at a time: `MOUSE`, `KEYBOARD` or `SHORTCUTS`; do not cram two main modes onto a single screen.
- **FR-002 MUST:** landscape orientation 800 × 480 without scrolling of the interface, without scaling elements below the permitted touch targets, without covering the keyboard with a pop-up.
- **FR-003 MUST:** the top bar permanently presents `Mouse / Keyboard / Shortcuts`, `Host` and `Client`. Minimum touch target size 44 × 44 px; ≥48 px preferred. Touching an indicator opens the details, it does not artificially change the status.
- **FR-004 MUST:** on first boot the default view is `MOUSE`; the last view may be stored as a preference, but a faulty NVS setting does not block startup.
- **FR-005 MUST:** the status does not depend on color alone; apart from the dots it shows the text `Active / Suspended / Disconnected` and `Connected / Paired / Disconnected` in the details.
- **FR-006 MUST:** during an update, a USB error and a loss of control permission the device does not attempt to execute HID requests; the reason is legible on the screen.
- **FR-007 MUST:** switching the screen with a single tap; no gestures required for navigation between modes. Touchpad gestures work only on the touchpad surface.
- **FR-008 SHOULD:** a simplified, calm appearance on a light background; the visual dominant is the user's task, not decorations/animations. Colors must not act as the only signaling.
- **FR-009 MUST:** no simulated host cursor on the DEVICE surface. A discreet indication of the detected touch may be shown for diagnostics, disabled by default.

### 2.1 Reference geometry (pixels, not an adaptive CSS viewport)

```text
screen: [0,0] → [799,479]; landscape; topbar y=0..64
MOUSE:    touch surface x=16..783, y=70..401;
          footer/controls x=16..783, y=409..471
KEYBOARD: content x=16..783, y=70..470;
          6 rows + utility, without vertical scroll
SHORTCUTS:content x=16..783, y=70..470;
          grid 3 columns × 2 rows
```

Do not take these boundaries as absolute hitboxes without taking roundings and margins into account. Hit-testing is based on the real LVGL layout, not on a screenshot. For the keyboard the implementer is to generate a geometry table and the test `no_overlap && all_within_bounds && min_hitbox>=44`, and when it does not work out, change the proportions and document the changes.

## 3. MOUSE screen — gestures and meaning

### 3.1 Gestures: precedence order (MUST)

Touches are classified by **one** state machine. Never both `LEFT_CLICK` and `SCROLL` for the same sequence. No click should be emitted at the start of a gesture until the classification has determined it.

| ID | Gesture / entry condition | HOST action | Notes |
|---|---|---|---|
| FR-101 | Movement of 1 finger, segment >6 px | relative `pointer_move(dx,dy)` | no "tap" at the end of the movement |
| FR-102 | 1 finger, time ≤220 ms, movement ≤8 px | 1 × left click (DOWN→UP) | do not send on a two-finger contact |
| FR-103 | 1 finger, time ≥550 ms, movement ≤10 px | 1 × right click | emission after the threshold is exceeded; then **nothing** on UP |
| FR-104 | 2 fingers, the second joins within ≤100 ms, both ≤10 px, both UP ≤250 ms | 1 × right click | without a preceding left click |
| FR-105 | 2 fingers, the centroid moves >10 px | `mouse_wheel` on the Y/X axis | blocks the 1- and 2-finger click; no pointer_move |
| FR-106 | 1 finger: tap, next contact within ≤300 ms, movement of the second >8 px | second contact: LEFT DOWN→movement→LEFT UP | standard `double-tap-and-drag`; the first tap may have sent a click |
| FR-107 | `Drag lock` ON + movement of 1 finger | holding LEFT DOWN between gestures | a visible `Release` button and an inactivity time |
| FR-108 | Explicit `Right-click` | one right click | works without multitouch |
| FR-109 | After a change to 3+ fingers | **no** clicks; cancel the active classification | 3-finger extensions only after an ADR decision |
| FR-110 | The touch leaves the surface / navigation / disconnect | end the gesture, `RELEASE_ALL` according to the contract | rule out a "stuck left button" |

**Initial thresholds, not hardware facts:** the values 6/8/10 px, 100/220/250/300/550 ms are starting UX parameters. They may be tuned after HIL only within an ADR with test results; do not copy them from the HTML mockup as guaranteed values. A change exceeding 20% requires an update of the contract tests.

### 3.2 Ambiguous cases (MUST)

1. A transition from 1 to 2 fingers **before the gesture ends** cancels the pending 1-finger tap and 1-finger long-press. It does not emit a click retroactively.
2. Two fingers may arrive in two consecutive GT911 reads; if the interval is ≤100 ms, recognize a two-finger contact. Preserve the `track_id` instead of treating "the first finger" as `coords[0]`.
3. An additional finger during dragging: if LEFT is already DOWN, safely end the hold (UP), cancel the gesture; do not silently convert it into scrolling.
4. After `SCROLL` a return from 2 to 1 finger does not send a tap/pointer from the same sequence; a complete `touch_count=0` is required.
5. Loss of `TP_IRQ`, an I²C timeout, an unusual `track_id` jump, a sample gap >120 ms: cancel the sequence and release the buttons; do not "guess" a cursor jump.
6. On a fast transition to the KEYBOARD view do not emit a touchpad click at the place where a key is now.

### 3.3 Relative mouse and scrolling

- **FR-111 MUST:** the mouse speed is computed from the delta of consecutive samples of **the same finger**; the axes are scaled independently; the accumulator is reset on every new gesture.
- **FR-112 SHOULD:** the sensitivity curve as a monotonic, bounded function of speed, e.g. `gain = clamp(1 + 0.35*speed_norm, 0.7, 2.2)`; the `Slow / Normal / Fast` settings must be deterministic. A test for the absence of jerks during slow movement.
- **FR-113 MUST:** fractional deltas accumulated for sending; do not lose 0.3 px to every rounding; the report delta limit conforms to the HID format.
- **FR-114 MUST:** scroll from the centroid of 2 fingers; a fractional accumulator separately for X/Y; a change of direction does not switch the axis. Reasonable hysteresis for axis locking; a horizontal wheel only in a report that supports pan or in a mapping with a fallback.
- **FR-115 MUST:** the `drag lock` gesture displays a visible `Dragging` status and the ability to switch it off immediately, releases on cancellation, on loss of permission and after 10 s without activity (configurable; safety limit 60 s).
- **FR-116 MUST:** if multi-touch in the driver works only partially, the function is marked as a degradation and the `Scroll` mode is made explicitly available as a fallback — do not pretend that the two-finger gesture works.

## 4. KEYBOARD screen — HID semantics

- **FR-201 MUST:** QWERTY with a number row, Backspace, Enter, Tab, Esc, Space, CapsLock, Shift, Ctrl, Left Alt, Right Alt/AltGr, Meta/Win, Delete, Insert, Home, End, PageUp, PageDown, arrows, Backslash, punctuation. F1–F12 through the `F1–F12` layer.
- **FR-202 MUST:** every key is an unambiguous `USB HID Keyboard Usage` from the symbol table, NOT the entry of an ASCII code; Polish characters result from the HOST layout that is set (e.g. Polish Programmer). Do not promise Unicode from HID usages alone.
- **FR-203 MUST:** pressing an ordinary key sends DOWN and UP in that order. The activation length is chosen so that the HOST does not lose short taps (at least one effectively delivered DOWN report before UP).
- **FR-204 MUST:** `Shift`, `Ctrl`, `Alt`, `AltGr`, `Meta` after a single tap have a **one-shot** mode (for the next key), 2 taps within ≤300 ms lock them (`LOCKED`), a tap on a locked one releases it; visual state `OFF / ARMED / LOCKED`.
- **FR-205 MUST:** an ARM modifier only starts being pressed in HID atomically together with the actual key; after an ordinary key it returns to OFF. LOCKED remains logical for the following keys, and on USB it may be held between them. `RELEASE_ALL` clears physical and logical locks.
- **FR-206 MUST:** `CapsLock` is a normal toggling key on the HOST; the state indicator is based on the **USB HID LED output report** if available; when there is no synchronization show `Unknown`, do not guess.
- **FR-207 MUST:** `F1–F12` does not cover the main keyboard, it only swaps the utility row; `Fn` is a local UI layer, **not** a standard HID key sent to the HOST.
- **FR-208 MUST:** no auto-repeat generated in the firmware for ordinary keys in v1; host repeat after a longer hold depends on a real DOWN, and not on successive synthetic clicks. If the UX provides for a hold, a native `keyDown` after the touch and `keyUp` after the release; counteract stuck keys.
- **FR-209 MUST:** when a combination is sent from the SHORTCUTS view the modifiers of the KEYBOARD screen are not inherited; a shortcut has its own, atomic set of usages.
- **FR-210 SHOULD:** an optional `Windows/Linux` and `macOS` profile for the `Win/Meta/Cmd` labels, but the HID usage and the meaning on the host side are described explicitly. Version v1 defaults to `PC/Windows`.
- **FR-211 MUST:** five-point touch does not mean that multi-key typing with simultaneous fingers has to be implemented; the priority is deterministic single-target + modifier locks. Two keys at once only after geometry and HID tests.
- **FR-212 MUST:** no loss of the keyboard after an NVS failure, a network client error or a lack of Wi-Fi.

### 4.1 Keyboard — source of truth

The visual reference is `reference/kvm-essential-800x480.html`. For HID functions this specification is binding, not the prototype script. Every layout must have a test that detects all required keys and the corresponding `usage_id`. A test of use with a physical finger requires checking the extreme corners and the bottom-edge zone, not only a mouse simulation in a browser.

## 5. SHORTCUTS screen

| ID | UI label (default Windows profile) | HID chord | Action/limitations |
|---|---|---|---|
| FR-301 | `Ctrl + Alt + Del` | LCtrl + LAlt + Delete | Works as physical HID only if the host OS/firmware honors it; a remote RDP may intercept it differently |
| FR-302 | `Alt + Tab` | LAlt + Tab | Performs one step; for selecting among several windows the Alt lock on KEYBOARD can be used |
| FR-303 | `Task Manager` | LCtrl + LShift + Escape | Windows profile; do not promise it on every OS |
| FR-304 | `Show Desktop` | LGUI + D | Windows; description of the limitation |
| FR-305 | `Close Window` | LAlt + F4 | May close the active window; show the name of the action clearly |
| FR-306 | `Screenshot` | PrintScreen | The host may save to the clipboard, to a file or launch a tool — it depends on the configuration |

- **FR-307 MUST:** every operation sends a consistent, short chord: modifiers DOWN → key DOWN → key UP → modifiers UP (or atomic state reports in sequence, at least one effective IN report per phase).
- **FR-308 MUST:** do not execute a macro if there is no `HOST_MOUNTED` or no `CONTROL_OWNER` for local input. After a chord is interrupted, unconditionally attempt to neutralize the report.
- **FR-309 MUST:** do not make the ability to store arbitrary HID scripts available to an untrusted CLIENT in v1. Only an explicit allowlist of actions.
- **FR-310 SHOULD:** `Alt+F4` may be protected with a short confirmation or a long press if tests indicate a large number of accidental activations; do not require confirmation for all shortcuts.

## 6. Connections: independent semantics

### 6.1 HOST (USB HID)

| State | Real condition | View | HID |
|---|---|---|---|
| `USB_UNINITIALIZED` | driver/PHY not ready | `Initializing` | blocked |
| `USB_DETACHED` | no detectable connection / detached | `Disconnected` | blocked, logical release |
| `USB_ENUMERATING` | the host is enumerating | `Connecting` | blocked |
| `USB_MOUNTED` | USB configured, the stack is ready to report | `Active` | allowed after arbitration |
| `USB_SUSPENDED` | host USB suspend | `Suspended` | blocked; optional remote wake only in accordance with the USB specification and host permission |
| `USB_ERROR` | an error unrecoverable without a reinit | `Error` + details | blocked |

**FR-401 MUST:** VBUS alone, the USB cable or `Wi-Fi connected` do NOT mean `Host Active`. The UI must not confirm the actual reading of HID by the host application; it may show only the USB enumeration status. **FR-402 MUST:** after deenumeration, a reset of the physical and logical state; after remounting, a neutral initial state, without restoring old DOWNs.

### 6.2 CLIENT (network transport)

| State | Condition | View | Control |
|---|---|---|---|
| `CLIENT_DISABLED` | the function is disabled in the configuration | `Off` | no |
| `CLIENT_NETWORK_DOWN` | Wi-Fi/IP unavailable | `Offline` | no |
| `CLIENT_LISTENING` | the service is ready, no paired session | `Available` | no |
| `CLIENT_AUTHENTICATING` | handshake / pairing | `Connecting` | no |
| `CLIENT_CONNECTED` | transport + auth + heartbeat valid | `Connected` | **not necessarily yet** |
| `CLIENT_CONTROL_GRANTED` | a valid operator lease | `Controlling` | yes, until a LOCAL takeover |
| `CLIENT_STALE` | no heartbeat / protocol violation | `Reconnecting` or `Disconnected` | revoke immediately |

- **FR-403 MUST:** the main `Client connected` status requires an authenticated session, not Wi-Fi association alone.
- **FR-404 MUST:** a client that is connected but without control permission may see the status, but does not generate HID.
- **FR-405 MUST:** all `REMOTE_CONTROL → NONE/LOCAL` transitions, a loss of auth/lease/session trigger a release in the arbiter.
- **FR-406 MUST:** network access must not stall USB, touch or the GUI. Reconnect has jitter and backoff, not a busy loop.

### 6.3 Physical power limitation

If the DEVICE is powered **only from the HOST USB port**, switching off/disconnecting the HOST may also switch off the DEVICE: the screen then cannot show "HOST disconnected". **FR-407 MUST:** the requirement of a visible disconnected-HOST status when the HOST is completely switched off is met only with an independent, safe power supply for the DEVICE (an external supply/battery in accordance with the documentation). Do not combine power supplies/ports without checking the schematic and backfeed; the decision is in ADR-004.

## 7. User processes — the "happy path" and failure paths

### PROC-001: First boot, without Wi-Fi

1. The DEVICE starts up, initializes CH422G → LCD/GT911 → USB in device mode.
2. The GUI shows `MOUSE`, `Host Connecting`, `Client Off/Available` depending on the settings.
3. The HOST enumerates USB HID; `Host Active` only after mounted.
4. The user moves a finger and clicks, without an account, Wi-Fi or the Internet.
5. Outcome: a stable HOST cursor, no "left-behind" buttons.

**Alternative:** no HOST — the UI works, shows a message and does not send HID; with a separate power supply the settings can be used.

### PROC-002: Touch and click

1. The user touches the MOUSE surface; the parser records `track_id`, `x`, `y`, time.
2. The classifier decides between tap/move/long-press/2-finger.
3. The arbiter checks `USB_MOUNTED` and `LOCAL` ownership; the translator converts the gesture into USB reports.
4. The USB task sends them, the device may display a discreet touch response if it does not cause delays.
5. After up/timeout a transition to IDLE; the DOWN counter == 0.

### PROC-003: Dragging a window

1. The user switches on `Drag lock`, the UI unambiguously shows `Dragging`.
2. HID sends LEFT DOWN; finger movements — `MOUSE_MOVE` with the button held.
3. Touching `Drag lock` or `Release` again — LEFT UP.
4. Gesture interruption, a mode, HOST/CLIENT disconnection, OTA, watchdog recover — release.
5. **Do not send LEFT DOWN again** on every new movement if the lock is already active.

### PROC-004: Typing on the keyboard

1. The user enters `KEYBOARD`.
2. A tap on Ctrl sets the one-shot `ARMED`; a tap on C triggers Ctrl+C as one HID cycle.
3. `Ctrl` returns to OFF; the remaining modifiers are not inherited by accident.
4. Two quick taps on Shift → `LOCKED` and a distinct state; a tap on Shift unlocks it.
5. When the host disappears during a modifier, all states are cleared before re-enumeration.

### PROC-005: Administrative shortcut

1. The user selects SHORTCUTS and `Ctrl+Alt+Del`.
2. The system checks HOST readiness and LOCAL ownership.
3. HID executes ordered reports; it always transitions to a neutral report.
4. The UI shows a small `Sent` confirmation only when the reports were actually handed over to the USB stack; it does not claim that the OS performed the operation.

### PROC-006: Pairing a CLIENT

1. A trusted person physically opens `Settings → Remote → Pair client` on the DEVICE.
2. The DEVICE displays a one-time code / QR valid for a short time (SEC details), without a control port open without authentication.
3. The CLIENT negotiates **trusted TLS** and the `v=1` protocol; it passes authentication and receives minimal permissions.
4. The DEVICE shows the client name and its role. `Connected` means an auth+heartbeat session, **not** control.
5. The CLIENT sends `control.request`, LOCAL must confirm on the screen (by default every time, SECURITY decision).
6. After granting: `CONTROL_OWNER=REMOTE(sessionId)`, HID accepts approved events.

**Errors:** TLS not verified, an invalid code, 3 consecutive failed attempts, a timeout, no physical consent → refusal, without active control. Certificate handling in the browser must have an explicit deployment variant; see ADR-002.

### PROC-007: The local user interrupts remote work

1. `REMOTE` holds the lease. LOCAL starts a deliberate input action (tap/move/keyboard/shortcut).
2. The arbiter immediately: revoke REMOTE, `RELEASE_ALL`, transition to LOCAL, acceptance of the local action in a new epoch.
3. The CLIENT receives `control.revoked(reason=local_override)`; its subsequent events are rejected.
4. A takeover by REMOTE again requires a request and consent, not automatic reactivation.

### PROC-008: Loss of connectivity while Ctrl is held

1. REMOTE sent Ctrl DOWN.
2. The heartbeat expired or the session was closed.
3. The arbiter invalidates the lease, immediately calls `RELEASE_ALL`; if USB is online, it sends a neutral keyboard report. The logical state is empty regardless of the effectiveness of the transmission.
4. After a reconnect the old `seq`, epoch, lease are rejected. No automatic Ctrl DOWN.

### PROC-009: OTA / update

1. An approved update requires operator permission and a secure signature path.
2. The DEVICE announces the update; it enters maintenance, takes control away from the CLIENT, calls `RELEASE_ALL`, blocks HID input.
3. OTA writes the image to the inactive slot; the GUI may limit animations/refresh and show a simple state. Do not promise 60 FPS during flashing.
4. After signature verification and a restart, HEALTH-CHECK decides on confirming the new image or a rollback.
5. Power lost during OTA must not cause the loss of the working previous slot.

### PROC-010: Settings reset / recovery

1. A full reset only from local authorization and confirmation; a single remote network command is not enough.
2. It removes paired CLIENTs, Wi-Fi and session tokens, keeping the safe firmware/recovery.
3. Before the wipe, `RELEASE_ALL`, the UI informs about the consequences; sessions are disconnected.
4. Recovery after an NVS failure starts local touch + USB (provided the hardware works), it does not start an unauthorized remote service.

## 8. Settings (v1)

| ID | Setting | Default | Range / policy |
|---|---|---|---|
| FR-501 | Mouse sensitivity | Normal | Slow / Normal / Fast; NVS storage with a schema version |
| FR-502 | Natural scrolling | Off | inverts the wheel sign; without changing the pointer direction |
| FR-503 | Long press as right click | On | after it is switched off the explicit `Right-click` is still there |
| FR-504 | Two-finger scroll | On | `Scroll` fallback when GT911 fails multi |
| FR-505 | Screen brightness | factory | available only if the board driver supports PWM regulation; otherwise ON/OFF, without a fake slider |
| FR-506 | Remote enabled | Off on first boot | the owner activates it locally after configuring a trusted network |
| FR-507 | Local takeover | Always | no option to switch it off in v1 |
| FR-508 | Host keymap label | PC/Windows | the host layout remains on the OS side |
| FR-509 | Diagnostics overlay | Off | does not show passwords, tokens, raw keystrokes |

## 9. Failure behavior and accessibility limitations

- **FR-601 MUST:** no visually "green" connection if it is actually `suspended/offline`.
- **FR-602 MUST:** with damaged multi-touch the user retains the ability to right-click and to scroll through dedicated controls.
- **FR-603 MUST:** no hidden gestures for critical functions: cancel, release, pairing approve and the modes are available through a single legible button.
- **FR-604 MUST:** status summaries and toasts legible at low ambient contrast; a typical key font ≥19 px, status ≥13 px in the target render, labels in the preferred English language consistent with the mockup.
- **FR-605 MUST:** no infinite spinner when Wi-Fi is missing; the local UI does not depend on the network.
- **FR-606 MUST:** in case of a touch controller defect or an I²C hard fault, status and the possibility of recovery through the diagnostic UART / a restart; the device does not generate phantom HID.
- **FR-607 SHOULD:** after a long period without use reduce the brightness, but do not switch off USB HID or the network without an explicit policy; the first touch that wakes the device from blanking does not generate a click.
- **FR-608 MUST:** the UI does not require any fonts from macOS. A font with a license appropriate for firmware distribution must be used (e.g. a verified open-source one), with pre-rendered glyphs where needed.

## 10. NFR — qualitative project goals (to be confirmed on hardware)

| ID | Parameter | v1 goal / method |
|---|---|---|
| NFR-001 | Latency from touch sample → correct HID report queued (host USB mounted) | median ≤20 ms; p95 ≤45 ms, test conditions from `04` |
| NFR-002 | Loss of "DOWN" buttons after an owner change/disconnect | **0 stuck keys** in 10 000 HIL cycles |
| NFR-003 | GUI smoothness | ≥25 render FPS under full network/touch activity; goal 30; no RGB shift/flicker |
| NFR-004 | Navigation time between screens | visual response p95 ≤100 ms |
| NFR-005 | UI startup | ≤3.5 s from reset to interactive LOCAL on the target board, a target, not a guarantee |
| NFR-006 | HOST USB recovery after a reconnect | automatic re-enumeration; a test of 100 disconnections without manual flashing |
| NFR-007 | Reliability | 72 h soak test: no crash/heap monotonic leak/pseudo-input |
| NFR-008 | Network | every remote input with authorization; no plaintext transport in production |
| NFR-009 | Memory usage | without exhausting internal RAM; budgets in `02`, peak logged |
| NFR-010 | Performance independent of the Internet | LOCAL HID works on a completely disconnected network and after a failed pairing |

## 11. Conflict and interpretation rules

1. `RELEASE_ALL` and the safety locks have a higher priority than clicks, rendering and the CLIENT transport.
2. `LOCAL` is the owner with the highest priority; no network request may prevent a local override.
3. For the UI the specification may simplify the appearance of the mockup, but it is **not** allowed to worsen the ergonomics of the hitboxes or to remove mandatory functions.
4. Any uncertainty about "whether the key was pressed" means neutralization and an explicit error, never a "let us send it several times" non-idempotent click.
5. All `MUST` requirements must have an `AT-...` test in `04`, otherwise the feature is insufficiently described.

## 12. Traceability table (excerpt)

| Requirements | Tests | Task |
|---|---|---|
| FR-001..009 | AT-UI-01..06 | TASK-060 |
| FR-101..116 | AT-GEST-01..12 | TASK-040 |
| FR-201..212 | AT-KEY-01..10 | TASK-070 |
| FR-301..310 | AT-HID-05..09 | TASK-050, TASK-070 |
| FR-401..407 | AT-CONN-01..10 | TASK-030, TASK-080 |
| FR-501..509 | AT-UI-05, AT-SEC-08 | TASK-090 |
| FR-601..608 | AT-FAULT-01..10 | TASK-100 |
| NFR-001..010 | AT-PERF-01..09 | TASK-110 |
