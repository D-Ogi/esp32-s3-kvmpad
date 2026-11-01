# KVMpad — UI Core architecture on top of LVGL

**Document:** 07 · **Version:** 1.1.0 · **Date:** 2026-11-01  
**Status:** ACCEPTED AS REFERENCE / requires confirmation on hardware (M0/M5)  
**Device:** Waveshare ESP32-S3-Touch-LCD-7 · 800 × 480 px · RGB565 · GT911 (up to 5 points)  
**Scope:** only the architecture of presentation, interaction, navigation, UI updates and touch integration. It does not change the HID, USB, remote access and security contracts from documents 01–06.

## 0. Architectural decision — ADR-013

**We are building *KVMpad UI Core*, a thin application framework on top of open LVGL 9.6, not our own GUI engine.**

We use natively: the object tree, styles, Flex/Grid, fonts, animations, widgets, event handling and `lv_subject_t`/Observer. We only add the missing application elements: a typed state model, a navigation and overlay controller, a set of product components, an input adapter from GT911, an intent layer and a deterministic view lifecycle.

**Visual tooling (optional):** EEZ Studio in LVGL mode, *without EEZ Flow*, exclusively for generating the view layer. The generated code is isolated and automatically reproducible. A successful EEZ Studio build with the pinned LVGL 9.6 is not an assumption — it is the TASK-061 gate.

**ESP-IDF renderer:** as the M0 candidate `espressif/esp_lvgl_adapter` 0.7.4 (2026-10-28, LVGL 9.6 support, RGB) instead of `esp_lvgl_port`; if an actual build/HIL reveals a regression, a pinned `esp_lvgl_port` is acceptable after an ADR/comparison. **Do not mix adapters or flush stacks for the same LCD.**

**We do not use:** React/Flutter, a JS engine on the ESP32, our own layout parser, our own CSS, DOM emulation, our own style system unrelated to LVGL, an XML runtime as a dependency. HTML/CSS remains *a UX prototype and a source of geometry*, not code on the MCU.

### Why not a ready-made complete framework?

- **LVGL Pro:** a very good XML editor + preview + C export, but a separate Pro license and an XML runtime license; we do not want to make public firmware dependent on the cost / license terms of a tool. LVGL Open itself is MIT.
- **EEZ Studio:** GPL-3.0 applies to the program; according to the authors the generated code has a separate status; without EEZ Flow there is no obligation to include its runtime. It requires verifying the generator version and a deterministic export. *This does not automatically mean that the generated code is GPL.*
- **SquareLine:** a commercial tool for visual C export; it does not provide us with a source of truth about the device state and safe HID arbitration.
- **Our own full framework:** it multiplies bugs and the testing overhead on a small MCU. We need an *application shell*, not a replacement for LVGL.

## 1. Normative architecture and responsibility boundaries

```text
                         +----------------------------------+
                         |  USB / WIFI / CLIENT / HID CORE  |
                         |  source of truth, owner, epoch    |
                         +----------------+-----------------+
                                          |
                               typed StateSnapshot/Delta
                                          |
                                 UI mailbox (bounded)
                                          |
           ONLY UI CONTEXT (LVGL adapter task, with lock if required)
        +---------------------------------v------------------------+
        |                       KVMpad UI Core                      |
        |  UI Store -> LVGL Subjects -> Reusable Components        |
        |  Router + Overlay Stack -> Mouse / Keyboard / Shortcuts |
        |  Theme Tokens + Focus/Accessibility + Diagnostics       |
        +---------------------------+------------------------------+
                                    |
                             UI Action / UI Intent
                                    |
                  typed command queue, no LVGL types outside UI
                                    |
                         APP / InputArbiter / HID Core
                                    |
                               TinyUSB -> HOST

 GT911 (one hardware reader)
   |
   +-> TouchFrame / track_id manager / per-contact capture
         |
         +-> UI touch bridge (LVGL indev; chrome/buttons/modal)
         |
         +-> dedicated raw GestureFSM (trackpad contact region)
                   |
                   +-> HID Intent -> InputArbiter (NOT UI shortcut)
```

