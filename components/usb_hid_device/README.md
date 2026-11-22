# `usb_hid_device`

The input scheduler in `hid_core` supplies reports through the transport hooks. Windows and Linux enumeration, OVMF behavior and the scope of physical testing are recorded in [USB and firmware compatibility](../../docs/COMPATIBILITY.md).

For the boot keyboard and mouse, `GET_REPORT(Input)` reads the scheduler's current held controls.
The mouse returns its buttons and zero relative motion. An EP0 query can occur while an interrupt
transfer is pending. The query does not consume that transfer.

The boot keyboard starts in change-only mode. A host can use `SET_IDLE` to
request the current keyboard report every 4–1020 ms, in 4 ms steps. Changes
still take priority. The rate survives suspend and resume within the same
configuration and resets to zero when the host configures the device again.
For the intervals observed on a physical board, see the [USB and firmware compatibility](../../docs/COMPATIBILITY.md).

**Role:** USB layer: boot keyboard + boot mouse descriptors, TinyUSB callbacks, LED output, USB states (enum/suspend/ready).

On a Linux USB host, run `python3 test/hil/usb_enum_linux.py --serial <board-serial>` with the normal profile active. The observer reads sysfs, checks the two boot interfaces and their input capabilities, and prints the kernel version and report descriptor hashes. The same command works inside a guest with direct USB passthrough for the whole board.

**Tests.** Host report tests in `test/host_unit/test_usb_hid_reports.c`, Windows enumeration and LED feedback via `test/hil/usb_enum_windows.ps1 -CapsLed` (AT-HID-01), Linux and OVMF hardware passthrough results linked above.

| File | Content |
|---|---|
| `include/usb_hid_reports.h`, `src/usb_hid_reports.c` | pure report builders (boot keyboard 8 B, mouse 3 B boot / 5 B report with AC Pan, consumer) |
| `include/usb_hid_device.h`, `src/usb_hid_device.c` | descriptors (VID `0x303A` / prototype PID `0x400C`, TECH-037), TinyUSB callbacks, mount epoch, transport hooks (`usb_hid_device_set_transport`, `_request_pump` via `usbd_defer_func`, `_send` refused outside the TinyUSB task — TECH-034); fallback neutral reports only without a scheduler |

**Rules (MUST):**

- `esp_tinyusb` 3.0.0 is a breaking release — do not copy 2.x examples; do not add a separate `tinyusb`.
- This component owns the TinyUSB device callbacks and sends reports from the USB task.
- Without a fresh event the device does not generate movement (TECH-064).

## Product behavior

- The three-interface HID profile adds Consumer Control. The mouse supports `AC Pan` in report protocol and preserves the three-byte boot format (TECH-072/073).
- **ESP32-S3 endpoint budget: 4 usable IN** (5 with EP0, TinyUSB `dwc2_esp32.h`). 3 × HID = 3 IN. USB CDC on the OTG port does not fit — debug through the USB-UART port.

## Four-interface candidate

Use a fresh ESP-IDF build directory and a fresh SDKCONFIG. Apply `sdkconfig.defaults;sdkconfig.hpe` in that order through SDKCONFIG_DEFAULTS. To select the three-interface reference explicitly, set `SDKCONFIG_DEFAULTS=sdkconfig.defaults`. The default project configuration adds `sdkconfig.boot-hid`. A saved three-interface sdkconfig takes precedence over defaults. The profiles share the prototype VID/PID and per-device serial. Their descriptor revisions are 0x0100 (reference) and 0x0101 (candidate). The 500 mA power declaration still requires measurement on the 7B.

| Interface | Interrupt endpoints | Reports |
| --- | --- | --- |
| 0, Boot Keyboard | IN 0x81, 8 bytes, 1 ms | No ID. LEDs use EP0. |
| 1, Boot Mouse | IN 0x82, 8 bytes, 1 ms | No ID. Three bytes in Boot protocol. Five in Report protocol, including the existing AC Pan. |
| 2, Precision Touchpad | IN 0x83, 64 bytes, 1 ms | Input 0x10, features 0x11 through 0x15. |
| 3, shared collections | IN 0x84 and OUT 0x05, 64 bytes, 4 ms | Radial 0x20, Consumer 0x21, Studio 0x22 through 0x24 and Joystick 0x30. |

The pinned HID class uses its interrupt buffer size for GET_REPORT requests. A linker wrapper around `hidd_control_xfer_cb` serves the exact IF2 GET_FEATURE(0x12) using TinyUSB's multi-packet EP0 transfer. The 257 wire bytes include the report ID and Microsoft's default uncertified status. The ordinary interrupt buffers remain 64 bytes. Configure the measured effective pad width and height in tenths of a millimeter before host qualification. Zero means uncalibrated.

The local touch router forwards captured pad contacts to a single producer. Mouse mode uses the gesture FSM. In mode 3 the producer scales coordinates to 0..32767 and emits a parallel PTP frame. Surface switch off suppresses the native surface. The scheduler gives neutral reports priority and waits for their completion across interfaces. Shared IF3 receives separate Consumer, Radial and Joystick neutral reports. Contact liftoffs retain their identifiers and last positions. Physical release and fresh input are required after a route change with a finger down.

