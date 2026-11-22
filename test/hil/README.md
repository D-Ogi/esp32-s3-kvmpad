# `test/hil/` — hardware-in-the-loop helpers (L3–L4)

Use the serial port assigned to the board's USB-UART interface. UART tests require Python with pyserial. USB enumeration tests run on the computer attached to the native USB interface.

| Script | Use |
|---|---|
| `hpe_get_windows.ps1` | Read the enhanced HID feature reports of the board selected by `-Serial <board-usb-serial>` or the `KVMPAD_USB_SERIAL` environment variable. Add `-Inspect` to inspect descriptor fields. `-BuildOnly` builds the probe without a connected board. |
| `kvm_serial.py` | UART console of the board (pyserial, `COM3` by default). `boot` resets via RTS and checks the boot (EXIO5 verified LOW, GT911 1024×600, USB started and mounted); `cmd "<line>" …` runs console commands without a reset; `bootloop N` repeats `boot` N times. `--no-host` when no USB HOST is attached. |
| `usb_enum_windows.ps1` | AT-HID-01 on a Windows HOST. Checks the boot keyboard and mouse, interface count, class, subclass, protocol and node status. Pass `-Interfaces 3` for Consumer Control or `-Interfaces 4` for the enhanced profile. The optional `-CapsLed` toggles Caps Lock twice and checks the device's LED output reports. |
| `wifi_latency.py` | ADR-017 benches over the console, no reset: `reconnect --count N` (AT-EXT-02, alternating cached and full-scan reconnects timed on the board) and `ping --host H --idle 10 60` (AT-EXT-01, first-packet latency after silence with `WIFI_PS_NONE` vs `WIFI_PS_MIN_MODEM`; `ps none` is restored at the end). |
| `remote_probe.py` | TASK-100 transport from the LAN, no pairing and no token: `mdns` (`_kvmpad._tcp` PTR/SRV/TXT/A; `--iface <LAN address>` on a PC with virtual adapters, `--multicast` when the firewall drops the unicast answer), `tls` (version, certificate, SHA-256 pin, `--expect-sha256` = the `remote status` value), `admit` (HTTP Upgrade refusals before any WebSocket: 403 / 400 / 404, never 101), `plain` (no plain-text listener). `--save <dir>` writes the output and prints its SHA-256. |

Keep reset loops short (15 cycles): every reset re-enumerates the USB device on the HOST.
Without the physical board every hardware result is `NOT_RUN_HW`.

For MAG-13, boot the engineering image with `KVM_MEDIA_SYNTHETIC_DIAG` and guarded double-partial scanout capture,
leave the panel at Home and run
`python -B test/hil/magnifier_pattern.py <new-output.png> --port COM3` from the repository with pyserial and Pillow.
The UART transfer at 115200 baud takes about four minutes. The command displays a local 512×512 RGB565 chart,
reads the scanout counters and validates the native LCD snapshot in memory before saving it. The guarded transfer
must report `SNAP_SOURCE scanout` before any rows. **Stop** and **Home**
remain available on the panel. After transfer the runner closes the chart and verifies the return to the shell
before writing the PNG.

With the same engineering build at Home and the test card inserted, run
`python -B test/hil/card_playback_regression.py <new-results.json> --port COM3` from the repository.
For the standard 7B panel, the runner measures completed RGB frames against VSYNC
from the start of each case through its result screen and verifies both interrupt callbacks ran on CPU1.
Use the product display configuration for this check. An engineering build left on the CPU0
comparison setting can verify every picture in memory while the physical panel shifts. For a deliberate
comparison or historical firmware without interrupt telemetry, use `--skip-scanout-check`.
The runner requires every frame of the four 640×360 MP4 clips encoded at 5, 10, 15 and 25 fps
to complete a display refresh through direct Exact playback.
The 1080p sample must use its prepared Exact companion, complete 73 framebuffer checks and show zero
observed black gaps across its 72 qualifying transitions. The runner measures LCD fps, checks the return
to Home and the boot count for every file, and accepts either valid OTA slot. It opens UART without resetting the unit.

For temporal playback, place the 12 MP4 test clips and their matching .kvf and .kvs files in the card
root and add `--temporal-rates`. This measures both temporal modes at 320×180, 640×360 and 960×540
at 5, 10, 15 and 25 fps. Add `--rate-matrix` when the spatial companions and the original 1080p sample
are present too. Each timed result includes the LCD rate and refresh interval percentiles.

Use `--compare-scale` with the twelve MP4 clips and their Exact companions to run 24 paired presentations.
Each clip plays at Fit and 1:1 in alternating order. The runner checks the displayed dimensions, frame
count, source timeline, LCD refresh counters and return from playback for each presentation.

For the synthetic touch-routing benchmark, enable `CONFIG_KVM_UI_BENCH_TAP` in a bring-up build and
select a benchmark scene with `ui scene idle`. The default firmware receives local touches from the GT911.

To check USB display colors from Windows through the 7B LCD, open **Extensions → USB monitor** and
extend the Windows desktop onto the board. From the same interactive Windows desktop, run
`python test/hil/usb_monitor_pattern.py --probe PATH/usb-monitor-windows.exe --serial BOARD_USB_SERIAL --port COM3`.

Use `usb-monitor-windows.exe BOARD_USB_SERIAL --modes` to list the modes Windows offers on the
active, independent display identified by that 12-digit USB serial. The header reports the current
desktop resolution and, when Windows supplies it, active signal resolution. Mode rows give the
driver's integer refresh rate (0 or 1 means default), bit depth and display flags.
`usb-monitor-windows.exe BOARD_USB_SERIAL --test-mode 1024 600` asks the driver to validate each
listed 1024×600 variant using `CDS_TEST`. Exit code 0 means at least one variant was accepted.
This preflight leaves the desktop untouched. Actual scanout and board presentation require a separate run.

Use the Windows probe built as described in ADR-043, Python with pyserial, and firmware
that reports `SNAP_SOURCE scanout`. The launcher checks the probe's `--pattern-version` and prints
the current Windows display mapping before it opens the UART. Pillow is required only when
checking a saved PNG with `--snapshot`. The probe draws four color fields on the identified board for
30 seconds. The UART check waits for a fresh USB presentation, verifies the checksum of the
quarter-size panel readback, and checks the interior of each field. Firmware with decoded-picture
diagnostics also reports whether the selected RGB565 frame contained any nonblack pixel immediately
before the snapshot. The two observations can refer to successive frames. The color pattern stays
visible during both. The test saves no desktop image.

Add `--final-frame` to test what the receiver retains after the last desktop change.
Once the first moving red, green, blue and white quadrants pass, the probe paints white, red, green
and blue in the same positions and stops animating. The runner waits two seconds, requires an additional
board presentation and checks all four quadrants of a second LCD snapshot. It leaves the Windows pattern still
for another five seconds and checks a third snapshot. The JSON records how many presentations
occurred during the hold and readback. Zero additional presentations is valid. A black picture, the first
pattern left on screen, or loss of the display target fails the test. With `--result FILE`, the JSON
records the counts for each verified phase. This mode requires a probe reporting `KVM_USB_FINAL_PATTERN 2`.