**Key invariant:** GT911 is **read only once by a single module**. Neither LVGL nor GestureFSM performs independent `esp_lcd_touch_read_data`/I²C reads. Both layers take the same stable, numbered `TouchFrame`.

**Second invariant:** the interface never sends a USB report. A view callback creates only a typed `UiAction` or `HidIntent`, which goes to the existing arbiter/scheduler from specification 03.

**Third invariant:** HID functionality works locally while the UI is stopped, if the hardware is working. A router/animation failure must not "hold" pressed buttons.

## 2. Layers and repository

```text
components/
  ui_platform/             # lvgl/esp_lcd integration; lock, render metrics, display init
  ui_core/                 # typed state mailbox, router, overlay, lifecycle, actions
    include/ui_core.h
    ui_runtime.c
    ui_store.c
    ui_router.c
    ui_overlay.c
    ui_actions.c
  ui_theme/                # tokens, fonts, LVGL styles, colors, spacing, touch targets
    include/ui_tokens.h
    ui_theme.c
  ui_components/           # ready-made LVGL components without USB/network logic
    ui_status_item.c
    ui_topbar.c
    ui_action_key.c
    ui_modal.c
    ui_toast.c
  ui_views/                # composition + view-specific local state only
    ui_view_mouse.c
    ui_view_keyboard.c
    ui_view_shortcuts.c
    ui_view_settings.c
  ui_generated/            # [OPTIONAL] output EEZ; NEVER EDIT manually
  ui_touch_bridge/         # one GT911 sample → capture → LVGL indev / GestureFSM
  touch_input/             # existing GT911 source
  gesture_engine/          # existing pure gesture FSM
  hid_core/                # existing HID, source-of-truth button ledger/ownership
simulator/
  lvgl_sdl/                # real LVGL 800x480, fake USB/NET state publishers
  fixtures/                # deterministic state + touch playback
  golden/                  # screenshots from native LVGL, not HTML render
reference/
  kvm-essential-800x480.html
  ...
```

**Source of truth:** the HOST/CLIENT/ownership status and the state of keys/buttons is maintained by `hid_core`/network core; `ui_store` creates only a *presentation projection* and local states unrelated to security, e.g. the visible screen or an open dialog.

**Dependency rule:** `ui_views -> ui_components -> ui_theme`, `ui_views -> ui_core`; `ui_core` does not import `tinyusb.h` or network transport headers. The domain core does not import `lvgl.h`. `ui_touch_bridge` has no access to the USB stack.

## 3. Contracts: actions, snapshot, state publication

The definitions below are **design contracts**, not confirmed headers of the existing firmware. Their final name and compliance is approved by TASK-062; competing types must not be created in parallel in several components.

```c
/* ui_core/include/ui_contracts.h - project API (design draft) */
typedef enum {
    UI_VIEW_MOUSE = 0,
    UI_VIEW_KEYBOARD,
    UI_VIEW_SHORTCUTS,
    UI_VIEW_SETTINGS
} ui_view_id_t;

typedef enum {
    UI_ACT_NAVIGATE,
    UI_ACT_OPEN_HOST_DETAILS,
    UI_ACT_OPEN_CLIENT_DETAILS,
    UI_ACT_OPEN_SETTINGS,
    UI_ACT_DISMISS_OVERLAY,
    UI_ACT_KEY_DOWN,
    UI_ACT_KEY_UP,
    UI_ACT_SHORTCUT_REQUEST,
    UI_ACT_MOUSE_BUTTON_DOWN,
    UI_ACT_MOUSE_BUTTON_UP,
    UI_ACT_MODIFIER_TOGGLE,
    UI_ACT_DRAG_LOCK_REQUEST
} ui_action_type_t;

typedef struct {
    ui_action_type_t type;
    uint32_t request_id;
    uint64_t monotonic_us;
    uint16_t control_id;      /* UI-local stable ID, resolved through mapping */
    uint16_t value;           /* key usage/button/chord/view as relevant */
} ui_action_t;

typedef struct {
    uint32_t version;
    uint8_t host_usb_state;  /* enum from stable domain contract */
    uint8_t client_state;
    uint8_t owner_kind;
    uint32_t owner_epoch;
    bool wifi_connected;
    bool touch_healthy;
    bool drag_lock_active;   /* authoritative HID state, not GUI wish */
    uint8_t modifiers_down;  /* authoritative keyboard ledger projection */
    bool caps_led_known;
    bool caps_led_active;
    uint16_t error_code;
} ui_state_snapshot_t;
```

