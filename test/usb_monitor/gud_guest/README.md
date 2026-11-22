# Linux GUD driver integration test

This test boots a signed Arch Linux 6.18.64-1-lts kernel in QEMU TCG. Both ends of its full-speed USB connection live in guest memory. The gadget runs the production KVMpad GUD receiver behind FunctionFS. The Linux DRM client creates a 640×480 XRGB8888 framebuffer, sets the CRTC and submits a damage clip. The receiver checks every reconstructed monochrome pixel after each update.

On each cold boot the fixture checks that the guest leaves the sentinel identity `ffff:ffff` unbound. It then adds a temporary match to the guest's GUD driver. After a simulated cable disconnect it checks that the old DRM card disappears. On reconnect it verifies a new USB address, the driver binding and both test images without registering the identity again. Finally it unloads and reloads the driver and confirms that the temporary match is gone. The test uses an in-memory USB controller. For tests on the 7B LCD, see [USB and firmware compatibility](../../../docs/COMPATIBILITY.md).

Before opening the DRM card, the guest writes two different messages to the active Linux virtual terminal (`/dev/tty1`). It reads the console font with `KDFONTOP` and compares every reconstructed pixel at the USB receiver with the expected glyphs on a black background. Each `GUD_VT_MATCH` line records the text, font dimensions and number of lit pixels. This exercises the Linux framebuffer console in the initial RAM filesystem, before login and before any graphical session starts.

From this directory, set `OUT` to a dedicated build directory outside the repository. The fixture needs Python 3.10+, GnuPG `gpgv`, `zstd`, Clang with LLD, and `qemu-system-x86_64`. The first two commands fetch and verify the distro packages. Once prepared, each run is offline.

```sh
python fetch.py "$OUT"
python prepare.py "$OUT"
python run.py "$OUT"
```

Use `--gpgv PATH` on the first two commands and `--clang PATH` or `--qemu PATH` on the last command when a tool is outside PATH. The optional `fetch.py --audit-debian-kernel` also downloads the Debian Bookworm generic kernel. In that case `prepare.py` records its configuration for GUD and the virtual USB controller.

`guest.log` records the USB probe, mode negotiation and DRM framebuffer dimensions. A passing run shows zero pixel differences for the complete checkerboard. After a 21×9 update at (619,471), Linux sends a 24×9 rectangle beginning at (616,471) because R1 packs eight pixels per byte. The second comparison covers the whole frame, including the unchanged pixels next to the damage. Package signatures and SHA-256 digests are saved in the same directory.

## One physical USB target

Use a board in its explicitly selected GUD personality with an authorized dedicated VID:PID. On Windows the opt-in engineering firmware built with `-DKVM_GUD_GUEST_CHECK=ON` supplies the in-box WinUSB binding for QEMU while GUD is selected. The host needs QEMU with `usb-host`, its libusb library and the current libusb bus/address of that interface. The fixture uses the existing prepared kernel and sysroot. The physical guest contains the DRM client, GUD and its compression dependency. It has an xHCI controller, no emulated gadget, network adapter or disk.

```sh
python -B physical.py "$OUT" "$RUN" --attach --vidpid "$VIDPID" --serial "$SERIAL" --hostbus "$BUS" --hostaddr "$ADDR"
```

Use a fresh output directory each run and keep the cable connected throughout. The launcher checks the product string and 12 uppercase hexadecimal serial digits via libusb before starting QEMU. QEMU filters on all four bus/address/VID/PID fields. Inside the guest, the fixture rechecks identity, 12 Mbit/s and the complete USB configuration. Both accepted layouts have the same vendor interface 0 and dedicated 64-byte bulk OUT endpoint. The optional composite adds exactly a boot keyboard on interface 1 and a boot mouse on interface 2. The guest records automatic GUD binding or adds an ephemeral match after checking the device. The DRM node must belong to interface 0.

Add `--require-controls` to require the composite layout and Linux input registration on those two interfaces. Each must be bound to `usbhid`, have a `hid-generic` child and expose a distinct evdev node under that child. The keyboard must advertise A and Enter. The relative mouse must advertise X, Y and its left button. The signed guest kernel already includes the required HID and evdev drivers. This check reads registration and capabilities without generating events.

For the fixed-input hardware acceptance test, pass `--check-input-events` with `--attach`. It implies `--require-controls` and `--console`. Use an isolated guest with the complete USB device assigned to it. After the guest opens and exclusively grabs the two verified evdev nodes, the launcher publishes `ready-tty1.json` with `input_gate: grabbed`. Only then run the existing board UART probes `hid probe keys` followed by `hid probe move`, and qualify the tty1 image as described below. The observer checks twelve F13–F24 down/up pairs and the four relative moves +300 X, −300 X, +200 Y, −200 Y across split reports. It emits a pass or rejection marker without logging keycodes or raw events. The second message waits for both the first image acknowledgment and successful input observation. Exiting the observer releases both grabs.

