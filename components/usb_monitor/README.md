# USB monitor extension

### BIOS Setup screenshots

The `sdkconfig.uefi-capture` overlay adds **Extensions → USB monitor → BIOS screenshots / USB drive**.
On first use the board creates a 32 MiB FAT16 image at `/KVM-CAP/CAPTURE.IMG` on microSD.
The PC sees the private `KVMPADSHOT` volume, a boot keyboard, and a boot mouse.
In Gigabyte Intel 700 BIOS Setup, use **F12 / Capture**, choose that volume if
the firmware asks, and wait for its save confirmation. **View latest** detaches
USB storage, restores normal HID and reads the most recent short-name BMP from
the volume root. Uncompressed 24-bit and 32-bit BMPs are scaled to fit the LCD.
**Start another capture** reconnects the volume. Each displayed image stays
still until the next capture. The firmware screenshot command is available
inside Setup. Early POST and ordinary Windows desktop output use different
paths. [Gigabyte Intel 700 BIOS guide](https://download.gigabyte.com/FileList/Manual/mb_manual_intel700series-bios_e.pdf)

The Windows extended-desktop trial used xfz1986 1.2.25.2. The catalog timestamp failed verification and the graphics DLL chained to an untrusted WDK test certificate. KVMpad does not recommend installing this package. The ordinary USB keyboard and mouse use the built-in Windows HID drivers. [USB and firmware compatibility](../../docs/COMPATIBILITY.md)

While the receiver is open, `ui usb-monitor-status latency` reports microseconds spent receiving
the JPEG payload, waiting for decoding, decoding, waiting for LVGL selection, and finishing the
selected refresh. The last sample and session means use only frames whose final flush reached
the LVGL refresh-complete event. The peak is the longest total for a single presented frame.
Windows capture, encoding and panel scanout fall outside this measurement.

When a cursor moves, the receiver can send its old and new areas to LVGL in one refresh. During the RGB565 comparison it tracks the largest gap of unchanged rows and checks for a common gap of unchanged columns across the affected rows. It chooses the smaller pair of rectangles when they save at least 4,096 source pixels and cover at most half their combined bounding box. The portrait console scales and clips each area for its viewport. In `ui usb-monitor-status redraw`, `split` counts completed refreshes submitted in two areas.

For native RGB565 input, the receiver remembers which 128-pixel runs in the previous picture have a single color. It checks every new pixel against that color and compares mixed runs against the original pixels. This saves reads from PSRAM on desktop backgrounds and flat panels. If fewer than one in eight runs has a reusable color, subsequent frames compare the whole band in one pass until it changes. The 4,800 color tags occupy 9,600 bytes in the eight padding rows of the 1024×608 decoder surface. A full decode or recovery clears the tags before the next comparison.

During USB suspend the panel retains the desktop and the receiver keeps its place in the
current JPEG. `ui usb-monitor-status` reports `mounted=0` and `suspended=1` while that
configured link sleeps. Resume continues reception. A disconnect, bus reset or configuration
loss clears the stream.

Before Windows configures the monitor, the screen identifies the USB data port. Once
configured, it asks the user to select Extend and move a window onto the KVMpad display.
In the 640×480 view, an idle, configured desktop keeps its normal caption. USB suspend displays **USB suspended**
and indicates when the LCD is holding the last picture. A rejected JPEG displays **USB picture
could not be decoded** before the first picture or **USB picture error / Last picture held**
when a desktop is already visible. A successful decode clears the error, and a USB reset
clears it for the new stream.

When the 640×480 desktop stays completely black for one second, the footer suggests moving a window
onto the monitor. Repeated black pictures keep the hint visible below the 640×480 desktop.
A presentation with any nonzero RGB565 pixel clears the hint. USB suspend temporarily hides it.

The `sdkconfig.usb-monitor-eval` profile adds **Extensions → USB monitor**. Open it locally to switch the device USB port to the display-only personality. In the Windows trial, xfz1986 sent the extended desktop to the 640×480 monitor. **Home** closes the monitor and restores the normal HID device. At 640×480, Home remains outside the picture. At 1024×600, the picture fills the LCD. Tap anywhere on the picture to reveal Home for three seconds, then tap Home to leave. [Espressif example](https://github.com/espressif/esp-iot-solution/tree/master/examples/usb/device/usb_extend_screen) · [Windows driver documentation](https://github.com/espressif/esp-iot-solution/tree/master/examples/usb/device/usb_extend_screen/windows_driver)

The vendor interface advertises `R640x480_Ejpg4_Fps10_Bl96` or `R1024x600_Ejpg4_Fps10_Bl96` for the selected session. The firmware receives complete JPEG images through bulk OUT. The 16-byte little-endian header carries the format, rectangle, a 10-bit wrapping frame ID and a 22-bit payload length. Format 3 covers the full rectangle at (0,0). The receiver consumes unsupported records, including the optional zero-length END record. It accepts headers split across transfers and multiple records in one transfer. [Espressif receiver](https://github.com/espressif/esp-iot-solution/blob/master/examples/usb/device/usb_extend_screen/main/app_vendor.c) · [Driver capability string](https://github.com/chuanjinpang/win10_idd_xfz1986_usb_graphic_driver_display)

At 640x480 the session preallocates two RGB565 surfaces and a decoder worker. At 1024x600 the LVGL task decodes between renders into one surface padded to 608 rows. It retains the last good JPEG to restore the desktop if a new frame fails partway through decoding. Both modes reserve one 96 KiB input before switching USB and allocate up to two more inputs from available PSRAM. With an additional 96 KiB working copy, Espressif's S3 decoder writes RGB565 directly into aligned bands. The ROM decoder handles sessions without that working memory and supported fallback cases. With three buffers USB can receive the final desktop change while one picture is decoding and another is waiting. Only a complete replacement displaces the waiting picture. With one or two buffers the receiver can discard a new arrival while all inputs are occupied. A USB reset discards partial input, cancels decoding and clears the picture on the next UI update. Home releases and clears the image storage. The release diagnostic `ui usb-monitor-status` reports USB mount state, queued frames, successful decodes, arrivals rejected while busy, decoding errors, completed LVGL picture refreshes, complete waiting JPEGs superseded by newer pictures, and the number of JPEG buffers allocated for the session.

On the local UART, `ui usb-monitor-status picture` reports `decoded=absent` when the foreground USB monitor has no selected picture, `all_black` when every selected RGB565 pixel is zero, or `has_nonblack` when at least one pixel is nonzero. The response contains only the classification. The receiver separately classifies the visible RGB565 pixels during decoding. The LCD uses that result after the completed refresh to time the black-picture hint. The UART query reads the selected surface directly. To compare it with the LCD framebuffer, hold the source still, run the picture check next to an explicitly requested `ui snap 1`, and verify stable presentation counters across both commands and `SNAP_SOURCE scanout` in the snapshot log. The 640×480 viewport starts at (192, 84) on the 1024×600 panel. The native viewport starts at (0, 0). A black decoded surface can originate in the input JPEG or in decoding.

In the synthetic diagnostic build, open Extensions and run `ui usb-monitor-test`. The normal menu button opens the view with USB switching suppressed. A local red/blue test image passes through the production receiver and decoder. The test waits up to three seconds for the final LVGL flush and samples 16 pixels in each available board framebuffer. It reports a matching framebuffer mask and returns Home. A framebuffer match measures the scanout source. Optical panel output and Windows display enumeration require separate hardware checks.

To check physical USB enumeration, the diagnostic build accepts `ui usb-monitor-link-test` from the Extensions category screen. The USB port exposes its display descriptors for 15 seconds with the pixel receiver closed, then restores HID. During that interval the Windows test accepts `--usb` after the board serial to read the configuration and capability strings through the USB hub. This check also works before installing the Windows display driver.

The engineering command `ui usb-monitor-desktop-test` opens the same Extensions button with the Windows driver and LCD receiver enabled. The trial returns to Home after 75 seconds. Home can also end it early. Run the Windows topology probe with the board serial during the trial. It reports the Windows GDI source name and the extended desktop dimensions.

With the additional CMake option `KVM_USB_PICTURE_CHECK=ON`, the color fixture and timed desktop test also decode each successful JPEG with an independent implementation. The mask-ROM decoder checks frames rendered by Espressif's accelerated library. Espressif's block decoder checks frames rendered by the ROM fallback. The engineering reply includes `reference` and `sequence`. A nonzero sequence identifies the picture during that trial. The reference covers the visible rectangle and reports `failed` if the independent decode cannot finish. Compare both classifications and the sequence before and after a snapshot while presentation counters hold steady. The normal monitor processes each accepted frame once.

For a full-resolution local UART capture, use `ui snap 1 compact` and decode the console log with `test/hil/ui_snapshot.py`. Repeated RGB565 pixels are sent as runs. The output PNG retains every native pixel. The USB pattern runner requests compact rows at one-quarter scale and also reads the literal rows emitted by older firmware.

### Local cursor motion benchmark

An engineering build with `KVM_USB_MOTION_CHECK=ON` and `CONFIG_KVM_USB_MONITOR_EVAL=y` can compare two redraw strategies on the physical LCD. Open Extensions, then run `ui usb-motion start 640x480 box`. The four regular HID interfaces stay active. `ui usb-motion next` supplies one fixed JPEG through the production receiver. The first frame establishes the background. Subsequent frames move a white 16×16 marker between (32,32) and (480,288). After each completed presentation, `ui usb-motion check` reports which board framebuffer contains the expected marker and cleared background at the two positions. Read `ui usb-monitor-status latency` for the last completed frame in microseconds. The refresh field covers frame selection through the final LVGL flush. The sum includes local submission and decoding.

Use `ui usb-motion stop` to return to Extensions. Repeat with `split` to exercise the normal two-region path and with `1024x600` to measure native mode. Compare equal numbers of alternating movements, excluding the first frame of each session. The source runs locally on the board, so this benchmark excludes Windows capture, physical USB transfer and the optical response of the panel.

For pixel-comparison timing, append a scan method and a scene, for example `ui usb-motion start 1024x600 split packed desktop`. The methods are `scalar` (the previous implementation) and `packed` (the production implementation). The scenes are `cursor` (black background) and `desktop` (colored window and text-like rows). Both use identical marker coordinates. The `check` reply gives microseconds in the vendor JPEG block processor, nonblack-pixel detection and damage comparison for the completed picture. These three fields exclude frame setup and teardown.

The `sideways` scene moves the marker between (32,32) and (480,32) on the colored desktop. For this scene, `box` uses the previous row-only comparison and `split` enables the production column comparison as well. Both positions can be checked in the LCD framebuffer.

On ESP32-S3, `ui usb-motion start 1024x600 split vector sideways` compares the same aligned RGB565 rows using 128-bit instructions. Run again with `packed` for the production comparison. Each start checks equality and then flips one byte at each of 128 offsets to verify that both groups detect every change. The vector implementation is linked only in engineering builds.

The standalone monitor and the portrait viewport check for pictures every 8 ms in both resolutions. VGA starts its decoder worker when USB completes a frame. The next poll selects the decoded picture. Native mode starts a waiting decode on the poll itself. Both views mark the LVGL display timer ready when they select a changed picture for repaint. Idle polls and identical decoded pictures keep the ordinary display schedule.

To compare selection intervals on the board, run `ui usb-motion start 640x480 split packed sideways poll25` and repeat with `ready`. The start reply reports the actual timer period in milliseconds. In `ui usb-monitor-status latency`, the `ready` stage measures time from completed decode to frame selection. To compare display scheduling after selection, use `period` and `ready`. The engineering `check` reply reports `wait_us` from selection to the LVGL refresh start and `draw_us` from refresh start through the final flush.

To compare native pixel reads on the board, run `ui usb-motion start 1024x600 split packed sideways` and repeat with `cells` in place of `packed`. The `sideways` fixture moves the cursor across a colored window. Use `texture` for the same motion on a textured background and `alternate` for changes covering the whole picture. The comparison uses the same decoder and redraw rules in both runs.

### Windows paint to LCD refresh

For a missing first picture, sample `ui usb-monitor-status input` before opening the monitor and again while waiting. The vendor OUT callback counts passed and filtered bytes across USB configurations, modulo 2³². The receiver counts wire bytes, complete headers (including empty END records), and nonempty records rejected on their first payload byte. It also reports the bytes buffered for an incomplete header and the bytes still expected in the current payload. These receiver counters reset with the stream. Compare them with the queued, decoded and presented frame counts. Zero endpoint deltas mean the callback observed no payload during that interval.

Run the following from the active Windows desktop with the standalone USB monitor open on the 7B. The probe identifies the unit by USB serial in either display-only or composite mode. The driver must provide an independent desktop matching the requested resolution and USB signal. Keep the pointer on another screen during the trial.

```text
python -I test/hil/usb_monitor_latency.py --probe path/to/usb-monitor-windows.exe --serial BOARD_USB_SERIAL --port COM3 --result latency.json
python -I test/hil/usb_monitor_latency.py --mode 1024x600 --probe path/to/usb-monitor-windows.exe --serial BOARD_USB_SERIAL --port COM3 --result native-latency.json
```

For the installed **USB Monitor Console**, start its USB monitor locally, show the whole picture with **Fit**, and close the gesture drawer. Run the same command with `--portrait-console`. Both the Console and Pointer pages use the same monitor area. The runner reads the physical RGB565 framebuffer in board scan order and checks the four colors inside the rotated viewport. The colors identify the displayed phase and orientation. Select Fit before starting because a centered zoom can display the same four colors. Each sample also records the board's receive, queue, decode, ready and refresh times in microseconds. The timing record and the verified picture must belong to the same presentation. The runner leaves the extension open.

The probe paints eight alternating static color patterns on the board's Windows display. Each interval starts immediately before GDI paints the new pattern. The runner brackets one completed LCD refresh with Windows performance-counter timestamps around local UART reads. It then verifies the expected colors on the submitted framebuffer and checks that the same presentation is still selected. The JSON records lower and upper millisecond bounds for each step and for the median. These bounds include the Windows driver and USB transfer. Physical input and the panel's optical response require separate measurements.

The optional `--open-from-extensions BOARD_CERT_SHA256` verifies the UART board, opens the requested monitor mode from Extensions and returns there on exit. With `--match-opened-desktop path/to/KVMpad.Client.exe`, the runner can also set the Windows desktop to the resolution of the USB signal it just opened. For a borrowed monitor session it observes the existing configuration and leaves the session open. The probe checks the target on every paint. At 1024×600 it requires an actual native source and active signal, and the verifier samples the full panel. At 640×480 it samples the centered VGA viewport.