**Security note:** `request_id` identifies a command, not the host state. The mere fact of a UI click does not grant the right to display `Host Active` or `Connected`. At most a temporary "Requesting..." may be shown until an authoritative event is obtained.

**Handling:**

1. The domain service publishes `ui_state_snapshot_t`/delta to a bounded mailbox (without pointers to data with a variable lifetime).
2. The UI adapter task takes the events; during a single UI tick it applies the newest snapshot and updates the LVGL Subjects.
3. Views bound to Subjects refresh text/states automatically; **they do not poll Wi-Fi or TinyUSB in the renderer**.
4. `UI_ACT_*` goes to the existing typed action gateway; HID actions are approved by the InputArbiter.
5. A refusal of an action or an error updates the state and displays readable information; they do not change the status optimistically.

**Concurrency flow:** `lv_subject_set_*`, `lv_obj_*` and the creation/destruction of screens are performed only by the task that owns LVGL or under the proper lock of the selected adapter; they are not called by a network callback, a TinyUSB ISR or the touch sampler.

**Coalescing:** a repeated RSSI / FPS counter may be replaced with the last value; critical owner/USB/maintenance changes require reliable state synchronization. After a queue error the UI restores the full snapshot instead of deriving the state from lost events. `KEY_UP` and `RELEASE_ALL` must not be removed in the HID queue — it is separate from the UI.

## 4. We use the *native* Observer instead of a second Redux/React

In LVGL 9.6 there are `lv_subject_t`, `lv_subject_init_int`, `lv_subject_set_int`, bindings to widgets and automatic detachment of observers attached to objects that are being destroyed.

```c
/* Pseudo-example: only in the UI task, after verifying the API in pinned headers */
static lv_subject_t host_usb_subject;

void ui_model_init(void) {
    lv_subject_init_int(&host_usb_subject, 0);  // 0 = disconnected
}

void ui_apply_snapshot(const ui_state_snapshot_t *s) {
    lv_subject_set_int(&host_usb_subject, (int32_t)s->host_usb_state);
}
```

- `Subjects` are a *presentation* of the state, not the state of the whole firmware.
- Views subscribe through objects, so that the lifecycle is automatically tied to the life of the widget.
- We do not implement our own observation system with a dependency graph.
- We do not store pointers to LVGL in events that cross task boundaries.

## 5. Router: one activity at a time

**The root shell always exists:** the top status/navigation bar + `content_slot` + `overlay_layer` + `toast_layer`.

**Views (primary):** `MOUSE`, `KEYBOARD`, `SHORTCUTS`. **Settings:** a modal/subview opened deliberately. **Exceptional states:** independent, non-blocking notifications and a critical overlay only when security requires it.

Minimal router:

```c
typedef struct ui_view_descriptor {
    ui_view_id_t id;
    void (*create)(lv_obj_t *slot);   /* called once or on mount */
    void (*on_enter)(void);
    void (*on_leave)(void);           /* cancels local gesture and invokes domain release */
    void (*destroy)(void);
} ui_view_descriptor_t;

bool ui_router_show(ui_view_id_t target);      /* UI context only */
bool ui_overlay_show(uint16_t overlay_id);     /* max stack policy, no nested modal leaks */
void ui_overlay_dismiss(void);
```

**Atomic mode switch:** `input_cancel_for_view_change` → HID state release and acknowledgment/policy → invalidate old touch ownership → change visible content → publish `active_view`. It is not allowed to switch the view and leave a key/button in the DOWN state. If the HID release is asynchronous, the router must not assume that it succeeded; it passes the request and receives a status, and security is ensured independently by the HID core.

