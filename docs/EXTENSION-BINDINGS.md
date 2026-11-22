# Extension bindings

Each connection in `bindings.json` reads a declared local state or telemetry source and writes one widget field. A profile can have 64 connections. Each field has one writer. The desktop validator and firmware reject cycles. Text and StatusChip accept `.text`. GaugeArc, GaugeNeedle and ValueTile accept numeric `.value`. Horizon accepts numeric `.pitch` and `.roll`.

Section 14.3 of the v1.3.1 attachment names ten transforms. Its JSON Schema lists their names and leaves their parameters unspecified. The repository schema adds an optional `params` object and the defaults below. Profiles that omit `params` retain the original connection shape. Custom parameters require a validator and firmware with this extension. [Repository schema](../tools/kvmpack/schemas/kvmpack-bindings.schema.json)

| Transform | Optional parameters and defaults | Result |
|---|---|---|
| `identity` | None | Preserves the original type and value, including every signed 64-bit integer. |
| `clamp` | `min=0`, `max=1` | Constrains a number to the inclusive range. Equal endpoints are permitted. |
| `scale` | `factor=1` | Multiplies the input by the factor. |
| `offset` | `amount=0` | Adds the amount. |
| `round` | None | Rounds to the nearest integer value. Halfway values round away from zero. |
| `enum_map` | `entries` | Returns the matching label. The default table maps 0 to `Off` and 1 to `On`. Other codes display `Unavailable`. |
| `hysteresis` | `low=0.25`, `high=0.75` | Starts false, becomes true at or above `high` and becomes false at or below `low`. Requires `low < high`. |
| `lowpass` | `tau_ms=250`, integer 1–60,000 | A first-order filter starts at the first valid sample. The weight of the new input after Δt milliseconds is `1 − exp(−Δt/tau_ms)`. |
| `format_number` | `digits=2`, integer 0–6 | Produces fixed-point text with exactly this many fractional digits. Halfway values round away from zero. |
| `boolean_threshold` | `threshold=0.5` | Returns true at or above the threshold and false below it. |

All transforms except `identity` require numeric input. Arithmetic uses finite binary64 values. Results that overflow display `Unavailable`. Fixed-point rendering supports rounded magnitudes up to 9,007,199,254,740,991 after multiplication by the decimal scale. Each custom enum table accepts one to eight distinct signed 32-bit codes with labels of one to 48 UTF-8 bytes. All connections share a limit of 64 custom entries. An unmatched or fractional code displays `Unavailable`.

For example, this connection shows flap position as a label. The profile declares a numeric source named `flap` or a numeric local state with that name.

```json
{
  "from": "flap",
  "to": "flap_status.text",
  "transform": "enum_map",
  "params": {
    "entries": [
      {"code": 0, "label": "Retracted"},
      {"code": 1, "label": "Extended"}
    ]
  }
}
```

Each connection keeps its own filter and hysteresis history. The firmware evaluates active filters on its 50 ms presenter tick. A source becomes stale when its age exceeds its declared `max_age_ms`, or when the provider marks it stale. The current simulator bridge marks samples stale at 1,000 ms and unavailable at 3,000 ms. The display clears the old reading on either transition. The next valid reading seeds the filter again. Closing and reopening the profile resets its history. Pausing demo telemetry in the offline preview exercises the same aging rules.

## Local controls

Section 14.3 of extension specification v1.3.1 names these controls and events. Its UI schema leaves the keys of `props` open. The following contracts define the bounded offline implementation used by the device and the SDK preview. All four controls use `ui.basic`. Each node needs an ID and integer bounds inside its parent. A landscape profile has 1024 × 568 pixels below the system header. Set `"orientation":"portrait"` in ui.json for 600 × 992 pixels with the board turned clockwise.

| Control | Optional properties and defaults | Input |
| --- | --- | --- |
| TouchArea | `label` defaults to `Touch area`. Minimum 48 × 48 pixels. | A short tap emits `tap`. Moving at least eight pixels on either axis starts `drag` with local pixel coordinates. Holding still emits one `long_press` at the LVGL input threshold (400 ms by default). A drag or hold consumes the tap. Leaving the area cancels the gesture. |
| Dial | `label` defaults to `Dial`. Integer `min` and `max` default to 0 and 100. Both fit in signed 16 bits and min is below max. Initial `value` defaults to min. Minimum 72 × 80 pixels. | Turn the 270° arc to emit `rotate` on a value change. The preview also supports arrow keys and the Home and End keys while the dial has keyboard focus. |
| RateSlider | Empty properties. Minimum 48 × 96 pixels. The Studio palette starts at 64 × 160. | Touch the slider, then move up for `step_up` or down for `step_down`. Moving farther from the center repeats faster. Beyond either end, steps continue at the maximum rate, even if your finger drifts sideways. Return to center to pause or lift anywhere to stop. |
| TabStrip | `tabs` holds two to four distinct captions, each 1–24 UTF-8 bytes. Defaults are `["Tab 1","Tab 2"]`. Zero-based `selected` defaults to 0. Allow at least 48 pixels per tab and 42 pixels in height. | Selecting a different tab emits `change`. Optional `pages` names one sibling Column, Row or Grid per tab. Only the selected page is visible. Each page belongs to one strip. A scene can contain up to 32 tab captions in total. |
| StatusChip | `label` defaults to `Status`. Existing `text` takes precedence. Choose **Status chip role** in the Studio widget inspector or set `props.status` to `neutral` (default), `capability`, `permission`, `info`, `success`, `warning` or `error`. Each role has a distinct symbol, and its colors follow the device palette. | A text binding updates the label and retains the authored role. The Flight checklist example uses an `info` chip for both `Pending` and `Complete`. Telemetry text can also show `Stale` and `Unavailable`. |