The observer also opens the keyboard interface's hidraw node and reads its current USB input report through `HIDIOCGINPUT`. The keyboard must be neutral before and after the exercise. At least one control read must match the key currently held by the fixture. The aggregate count can be below twelve when guest scheduling delays a read past the end of a press.

To check the guest offline, run `python -B check_controls.py "$OUT" "$FRESH_CONTROL_DIR"`. This verifies the production descriptor gate against exact and malformed wire layouts, then checks Linux HID bindings on a composite configfs device on `dummy_udc.0`. Configfs assigns its own endpoint and string numbers. With `--exercise-events`, the guest checks key presses, releases and mouse movement first with GUD active and again with only the boot keyboard and mouse. Each configuration must reject an out-of-order key, an incorrect mouse direction and a stale keyboard control report. To run the input check on a connected 7B in its startup mode, follow the [Linux hardware test](../../hil/README.md#verify-keyboard-and-mouse-input-on-linux).

Add `--require-autobind` to the same command to qualify stock Linux driver matching. The guest reads the **actual USB interface modalias** from sysfs and compares it with the alias patterns from the shipped `gud.ko`. When the driver is unloaded, the fixture runs `modprobe` with that exact modalias using the module index in the guest. A successful run records `GUD_MODALIAS`, `GUD_LOAD modalias` (or `preloaded`) and `GUD_BIND upstream`, verifies the bound DRM node, then proceeds to the two image checks. A missing alias or failed bind stops the test before drawing. Strict mode never writes `new_id`. This exercises USB driver matching and alias-based module loading on the pinned kernel. A desktop distribution's hotplug daemon runs outside this minimal guest.

The guest draws a checkerboard and holds the framebuffer open. After `ready-full.json` appears, capture one fresh `ui snap 4 compact` through the independent board console into `full.log`. Run the verification command below. On `ready-edge.json`, capture again into `edge.log` and run the second command. Each ready record has a fresh run token and an explicit phase. The default deadline is 120 seconds per phase. The verifier rejects an older capture file, incomplete rows, a different source, a bad framebuffer hash or a mismatched pixel. Preserve both capture logs alongside the guest log.

```sh
python -B ack_scanout.py "$RUN" full "$RUN/full.log"
python -B ack_scanout.py "$RUN" edge "$RUN/edge.log"
```

The comparison covers all 19,200 sampled pixels inside the centered 640×480 LCD viewport. The 21×9 patch changes ten sampled pixels. This measures the scanout framebuffer at divisor four. The independent virtual test checks all 307,200 reconstructed USB receiver pixels.

To qualify the launcher without a board, run the following against the same prepared inputs. It checks the two-phase handshake on the virtual receiver, an empty xHCI bus, a different serial and a stale acknowledgment. Two strict cases run exclusively on `dummy_udc.0` inside QEMU. The lab ID `1209:0001` has no matching module alias and must be rejected. A simulated reference device using `1209:4fb3` must bind through the shipped alias and pass both framebuffer checks. The latter qualifies the fixture; it conveys no VID:PID allocation or physical KVMpad result. Each virtual receipt is marked `virtual-receiver`. Only `--attach` adds a host USB backend. The tests accept the same `--clang` and `--qemu` overrides as the original fixture.

```sh
python -B check_physical.py "$OUT" "$FRESH_TEST_DIR"
python -B -m unittest test_physical
```

## Pre-login messages on tty1

Add `--console` to the physical launch command to use Linux fbcon. Before writing, the guest verifies that tty1 is active and mapped to the framebuffer belonging to the selected GUD USB interface. It writes “KVMpad Linux console”, then “Recovery console ready” to `/dev/tty1`. This mode uses the existing USB binding policy and does not open a DRM card or submit a userspace framebuffer.

Wait for `ready-tty1.json`, save a fresh board `ui snap 1 compact` as `tty1.log` and run the first command. Wait for `ready-tty2.json`, capture `tty2.log` and run the second command. Both messages use the same terminal. The guest holds each one until its own acknowledgment arrives.

```sh
python -B ack_scanout.py "$RUN" tty1 "$RUN/tty1.log"
python -B ack_scanout.py "$RUN" tty2 "$RUN/tty2.log"
```

The ready JSON records the active kernel font's width, height and expected glyph image, packed high bit first per row. The verifier checks the entire 1024×600 scanout checksum and compares all 307,200 pixels in the 640×480 viewport starting at (192,84). The area outside the viewport belongs to the board UI. In loopback, the virtual receiver independently checks every reconstructed USB pixel before the corresponding ready signal.

## Switching the active display mode

Add `--modeset` to the physical launch command to request 640×480, then 800×600, then 640×480 again on the same open DRM card. The client reprobes the connector as DRM master, requires preferred VGA and offered SVGA, and reads back the CRTC mode and framebuffer after each change. The default drawing exercise also supports a board offering only VGA.

The six capture phases in order are `vga-full`, `vga-edge`, `svga-full`, `svga-edge`, `return-full`, and `return-edge`. After each `ready-<phase>.json`, save a fresh `ui snap 1 compact` and run `python -B ack_scanout.py "$RUN" <phase> <capture.log>`. Each acknowledgment checks the 1024×600 scanout checksum and every pixel in the source viewport. The upper-left corner is (192,84) for VGA and (112,0) for SVGA. A source pixel is white when `(x // 13 + y // 11) % 2 == 1`. The return to VGA inverts the checkerboard. Each edge phase additionally inverts the 21×9 bottom-right corner.

For an offline run use `python -B run.py "$OUT" --work "$FRESH_TEST_DIR"`. It checks all six images reconstructed from real USB transfers in a virtual Linux guest, including SVGA's two bands, and separately runs the original drawing exercise against an emulated VGA-only offer. `check_physical.py` also exercises the six external acknowledgment gates on virtual USB.

## Display blanking and wake

Use `--attach --blank-wake` for the Linux DRM power-cycle exercise. The guest
sets a 640×480 checkerboard, disables the CRTC, then restores the same mode
and framebuffer. For each of `lit`, `blank` and `wake`, wait for its ready
JSON, capture `ui snap 1 compact` on the board and pass that log to
`ack_scanout.py`. The verifier checks every viewport pixel for the checkerboard,
a completely black image, then the original checkerboard. The local controls
outside the viewport remain available while the host disables its output.

## Numbered frames at 2 Hz

Append `--motion` to the usual physical launch command to submit twenty fullscreen DRM damage updates at 640×480. The fixture retains all target, binding, mode and DRM ownership checks. After the initial checkerboard is displayed by the client, the guest prints `GUD_MOTION_READY` and waits. When the independent scanout sampler is ready, atomically copy the contents of `ready-motion.json` to `start-motion.json` in the run directory. The launcher checks the run token, target and phase and forwards the ASCII command `START` over the guest console.

Frames are numbered 1 through 20. Odd frames have a white body, even frames a black body. In the first 64 source rows the first six 96-pixel-wide columns encode bits 0 through 5 of the frame ID, least significant bit on the left. The remaining 64 pixels in each header row have the body color. For a `ui snap 16 compact` sample at column `u` and row `v`, the source position is `x = 16*u - 192`, `y = 16*v - 84`. Check only pixels for which `0 <= x < 640` and `0 <= y < 480`.

The guest schedules against absolute 500 ms deadlines on its monotonic clock. Each `GUD_MOTION_FRAME <id>` line is emitted after filling the framebuffer immediately before its fullscreen dirty ioctl. The launcher records `id` and `host_ns` (host `time.perf_counter_ns()` at serial line receipt) in `motion-frames.jsonl`. When all 20 ioctls return successfully, it publishes `held-motion.json`. Frame 20 stays selected until the sampler copies that record atomically to `ack-motion.json` and the launcher forwards `ACK motion`. Use a temporary file and rename it inside the run directory for each control record. The ACK releases the framebuffer. A board measurement needs its own timestamped, validated scanouts. Guest submission times alone do not measure LCD presentation or motion-to-glass latency.

For an offline check, the following command verifies every pixel received over guest-local USB for all twenty frames, both gates and the timestamps. It saves `guest.log`, `motion-frames.jsonl` and `verification.json` in a fresh directory.

```sh
python -B check_motion.py "$OUT" "$FRESH_MOTION_DIR"
```

### Verify saved physical motion runs

```sh
python -B verify_motion_scanouts.py /path/to/wire1 /path/to/wire2
python -B -m unittest test_motion_scanouts test_physical
```

The verifier reads `guest.log`, `motion-frames.jsonl`, `scans.json` and every indexed `scan-NNN.log`. It recomputes the complete 64×37 RGB565 checksum, decodes the frame ID from its header and checks all 1,200 sampled viewport pixels against that frame. The other 1,168 samples must stay equal to the first capture. Numbered frames must progress through IDs 1 to 20 in order; repeated samples of an ID are expected. Before frame 1, a scan must match the initial checkerboard at every viewport sample. The tool reports the number of those initial scans separately.

If the collector also saved an independent pre-START `baseline.log`, it is verified as a checkerboard and excluded from the numbered sequence. A summary value alone cannot verify that baseline. The tool reads the saved evidence without opening USB or UART and writes its results to standard output.

## Verify an extended X11 desktop

Run `python -B verify_xorg_scanouts.py "$RUN"` on the saved guest log and the three full LCD scanout logs (`primary.log`, `first.log` and `second.log`). The fixture uses an 800×600 virtual primary output and the 640×480 USB output at desktop coordinate (800,0). Xinerama exposes one 1440×600 root window. The same 160×120 white application window starts on the primary output at (20,32), then moves to (877,101) and (1184,273).

For each position the verifier checks the X window ID and compares the root window pixel hash from `xwd` with the expected desktop raster. It independently checks the board scanout checksum and every pixel inside the 640×480 LCD viewport. The first capture must be entirely black in that viewport. In the other two, the window must appear on the board at (77,101) and (384,273), respectively.