Studio discovery uses versioned 64-byte packets. HELLO carries major, minor, capability bits LE16 and a mount nonce LE32. HELLO_ACK echoes this nonce and appends a nonzero peer nonce LE32. PING/PONG carry a four-byte tick. Incoming messages use one sequence per logical message and increasing fragment indices. One 408-byte assembly buffer expires after 300 ms. Sequence comparison uses modulo 65536. A repeated ACK_REQUIRED message can receive another transport ACK without applying the operation again. The receiver limits input to 128 fragments per second and returns a bounded sequence error for misplaced fragments. A stale heartbeat clears the agent bank, cancels outstanding actions and renews the challenge.

Studio Basic tiles use Consumer shortcuts or balanced keyboard steps. Selecting the OBS bank shows three firmware-defined controls for recording start, recording stop and mute. A physical tap can submit one typed action after the agent advertises that tile and action for the current session. The device sends an eight-byte ACTION_REQUEST containing action ID, tile ID and request ID. Retransmissions at 100 and 200 ms retain the initial sequence and request ID. An ACK confirms transport delivery. ACTION_ACCEPTED governs the 600 ms admission deadline. ACTION_RESULT supplies a result code and state version within two seconds of the request. STATE_UPDATE separately updates each tile from observed state. Home, profile change, ownership release and disconnect cancel outstanding local work. The HID feature 0x24 identifies the active profile and includes the first four bytes of the compiled application ELF digest.

AccessPad provides direct taps, dwell, one-use modifiers and scanning. Scan selection locks on the press edge. A two-second hold stops scanning. Access timing exposes scan interval (600–4000 ms), dwell (600–1800 ms), debounce (20–200 ms) and cooldown (100–1000 ms). Apply sends the four values to the existing configuration writer. The NVS key `kvmpad_cfg/access_v1` holds the timing word so a three-interface build can continue using its schema-4 settings.

Granting a Flight Yoke package access to the joystick also requires a qualified four-interface build. Enable `CONFIG_KVM_HPE_JOYSTICK_QUALIFIED` after FLIGHT-01..04/08 and Dial regression on the target. Developers can exercise the descriptor and serializer independently. The headless test builds the qualification branch against a typed recorder.

`test/host_unit/check_hpe_descriptors.py ELF --interfaces 4` reads the actual configuration and report tables from the unstripped ELF or the compiled USB object. Change the interface count to 3 for the reference. The parser uses pyelftools supplied with the pinned ESP-IDF environment.

Both configurations revoke the previous USB session when TinyUSB resets its HID class. This covers a host bus reset while the cable stays connected. The dispatcher clears held and pending input and refuses an approval queued before the reset. A host that suspends and resumes the same configuration retains its Boot protocol and PTP features. A newly configured host starts with neutral input, Report protocol and default PTP features.

Run `python test/usb_device/check_session.py --cc clang --out BUILD_DIR` with the pinned managed dependencies present and a linker supporting GNU `--wrap`. The in-memory device controller accepts host setup packets, clocks each EP0 stage and completes interrupt transfers through the production TinyUSB HID and scheduler code. It covers both interface counts without opening a USB device.

## USB monitor evaluation on the 7B

The default 7B build starts with a boot keyboard and mouse. For other profiles, see the [USB build instructions](../../docs/HANDBOOK.md#usb-startup-and-display-profiles). On the board, open
**Extensions → USB monitor** and select a display mode. For the display-only profile, the native port disconnects and re-enumerates as `303A:2987`.
**Home** restores HID with the same board serial. Both switches take place in
RAM and preserve the saved CLIENT pairing.

The Windows graphics package tested with this profile failed signature verification. KVMpad does not recommend installing it. [USB and firmware compatibility](../../docs/COMPATIBILITY.md)

The fixed-size JPEG modes advertise either 640×480 or 1024×600, a 10 fps request and a 96 KiB compressed frame limit in both their product and interface strings. Interface 0 has 64-byte
full-speed bulk OUT and IN endpoints. This profile uses the example identity
for evaluation. [Espressif protocol documentation](https://github.com/espressif/esp-iot-solution/blob/master/examples/usb/device/usb_extend_screen/windows_driver/README_cn.md) · [Upstream capability strings](https://github.com/chuanjinpang/win10_idd_xfz1986_usb_graphic_driver_display)

Run `python -I test/usb_device/check_monitor_image.py PATH_TO_KVMPAD_ELF` to
verify both personalities in the linked image. The `check_session.py` replay
exercises local selection and cancellation, the host's reset and configuration
sequence, multi-packet descriptor reads, bulk OUT, suspend, unplug and return
to HID. See [USB and firmware compatibility](../../docs/COMPATIBILITY.md) for desktop results.

### Display with local keyboard and mouse

An extension can request the composite profile with `usb_hid_device_set_monitor_controls(true, native)`.
It uses the installed xfz1986 driver's composite identifier `USB\VID_303A&PID_2986&MI_00`.
Interface 0 carries the display over 64-byte bulk OUT/IN (0x01 and 0x81). Interfaces 1 and 2
are the existing Boot Keyboard and Boot Mouse, with 8-byte interrupt IN endpoints 0x82 and 0x83.
The USB task maps these to HID class instances 0 and 1 for the regular input scheduler.
The profile uses three non-control IN endpoints on the ESP32-S3. Keyboard LEDs use EP0.
Pass `false` as the first argument to restore the standard HID configuration and its per-device serial.