TouchArea, Dial and TabStrip accept local `state.set` actions. A literal follows the target state's usual type rules. An integer state can also receive the current dial value or selected tab index with `"value":{"event":"value"}`, or a drag coordinate with `"value":{"event":"x"}` or `"value":{"event":"y"}`. The target range must include every possible event value. A 200-pixel-wide TouchArea, for example, emits x values from 0 to 199. Control selections and gesture tracking reset when the profile opens.

For a local setpoint, add a `RateSlider` in Studio and bind its two events to separate actions. The following entries in `bindings.json` adjust a declared integer state named `altitude` by 100 feet per step. In `ui.json`, give the slider the ID `wheel` and set `"events":{"step_up":"increase","step_down":"decrease"}`.

```json
[
  {"id":"increase","trigger":"wheel.step_up","type":"state.adjust","capability":"ui.basic","args":{"name":"altitude","delta":100}},
  {"id":"decrease","trigger":"wheel.step_down","type":"state.adjust","capability":"ui.basic","args":{"name":"altitude","delta":-100}}
]
```

`state.adjust` reads the current local integer, adds a nonzero signed 32-bit `delta` and stops at the state's `min` or `max`. An omitted bound uses the corresponding signed 64-bit limit. Either direction may also invoke `state.set`. Both actions work offline and use the package's event budget. The sample opens in Extension Studio, displays the selected altitude and needs only `ui.basic`. It requires SDK and firmware support for `RateSlider`. [Altitude selector sample](../tools/kvmpack/samples/rate-slider)

```json
{"id":"set_trim","type":"state.set","capability":"ui.basic","trigger":"trim.rotate",
 "args":{"name":"trim_value","value":{"event":"value"}}}
```

## Multiple screens

Add a top-level `screens` array to ui.json, for example `"screens":["radio","kneeboard","setup"]`. List two to ten IDs. Each must be a direct child container of the root with a distinct ID and a `props.title` of 1–48 UTF-8 bytes. The list must include every direct child. Its order determines the picker order and its first entry opens at startup. A screen may contain a TabStrip and its own nested pages.

Reserve the bottom 48 pixels for the screen picker. This leaves 1024 × 520 pixels in landscape or 600 × 944 pixels in portrait for the root. Tap the bottom handle or swipe up from it to select a screen. Local selections remain active across screen changes. Choose **Return to KVMpad** inside the picker to leave the package. Single-screen packages retain their header Home button.

A TabStrip can follow a local integer state through an identity binding to `<id>.selected`. Declare both state bounds inside the zero-based tab range. Bind several strips to the same state and use their `change` events to set it. A state update refreshes the selected tab and its page without emitting another event.

## Offline pilot instruments

A `ChartViewer` accepts two or three distinct entries in `props.charts`. Each entry has a `title` of up to 48 UTF-8 bytes and an `asset` naming an indexed PNG in the package. In Studio, select **ChartViewer → Add widget** and choose the chart images together. Studio reads PNG, JPEG and WebP, fits each image into 600 × 440 and prepares a 16-color PNG. The control needs at least 540 × 420 pixels. Slots A and B start with the first two charts. Pinning the other slot's chart swaps them. Each chart keeps its own zoom and pan while the package is open.

Drag the chart with one finger. Place two fingers on the map and spread or pinch to zoom between 100% and 600%. The chart follows the midpoint of the two contacts within its pan limits. Lift both fingers to start another drag. The **+**, **−** and **Fit** controls also work with one finger.

An `AutopilotPanel` needs 540 × 680 pixels and accepts empty properties. It opens offline. **Start demo** enables a local aircraft simulation for the HDG, ALT and VS selectors and the AP, HDG, NAV, VS and ALT buttons. Values under **Actual** come from the demo aircraft. Submitted settings change after acknowledgment. The demo can reject a request and separately capture NAV or the selected altitude.