**Cache:** at most three main views in memory (created lazily or after start-up). The strategy is selected after `lv_mem_monitor`, internal heap measurements and stress tests; avoid allocation on every tap on the navigation.

**Animations:** by default no transition animation (0 ms), optionally a <=120 ms fade, if HIL proves there are no delays/lost gestures. LVGL blocks inputs during the standard screen load animation: therefore do not use `lv_screen_load_anim()` as the default way of switching interactive modes.

## 6. One GT911, two kinds of interaction

`touch_input` reads the frame only once, maintains `track_id`, monotonic time, coordinates in 800×480 and the error state. `ui_touch_bridge` classifies the contact at the moment of **TOUCH_DOWN**:

- `CHROME`: topbar, navigation, status/overlay, keyboard, buttons — to the LVGL indev.
- `PAD`: the large touchpad rectangle, only in MOUSE mode — to the pure `GestureFSM`, without generating artificial widget clicks.
- `BLOCKED`: outside the interactive geometry / a blocking modal / a 3–5 finger diagnosis — no HID.

**Per-contact capture:** a single `track_id` keeps its assigned target until the end of the contact, even if the finger travels over the edge. After a second finger is added in `PAD`, the FSM changes the mode to multi-touch, cancelling the pending single tap before the click is confirmed. Two fingers covering the PAD and a button require an explicit rule: mouse movement must not be combined with an accidental UI click; a cancel or a capture by the modal applies, depending on the area where the contact started.

`ui_touch_bridge` maintains a deterministic pointer-state queue to a custom LVGL indev, it does not perform a **second** I²C read. If the adapter integrates the touch device automatically, disable its duplicating read and attach our own feed or document the shared producer path.

Mouse gestures are independent of LVGL widgets:

- 1-finger movement: relative dx/dy,
- tap: a click only after the gesture qualification has finished,
- 2-finger tap: right click (the conflict with scrolling resolved by the FSM),
- 2-finger slide: scroll, without a tap,
- doubletap-drag / drag lock: a state machine with a guaranteed `RELEASE_ALL`,
- unsupported 3–5 fingers: no phantom HID; diagnostics may record the *number*, not a trace identifying the user.

## 7. Product components instead of a universal design system

We build only 6–8 genuinely shared elements:

| Component | Responsibility | Not allowed |
|---|---|---|
| `UiTopBar` | navigation + two independent HOST/CLIENT statuses | computing the real connection |
| `UiStatusItem` | text, color, state and details | "Connected" from DHCP alone |
| `UiActionKey` | pressed/released feedback and touch target | direct `tud_hid_*` |
| `UiKeyboardGrid` | tabular layout, US/PL and Fn layers | ASCII-as-HID, creating a keymap from the key text |
| `UiTouchSurface` | hit region and accessibility feedback | a standalone multitouch parser |
| `UiModal` | controlled overlay, focus trap and dismiss | modifying the HID owner |
| `UiToast` | short events, queue and fade-out | blocking the touchpad while in use |
| `UiConnectionDetails` | reading HOST/CLIENT diagnostics | revealing tokens/secrets |

**Design tokens:** colors, typography, spacing 4/8/12/16, corner radii, divider lines, disabled/pressed/active state, touch-min 44×44 px and preferred targets ≥48px. A single owner, `ui_theme`, creates the shared LVGL styles *once* at start-up. Do not copy 50 magic values across the views.

**Variants:** `status` (`ok`,`warning`,`offline`,`unknown`), `key` (`normal`,`modifier`,`destructive`), `button` (`primary`,`secondary`,`quiet`). Avoid 30 parameters in a single generic factory.

**800 × 480:** the existing mockup is a reference for proportions, not a render target. Hitboxes are required and there must be no hidden clickable elements. The Keyboard mode takes most of the height; Mouse has one dominant PAD. The topbar must not grow depending on the length of the IP/SSID. Do not place passwords, tokens or session identifiers on the topbar.

