# ADR-043 — USB-C extended desktop with the existing Windows driver

### Resolution selection in Windows

The signed Windows driver can expose **640 × 480 and 1024 × 600 in the same USB attachment**. On unit 1, a temporary firmware build omitted the resolution field from the composite display capability string and kept JPEG quality, frame rate and byte limit unchanged. Across two independent attachments Windows offered the same 17 sizes at 60 Hz, from 240 × 240 through 1920 × 1080. Both started with a 640 × 480 desktop and signal. In each 20-second observation the board received 5,442 bytes and presented one black VGA picture. The trial only enumerated the modes. Applying another size from Windows Settings and rendering it on the LCD still requires verification. [Two hardware observations](../../../evidence/build/usb-driver-default-modes/observations.json)

The driver uses a built-in list when the descriptor omits the width. Both the monitor mode and the target mode callbacks use that list. The device therefore has to handle incoming frame dimensions changing while USB stays mounted. The current release allocates its receiver for the selected fixed size and checks each frame against it. A Windows-managed profile needs to fit the incoming sizes to the panel within its PSRAM budget and preserve the keyboard and pointer through a size change. The trial restored the previous signed firmware, pairing and four HID interfaces. [Vendor mode selection](https://github.com/chuanjinpang/win10_idd_xfz1986_usb_graphic_driver_display/blob/main/win10_idd_xfz1986_usb_graphic_driver/idd_xfz1986_usb_graphic/usb_transf.cpp#L387-L428) · [IDDCX mode callbacks](https://github.com/chuanjinpang/win10_idd_xfz1986_usb_graphic_driver_display/blob/main/win10_idd_xfz1986_usb_graphic_driver/idd_xfz1986_usb_graphic/Driver.cpp#L947-L1021) · [Restored firmware](../../../evidence/build/usb-driver-default-modes/restored.json)

### Fixed USB profiles

The Windows CLIENT includes a USB display dialog in Tools. It resolves the active KVMpad by its serial-bearing USB parent and offers the driver's advertised desktop modes at the current refresh rate, orientation and color depth. Before a change it checks that the board has an independent desktop and that the proposed size fits the existing arrangement. After a change it reads the applied mode and layout back from Windows.

In a composite USB session on November 20 the CLIENT and an independent display probe both reported a 640×480 desktop, a 1024×600 signal, and a single offered 1024×600 mode. Windows returned `DISP_CHANGE_FAILED` during validation. The subsequent probe confirmed the same desktop dimensions, and all four HID interfaces returned on exit. Interactive application of the native desktop remains open. [Hardware record](../../../evidence/build/windows-usb-resolution/board.json)

On the 7B at boot 490, three read-only Windows validations per profile returned `DISP_CHANGE_FAILED` in the development session, including for the already active 640×480 VGA desktop. The physical signal and desktop both matched in VGA. The native profile advertised 1024×600 while the desktop remained 640×480. The latency probe rejected this combination before painting. The USB-only CLIENT command can match the source to an already active signal on the interactive Windows desktop. Native timing still needs that interactive run. [Mode checks and HID return](../../../evidence/build/usb-monitor-native-latency/qualification.json) · [Native attempt](../../../evidence/build/usb-monitor-native-latency/physical.json)

Status: PROPOSED
Date: 2026-11-20

Windows must expose an additional desktop surface on the attached 7B. The owner accepts a separately installed, already signed driver. The project must be able to ship its firmware without building, modifying or signing a Windows display driver.

Use the Windows 10/11 package distributed by Espressif for its USB Extended Screen example. The package originates from the xfz1986 USB graphics project. Espressif documents the ESP32-S3-LCD-EV-Board with an 800×480 RGB panel at 13 frames per second. The 7B has a different panel, pin mapping and competing HID functions. Its throughput requires a separate measurement. [Espressif example](https://github.com/espressif/esp-iot-solution/tree/master/examples/usb/device/usb_extend_screen) · [Windows package and supported USB identifiers](https://github.com/espressif/esp-iot-solution/blob/master/examples/usb/device/usb_extend_screen/windows_driver/README_cn.md)

The data cable connects to the native USB port. The UART port remains available for diagnostics. A 640×480 desktop fits the physical 1024×600 screen at native pixel size with room for local controls. The device advertises its dimensions and encoding limits according to the driver protocol. The existing keyboard, pointer and saved CLIENT pairing must be available again after leaving monitor mode. [Waveshare board interfaces](https://docs.waveshare.com/ESP32-S3-Touch-LCD-7B) · [Wire protocol and interface strings](https://github.com/chuanjinpang/win10_idd_xfz1986_usb_graphic_driver_display)

The evaluation configuration boots as the regular KVMpad. Selecting **Extensions → USB monitor** temporarily enumerates the native port as the example's display-only device, `303A:2987`, with a bulk input and output endpoint. Selecting **Home** restores the four HID interfaces. The Windows operator obtains the original installer from Espressif. The evaluation uses the example's USB identifier. Distribution under a project USB identity requires that identity to be covered by the signed package. [Espressif USB identifiers](https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_overview/usb_vid_pid.html)

Qualification requires Windows to bind the unmodified driver, report an additional active desktop target at the advertised resolution, and display changing test pixels on the physical LCD. Record the driver version, selected USB identity, measured dimensions and frame counts. Test local exit, cable detach and reconnect, return to the normal HID interfaces and preservation of the existing pairing. A screenshot of the magnifier or successful USB enumeration alone cannot verify extended-desktop support.

The default Windows test in `test/hil/usb_monitor_windows.cpp` checks the active target's Plug and Play ancestry against the board's USB serial, its 640×480 source mode and its independence from the other active desktop sources. Build it with `clang++ -std=c++17 -O2 -static -municode test/hil/usb_monitor_windows.cpp -lcfgmgr32 -lsetupapi -luser32 -lgdi32 -o usb-monitor-windows.exe`. Run it in the interactive Windows session with the 12-digit board serial as its sole argument. Add `--usb` to read the USB descriptors independently of the installed display driver.

On November 20, unit 1 opened the Extensions entry, decoded a synthetic red and blue JPEG, completed the LVGL refresh, and matched 16 expected RGB565 pixels in a physical LCD framebuffer. The run returned to Home with all four HID interfaces and the saved pairing. The test used the local receiver input and preserved the HID identity throughout. Windows driver binding and an active extended desktop remain to be measured. [Board test log](../../../evidence/hardware/20261120-usbmonitor-lcd-86a4677c0d26.log)

In a separate physical USB trial at boot 368, Windows detected `USB\VID_303A&PID_2987` on port 8. Reads through the parent USB hub matched the full 32-byte configuration, both 64-byte bulk endpoints, both 640×480 capability strings and the board serial. Windows reported device problem code 28 while the display driver was absent. The timed exit restored all four HID interfaces and the original pairing in the same boot. [USB hub and Plug and Play readback](../../../evidence/hardware/20261120-usbmonitor-link-9d6a9d469577.log)

At boot 369, after the owner installed XFZ1986 version 1.2.25.2, Windows started the adapter and exposed an independent 640x480 desktop on unit 1 alongside two other active targets. Windows validated the installed driver catalog signature. The owner saw the desktop on the physical panel. Over 6.03 seconds the board completed 13 picture refreshes, dropped 47 incoming frames while busy, and reported zero decode errors. The monitor remained active after the measurement. [Windows topology, signed driver and board counters](../../../evidence/hardware/20261120-xfz1986-desktop-boot369.json)

With the queued receiver on unit 1 at boot 371, the installed driver created two successive independent 640x480 desktops. Windows assigned a different GDI source name on each entry. The first trial completed six picture refreshes and the second completed seven before the snapshot. Neither trial reported a decode error. In both trials the four HID interfaces and the saved pairing returned in the same boot. The second trial read all 600 rows of a 1024x600 board snapshot and verified the device checksum `6C5F4F1A`. Its local controls are visible and the entire 640x480 desktop region is black. A separate local two-color fixture matched the expected LCD pixels. The mostly static Windows desktop did not establish sustained motion throughput. [First Windows session](../../../evidence/hardware/20261120-usb-desktop-boot371-first.json) · [Second Windows session](../../../evidence/hardware/20261120-usb-desktop-boot371-second.json) · [Board snapshot](../../../evidence/hardware/KVMpad-USB-LCD-boot371.png)

At boot 380 the driver exposed a separate 640×480 desktop on unit 1. The board accepted, decoded
and presented seven frames with zero busy drops or decode errors. Its selected 307,200-pixel RGB565
frame was entirely black on both sides of a board snapshot, with unchanged frame counters. All
19,200 desktop samples in the quarter-resolution scanout were black. The local Home button and
title had visible contrast in the same buffer. A separate red/blue JPEG test matched 16 expected
scanout pixels. This locates the black desktop at or before decoder output. The encoded Windows
frame itself and optical panel output were not measured. [USB desktop measurement](../../../evidence/hardware/20261120-magnifier-eddd8097f088.usb-desktop.json) · [Board snapshot](../../../evidence/hardware/20261120-magnifier-eddd8097f088.usb-desktop.png) · [Local color test](../../../evidence/hardware/20261120-magnifier-eddd8097f088.usb-colors.log)

At boot 382 the ROM decoder and the independent Espressif block decoder both detected color in the
local red/blue JPEG. The fixture also matched the expected LCD pixels. Windows then activated a
separate 640×480 desktop and sent seven JPEGs, all of which reached presentation. Both decoders
classified every RGB565 pixel of the seventh JPEG as black. The selected sequence and counters held
steady across the LCD snapshot, which contained 19,200 black desktop samples and visible local controls.
This frame already represented black when it reached the USB receiver. The Windows compositor content
for that moment and the light emitted by the LCD were outside the measurement. The original release
booted again at boot 383 with the same Extensions menu, four HID interfaces and its saved pairing.
[Board measurement](../../../evidence/hardware/20261120-usb-jpeg-63e5812926c5.json) · [USB desktop snapshot](../../../evidence/hardware/20261120-usb-jpeg-63e5812926c5.desktop.png)

At boot 392 the owner confirmed that the six local Extensions tiles were visible and that the Windows
picture reached the panel in USB monitor mode. The probe resolved unit 1 by USB serial to a separate
Windows source at (6000, 0), then named DISPLAY16. Its desktop and active signal were both 640×480,
with identity rotation. A half-resolution sample taken directly from the LCD scanout showed a desktop
window and wallpaper. The desktop rectangle contained 4,029 distinct colors and 76,791 nonblack
samples out of 76,800. While the board transferred this snapshot over UART, its presentation count
rose from 62 to 116 and both decoder error readings were zero. [Windows topology](../../../evidence/build/usb-monitor-owner-test/unit1-extended-640x480.txt) · [Board counters](../../../evidence/hardware/KVMpad-USB-LCD-20261120-055839.json) · [Board screenshot](../../../evidence/hardware/KVMpad-USB-LCD-20261120-055839.png)

A later native-resolution screenshot at the same boot showed black throughout the desktop rectangle
and visible local controls. Presentation count stayed at 116 over the 215-second UART transfer,
and the selected decoded picture subsequently reported all black. The two observations establish
that this session displayed colored and black desktop content at different times. The Windows-side
cause of the transition remains open. [Native snapshot and counters](../../../evidence/hardware/KVMpad-USB-LCD-native-20261120-060049.json) · [Native LCD screenshot](../../../evidence/hardware/KVMpad-USB-LCD-native-20261120-060049.png)

The Windows test now paints moving red, green, blue and white quadrants on the board's
extended desktop. After the first LCD readback passes, the test changes the quadrants to
white, red, green and blue, then stops painting. A second readback must match the new order
and the board must have presented another frame. The runner holds the same Windows picture for
another five seconds and verifies its four fields in a third LCD readback. It records how many
additional presentations occurred during the hold and readback, including zero when the driver
remains idle. With the Windows pattern still visible, the runner also queries the selected USB
picture after the last readback. A missing or entirely black picture fails the trial, even if
the framebuffer was still colored when the UART transfer began. If the firmware cannot report
the selected picture and another presentation arrived during readback, the runner requires a
matching readback with a stable presentation counter on one retry.
The initial owner confirmation covered desktop delivery. The three-readback hardware
trial is pending.

USB suspend pauses the configured monitor endpoints. The receiver retains the selected desktop
and any partial JPEG until resume. The USB task consumes completed bulk packets in event order,
including an OUT queued just before the suspend interrupt. Unplugging, bus reset, configuration
loss and closing the monitor end the stream and discard its partial input.

Release candidate `9a6fdd72a6a9` adds a local status message when the last presented USB
picture is entirely black and the stream has been idle for one second. The next presentation
clears the message. The signed build passed the four-interface descriptor check and the
display regression tests. Board 1 continued running release `b886896599cd` during these checks.

A subsequent readback from unit 1 showed the Windows wallpaper on the LCD. The presentation
counter stayed at 215 and the decoder error counter stayed at zero across this capture.
The half-resolution viewport contained 5,178 distinct RGB565 colors and 76,735 nonblack
samples out of 76,800. [LCD readback](../../../evidence/hardware/KVMpad-USB-LCD-20261120-121442.png) · [Board and Windows measurements](../../../evidence/hardware/KVMpad-USB-LCD-20261120-121442.json)

The black-picture hint now measures elapsed time from the first completely black presentation.
A second black frame keeps the same timer running. A frame containing any visible color
clears the hint when its refresh finishes. This covers both a stationary desktop and a
continuous black stream. [Monitor regression tests](../../../evidence/build/usb-monitor-stream-black/normal.log)

On the 7B, Windows offered 640 × 480 when the receiver advertised VGA. Three frames reached the LCD and leaving monitor mode restored the four HID interfaces. The native-mode receiver stopped before USB enumeration. Its two RGB565 surfaces alone require 2,457,600 bytes, and the board reported 2,220,916 bytes of free PSRAM before startup. This two-surface receiver could not open native mode. [Windows modes](../../../evidence/build/hybrid-edge/windows-modes.json) · [Native startup](../../../evidence/build/hybrid-edge/native-retry.json)

The single-surface receiver preserved a good native image through a failed decode by retaining its compressed JPEG. On unit 1, firmware `f656fb9f24d8` opened 1024×600 twice with three compressed input slots. Both sessions decoded and presented Windows frames without JPEG errors. Windows reported a 1024×600 active signal and an offered 1024×600 mode, while its source desktop remained 640×480. The Windows display configuration validation call returned access denied in the development session. A 1024×600 source desktop and user selection in Windows Settings still require interactive verification. [Board and mode measurements](../../../evidence/build/usb-monitor-latency/physical.json) · [Windows validation](../../../evidence/build/usb-monitor-latency/validation.json)

With the ROM decoder the native sessions averaged 477 and 514 ms in decoding and 813 and 932 ms from first JPEG payload bytes to LVGL refresh completion. The VGA run averaged 334 and 546 ms, respectively. Each sample includes the work for a single presented picture. These were desktop attachment measurements.

On the same unit the accelerated RGB565 receiver (`a6402470193f`, boot 463) used its block decoder in both modes. The native runs averaged 108 and 115 ms in decoding and 403 and 463 ms from the first payload to LVGL refresh completion. VGA averaged 121 and 303 ms. There were 14, 15 and 12 measured presentations in the three attachment runs. The selected image contained nonblack pixels in all three. The native display refresh alone averaged 151–152 ms. The results describe these desktop sessions, which received different Windows frames. No input-to-glass measurement was taken. Leaving the last session restored the four HID interfaces and the top-edge keyboard workspace. [Physical timing](../../../evidence/build/usb-monitor-fast/physical.json) · [USB return](../../../evidence/build/usb-monitor-fast/hid-return.json)

On November 20, boot 464 compared RGB565 output with the preceding picture before invalidating the LCD. In two native sessions, two full redraws took 304 ms in total, ten partial redraws took 474 ms, and 19 identical pictures required no redraw. VGA also presented a 256-pixel change in 42 ms. The ordinary four HID interfaces reappeared in Windows after the last session. These samples measure refresh time during desktop attachment. Latency during real pointer motion and window dragging remains open. [Board timings](../../../evidence/build/usb-monitor-damage/measure.log) · [USB return](../../../evidence/build/usb-monitor-damage/hid-return.json)

Boot 471 also compares complete JPEG bytes against the last successful picture before decoding. Native mode reuses its recovery JPEG. VGA temporarily retains a free input slot and relinquishes it when reception needs the space. In independent 15-second Windows attachment runs, VGA received 48 complete JPEGs, skipped 28 byte-identical repeats, decoded 19 and superseded one. Native received 56, skipped 32 repeats, decoded 16 and superseded seven queued images. One received frame was still queued or decoding at the native sample. The panel presented a nonblack picture in each mode. Neither session reported a JPEG error or an input drop due to occupied slots. Windows reported a 640×480 desktop source in both sessions. The composite display offered the matching advertised signal mode in each. These runs measure decoder workload during attachment. [Board counters and Windows modes](../../../evidence/build/usb-monitor-repeat/after.json) · [Receiver regression tests](../../../evidence/build/usb-monitor-duplicate-jpeg/regular/run.log)

On unit 1, moving a 16×16 marker horizontally by 448 pixels reduced the source area submitted for refresh from 7,424 to 512 pixels. All 56 locally supplied JPEG frames reached presentation, and both positions matched across scanout samples. Excluding the initial frame from each of eight sessions, median receiver time changed from 121.3 to 121.9 ms at 640×480 and from 184.9 to 183.3 ms at 1024×600. Native JPEG decoding and pixel comparison together still took 147.4 ms. The experiment measured on-board processing and refresh. Latency from a Windows pointer movement to visible LCD output remains open. [Board measurements](../../../evidence/build/usb-monitor-horizontal/summary.json) · [Scaled portrait console tests](../../../evidence/build/usb-monitor-horizontal/fit.log)

On the same 7B a second trial compared the production word scan with a 128-bit vector scan. Each combination of resolution and scan method had 12 timed moves after the initial frames. Both scans selected 512 source pixels and reproduced the reference scanout samples. Median pixel comparison took 31.7 ms with words and 32.8 ms with vectors at VGA, and 61.4 ms with words and 60.4 ms with vectors at native resolution. The small difference reversed direction between resolutions. Production keeps the word scan. In native mode the vendor JPEG block processor alone took 81.2 ms with either scan. [Per-stage measurements](../../../evidence/build/usb-monitor-vector-scan/summary.json) · [Firmware restored after the trial](../../../evidence/build/usb-monitor-vector-scan/restored.json)

Marking the LVGL display timer ready after selecting a changed USB frame reduced the median VGA wait before refresh from 9.43 to 0.17 ms. The 90th percentile fell from 14.66 to 0.19 ms. Median selection through refresh completion fell from 22.36 to 20.93 ms at VGA and from 28.48 to 27.20 ms at native resolution. Median on-board receiver time remained 107.3 ms at VGA and measured 190.7 and 187.0 ms at native resolution. All 104 locally supplied JPEG frames reached presentation. Each resolution and policy contributed 24 timed moves plus two initial pictures. Physical timing covered the standalone monitor. Simulator tests checked the same scheduling behavior for the portrait viewport, including that an idle poll or unchanged image preserved the existing display deadline. [7B measurements](../../../evidence/build/usb-monitor-ready-refresh/summary.json) · [Standalone regression test](../../../evidence/build/usb-monitor-refresh-ready/regular.log) · [Portrait regression test](../../../evidence/build/usb-monitor-refresh-ready/console-test.log)