A `RadioPanel` uses the same minimum dimensions and empty properties. In **Start demo**, select COM 1 or COM 2, tap the standby frequency, enter six digits, and confirm with **OK**. **Swap** exchanges the active and standby channels for the selected radio. Tap the four-digit transponder code to enter an octal code, including leading zeroes. The STBY, ON and ALT controls request a mode change. The panel shows confirmed values after readback. The shipped package uses the local demo.

These instruments use the local UI permission. A package can contain one widget of each kind.

## Joystick controls

The Flight Yoke template uses `hid.joystick.set_axes` for pitch, roll, yaw, throttle, buttons and the eight-way point-of-view hat. The package must declare `hid.joystick` permission and USB capability. The panel admits it when the installed firmware supports the capability and the operator grants it.

Each hat direction uses one Button with a pair of actions. For example, bind the button's `press` to `{"hat":2}` in the action's `args` for east and its `release` to `{"hat":8}` for neutral. The directions run clockwise from north (0) through northwest (7). A canceled press also releases the hat. Hat actions preserve throttle and the other controls.

An AxisPad binds X to `$event.x` and Y to `$event.y`. Its optional `props` configure the two signed axes. `dead_zone_percent` accepts whole percentages from 0 to 99 and defaults to 0. Each axis stays neutral inside that fraction of its center-to-edge travel. The remaining travel spans the full output range. `curve` defaults to `"linear"`. The `"cubic"` setting reduces sensitivity near center while preserving both endpoints. `invert_y` defaults to false. Set it to true to reverse pitch. `spring_center` defaults to true and returns both axes to zero on lift. The separate throttle slider retains its absolute setting on lift.

## USB monitor console

Use `usb.mode = "monitor_controls"` for a package combining a USB desktop with local input. Declare each surface's capability in `permissions`, `required_permissions` and `usb.requires`. A package needs a `MonitorViewport` so the operator can choose the USB signal size and start the desktop locally. The default is 640 × 480. The operator can also select 1024 × 600. Every page fits the complete picture at its original aspect ratio and shares the current selection. Stop the monitor to change its USB signal size. Windows manages the desktop resolution separately. This mode supports the following three USB capabilities.

| Surface | Capability | Minimum width × height |
|---|---|---|
| `MonitorViewport` | `usb.monitor` | 160 × 120 |
| `KeyboardSurface` | `hid.keyboard` | 440 × 176 |
| `TouchpadSurface` | `hid.mouse` | 132 × 88 |

To keep moving the pointer, slide your finger past an edge of the touchpad and hold it there. The pointer keeps moving toward that edge while your finger rests over the keyboard or monitor. Slide back onto the pad to resume ordinary pointer movement, or lift your finger to stop.

Tap **Gestures** on the touchpad to open a side panel with instructions for moving, clicking, scrolling and dragging. With the USB monitor connected, **Left click**, **Right click** and **Middle click** click at the current pointer position and close the panel. Tap **Close** or anywhere outside the panel to return without clicking. The gesture guide is also available offline.

Once Windows connects, tap **1:1** on the desktop to show each incoming USB pixel at its original size. Drag the picture to inspect areas outside the viewport. The separate touchpad continues to move the Windows pointer. Tap the picture to hide or show **Fit** and **Stop**. Switching pages keeps the zoom, the area you were viewing and the visibility of the controls. A larger viewport shows more of the desktop around that area, up to its edges. **Fit** restores the entire picture. A new session starts in Fit with visible controls.

`KeyboardSurface` embeds the system's compact keyboard. Its keycaps, palette and configured host layout come from KVMpad. The system Keyboard view also uses this surface when space is too narrow for its full rows. **123** opens the number and symbol keys, including the extra physical keys on JIS and Brazilian ABNT2 keyboards. On layouts with input-method keys, **IME** switches the bottom row to conversion controls such as **Henkan** or **Han/Eng**. Tap **IME** again to return to the usual controls.

**Caps** sends the Caps Lock key. Its second line shows the host keyboard LED report. **Unknown** (shown as **?** on compact surfaces) means that the keyboard is offline or the current USB connection has yet to receive a report. Letter and symbol legends follow the selected Shift and AltGr layers.

**123 → Fn** opens F1–F12 and document navigation keys. Layouts with room beside **Enter** also show **Fn** on the letter page. **Back** returns to the page you came from. **Win** taps the Windows key. To enter a shortcut such as Win+D, tap **Win+**, return to the letters and tap **d**. The modifier releases with that key. Switching keyboard pages keeps the desktop and touchpad in place.

Each surface uses its authored bounds and accepts empty properties, events and children. Each page in `ui.screens` can contain one of each type. A single-page package has the same limit. In portrait, a multi-page layout has 600 × 944 pixels between the header and the **Screens** handle. The preview shows the layout offline. [USB Monitor Console source](../extensions/usb-monitor-console/ui.json)