## 8. Designing in EEZ Studio — a very precise boundary

**Permitted spike (TASK-061):** export one `Mouse` screen + `UiStatusItem` from EEZ Studio *LVGL, no Flow*. Check: compatibility with the pinned LVGL 9.6, the size of the generated code and the heap, the license of the generated files, the possibility of a painless re-export and the integration through `ui_views`.

If it passes the gate:

- `design/kvm.eez-project` is the source of the UI visual; 
- `components/ui_generated/` is **generated in its entirety** and read-only for implementers;
- `ui_core` and `ui_bindings` remain hand-written and are not overwritten by generators;
- the editor does not decide about owner/USB/gestures, it does not generate HID business logic;
- the generator has a frozen version, the build export is run as a deterministic step, a diff of the generated code on CI;
- if the re-export generates non-reproducible artifacts or requires an unacceptable runtime, *we reject the tool*, leaving our own UI composition in C.

**We do not enable EEZ Flow** in v1: it generates an additional runtime and moves the application logic outside the contracts in C. The decision does not mean that Flow is bad — only that for HID security and small, well-bounded tasks duplicated logic is unfavorable.

**Alternative in case of a failed spike:** `ui_components` and `ui_views` created in pure C/LVGL. Prefer factory functions of 10–50 lines and descriptor tables, not our own DSL with XML/JSON.

## 9. ESP32-S3 rendering and resources

- RGB565 full frame: `800 × 480 × 2 = 768 000 B = 750 KiB`.
- Two frames: `1 536 000 B ≈ 1.465 MiB` (without working buffers), three: `2 304 000 B ≈ 2.197 MiB`.
- Frame memory/large assets: PSRAM after performance approval; small mutexes/queues/HID structures and ISR areas: internal RAM in accordance with the driver requirements.
- By default simple backgrounds, limited full-screen invalidation, no expensive blur/shadows requiring repeated rasterization; measure the real difference with Wi-Fi + HID + flash.
- One GUI owner; LVGL is not thread-safe. `esp_lv_adapter_lock` only where it is required; do not cover I²C, TLS, USB or NVS operations with it.
- Separate `FPS render` from `scanout Hz`; measure CPU load and high-water, per-component memory, p95 HID.
- Prefer a constant memory cost on mode changes; if 10 000 transitions increase the live heap or the number of observers — release blocker.

## 10. UI testing at three levels

**L1 — SDL desktop native LVGL 9.6:** real widgets and renderer, a fake hardware adapter. Fixture/snapshot for MOUSE, KEYBOARD, SHORTCUTS, HOST lost, CLIENT lost, pairing, owner remote/local, maintenance and 2 errors. Golden 800×480. Tests of hitboxes, hidden elements, accessibility and route transitions.

**L2 — integration/hardware abstraction:** `UiAction` events, fake ACK/reject from the InputArbiter, 1→2 fingers and 2→1 in the middle of a gesture; a view change with the KEY_DOWN/drag state, queues and a status snapshot after an overflow.

**L3 — HIL Waveshare + HOST:** GT911 1/2/5, reaction to USB suspend/plug, behavior under Wi-Fi load, a fresh start, NVS flash write, OTA maintenance, 10 000 navigations, 72h soak.

**Minimum of additional tests:**

| ID | Check | Threshold / result |
|---|---|---|
| AT-UIFW-01 | SDL snapshot of the three 800x480 screens | no clipping/overlap |
| AT-UIFW-02 | 10k screen switches | no growth of live objects/observers |
| AT-UIFW-03 | 100 erroneous statuses/owner changes | the UI always converges to the domain snapshot |
| AT-UIFW-04 | touchpad + tab bar at the same time | no phantom click and no stuck button |
| AT-UIFW-05 | disable the UI worker while HID is active | HID safety still works, release safe |
| AT-UIFW-06 | EEZ re-export (if used) | zero edits of generated, a clean build possible |
| AT-UIFW-07 | lock/task stress with Wi-Fi + USB | no deadlock, p95 still within NFR |
| AT-UIFW-08 | forced mode switch during KEY_DOWN | release/cleared state; no `UP` loss |
| AT-UIFW-09 | lv_subject lifetime on destroy | no use-after-free/observers leak |
| AT-UIFW-10 | 1000 modal/tap repetitions | focus does not block the pad after closing |

