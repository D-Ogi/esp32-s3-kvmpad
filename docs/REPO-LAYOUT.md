# KVMpad repository layout

| Directory | Contents |
|---|---|
| `main/` | Firmware entry point and integration between board, UI, input, display and network components. |
| `components/` | Board drivers, input handling, UI, USB and network services. |
| `clients/windows/` | .NET Windows CLIENT, its native pairing library and desktop tests. |
| `hosts/windows/` | Windows HOST software. |
| `extensions/` | Extension packs, including the USB monitor console and pilot panel. |
| `tools/` | Extension SDK and Studio, generators for keyboard layouts, fonts and IME data, and repository checks. |
| `simulator/` | Desktop LVGL runner, scenarios and reference screenshots. |
| `test/` | Host unit tests, fuzz targets, hardware probes and component integration tests. |
| `docs/` | Hardware compatibility, firmware update instructions and extension bindings. |
| `hardware/` | Board identification and measured Flash and PSRAM sizes. |
| `reference/` | HTML mockup and screenshots used as UI references. |

## Firmware components

| Component | Responsibility |
|---|---|
| `board_hal`, `touch_input` | Board I²C, display, backlight and USB selection. The touch driver reads contacts and tracking IDs from the GT911. |
| `gesture_engine`, `ui_touch_bridge` | Recognize gestures and route touch samples to LVGL or the gesture state machine. |
| `hid_core`, `usb_hid_device` | Track held keys and buttons, arbitrate input sources, schedule reports and expose USB HID interfaces. |
| `ui_platform`, `ui_core` | Own the LVGL display, presentation state, navigation, overlays and action delivery. |
| `ui_theme`, `ui_components`, `ui_views` | Supply fonts, styles, reusable controls and the product screens. |
| `kbd_layouts`, `ime_engine` | Translate host layouts and handle on-device input composition. |
| `config_store`, `net_status` | Persist settings, handle migrations and publish Wi-Fi station status. |
| `remote_protocol`, `secure_remote` | Validate CLIENT messages and manage pairing, sessions and control leases. |
| `update_manager` | Verify signed updates and handle rollback. |
| `usb_monitor`, `live_view`, `pack_runtime` | Support USB display sessions, streamed views and extension packages. |

## Build files

The root `CMakeLists.txt` configures the ESP-IDF firmware project. The application declares its sources and component dependencies in `main/CMakeLists.txt`. Use `sdkconfig.defaults` for defaults, `dependencies.lock` for pinned dependencies and `partitions.csv` for the flash layout, including the OTA slots and the IME data partition.

The Windows CLIENT uses a separate .NET solution. The SDL simulator and host tests have their own build instructions. [Windows CLIENT](../clients/windows/README.md) · [Simulator](../simulator/README.md) · [Tests](../test/README.md)

## Repository boundary

Keep tools, virtual environments, test output, raw measurement logs and working notes outside the repository. The board record describes the measurements that determine the firmware configuration. [Board record](../hardware/boards.md)

## Dependency rules

```text
ui_views ──► ui_components ──► ui_theme
    └──────► ui_core ──(UiAction, typed queue)──► hid_core (InputArbiter) ──► usb_hid_device ──► TinyUSB
touch_input ──► ui_touch_bridge ──► { LVGL indev , gesture_engine ──► hid_core }
remote_protocol ◄── secure_remote          (remote → hid_core only via InputArbiter)
```

- The domain cores, including `hid_core` and `gesture_engine`, are independent of LVGL.
- `usb_hid_device` owns the TinyUSB device interfaces.
- The gesture engine and HID core are pure C and can be tested on the host without ESP-IDF.

The `net_status` component owns Wi-Fi station status. USB state belongs to `usb_hid_device`. The keyboard modifier state machine lives in `components/hid_core/src/kbd_fsm.c`. The UI reads its state and submits actions through `ui_core`.