## 11. Tasks — extension of TASK-060/070

- **TASK-060A — LVGL 9.6 + adapter spike:** separate binaries for `esp_lvgl_adapter` and the fallback `esp_lvgl_port`; a benchmark without USB traffic, then under load. HIL evidence; one adapter selected in ADR-013.
- **TASK-060B — UI contracts/store/subjects:** type headers, a bounded mailbox, applying the snapshot in a single task, unit tests.
- **TASK-060C — Router/overlay lifecycle:** cache views, Observer cleanup, navigation without stuck; SDL unit.
- **TASK-060D — UI theme/components:** a set of tokens and 6 shared components, no dependencies on USB.
- **TASK-060E — GT911 touch router:** single reader, track_id capture, LVGL UI events/gesture stream without contention.
- **TASK-061 — EEZ editor comparison (optional spike):** one screen, export, license, PR convenience, performance; a written ACCEPT/REJECT decision.
- **TASK-070A — Mouse UI + gesture integration**.
- **TASK-070B — Keyboard keymap UI + exact HID intents**.
- **TASK-070C — Shortcuts/settings/status binding**.
- **TASK-070D — SDL snapshots + HIL acceptance**.

**Small-task rule:** do not start with "implement the framework". Every task gets *an exact list of files to modify*, the input contract `UiAction`/`UiState`, the existing `dependencies.lock` and 2–5 tests. Review independently after every task. LVGL must not be replaced with another UI without an ADR.

## 12. Definition of completion of the framework

The UI Core layer is ready when:

1. The three screens are created using the same components/theme (not 3 copies).
2. Every status comes from a single source of truth and unknown/offline is visible without guessing.
3. The `ui_core` + `ui_views` code can be run under LVGL SDL without Waveshare, TinyUSB and Wi-Fi.
4. In the firmware **one path** reads GT911; multi-touch still works.
5. The UI refreshes without races with FreeRTOS (LVGL owner thread only).
6. No UI callback emits HID directly.
7. Navigation with Ctrl held/with dragging ends with a correct release.
8. The `AT-UIFW-01..10` tests pass at the appropriate level; no fictitious PASS_HW.
9. The package is buildable from pinned versions (IDF/adapter/LVGL) and a documented toolchain.
10. The user can switch modes with a single touch, and the p95 of navigation meets NFR-004.

## 13. Sources as of 2026-11-01

- LVGL 9.6 release (2026-09-16): https://github.com/lvgl/lvgl/releases/tag/v9.6.0
- Native Subjects/Observer: https://lvgl.io/docs/open/api/public/core/lv_observer_h
- Screen lifecycle/animation: https://lvgl.io/docs/open/common-widget-features/screens
- Espressif `esp_lvgl_adapter`: https://docs.espressif.com/projects/esp-iot-solution/en/latest/display/tools/esp_lvgl_adapter.html
- `esp_lvgl_adapter` 0.7.4 changelog: https://components.espressif.com/components/espressif/esp_lvgl_adapter/versions/0.7.4/changelog
- ESP LVGL thread-safety: https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/peripherals/lcd.html
- EEZ Studio + rights to the generated code: https://github.com/eez-open/studio
- EEZ Flow integration: https://github.com/eez-open/studio/wiki/FAQ
- LVGL Open MIT: https://lvgl.io/docs/open/introduction/license
- LVGL Pro XML runtime licensing: https://lvgl.io/blog/announcement-lv_xml-removal-from-v9-5
- LVGL Pro editor pricing/licensing: https://lvgl.io/docs/pro/editor/license
- SquareLine Studio: https://squareline.io/

**Important:** this is an architecture choice and a documented proof plan, not the result of a build or of tests on a physical ESP32-S3. All the marked renderer/EEZ recommendations must pass TASK-060A/TASK-061.
