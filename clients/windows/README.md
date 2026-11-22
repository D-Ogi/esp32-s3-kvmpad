# KVMpad Windows CLIENT

The Windows x64 MSI includes the desktop runtime and board firmware tool. Open **Tools → Board firmware** for first installation, recovery or a signed update. [Installation and deployment](../../docs/INSTALL.md)

Run KVMpad CLIENT on the operator PC to send input over Wi-Fi. The KVMpad board forwards the keystrokes and mouse movements to the **target computer** over USB. The target uses its built-in keyboard and mouse drivers. [Pairing screenshots](../../README.md#pair-a-windows-client) · [Remote input example](../../README.md#send-keyboard-and-mouse-input)

The protocol specification calls the USB target the **HOST** and the board the **DEVICE**. The Windows application named **KVMpad HOST** is a companion that CLIENT starts on its own PC. Screen sharing captures a monitor on that PC. [Windows HOST guide](../../hosts/windows/README.md)

## Direct SSH input

The 7B firmware includes the SSH server. Enable remote access on KVMpad. Pair the Windows CLIENT with the board.

1. In **Devices**, connect to KVMpad.
2. Select **Host input → SSH command…**. Select your OpenSSH public key (`.pub`).
3. Run the command from CLIENT in PowerShell. Tap **Allow** on KVMpad. Keep CLIENT connected during the session.

CLIENT binds the public key to the current connection. It checks the board's SSH host key against the fingerprint from the paired TLS connection. OpenSSH uses the matching private key from your SSH agent or the file next to your public key. The command connects to TCP port 22 as user `kvmpad` and requests the `kvmpad` subsystem. Send one command per line.

Windows OpenSSH Client supplies `ssh.exe` and `ssh-keyscan.exe`. On first use, CLIENT checks the SSH host key against the fingerprint from the paired connection and saves it under `~/.ssh/kvmpad/`. On later connections, CLIENT verifies the saved key against the current fingerprint. The generated command uses that file for strict host verification even if the board's IP address changes.

For example, open a text editor on the target. Place the pointer over its text area. Enter these lines in the open SSH session on the operator PC. The first two lines click the editor. The next two press and release Enter to insert a line break.

```text
mouse down left
mouse up left
key down 0x28
key up 0x28
```

## Layout

| Path | Target | Contents |
|---|---|---|
| `src/KVMpad.Client.Core` | `net10.0` | Protocol v1 codec and session state machine, input rate limiter and coalescing, certificate pinning, scan-code → HID usage map, capture ledger, type text, mDNS discovery, the pairing run. No UI and no NuGet dependency; its one native dependency is the pairing library below, loaded by name. |
| `layouts/kbd_layouts.json` | data | The generated host layouts of ADR-024 (581 layouts, about 4.5 MB), produced by `tools/kbd_layouts/gen_layouts.py` — the same file the device compiles into its firmware. Read-only here: never edited by hand, and regenerated only by that tool. Both the application and the test project copy it to `layouts/kbd_layouts.json` beside their binary, which is where `HostLayoutCatalog` looks for it. |
| `native/` | Windows x64 | `kvmpad_pairing.dll`: the pairing core of ADR-029 — `components/secure_remote/pairing/**` — compiled against the TF-PSA-Crypto of the pinned ESP-IDF v6.1 tree, plus a flat C ABI for P/Invoke. Also `kvmpad_pairing_test.dll`, a test fixture that is never shipped. |
| `src/KVMpad.Client.App` | `net10.0-windows` | WinForms window, tray icon, dialogs, and the Win32 layer: `WH_KEYBOARD_LL`, Raw Input, session/power notifications, Credential Manager. |
| `test/KVMpad.Client.Core.Tests` | `net10.0` | xUnit tests for everything in Core. No test contacts a device or any network service; the TLS tests complete a real handshake on the loopback interface against a certificate the test itself creates. |

## Build and test

There are two build steps and the native one comes first, because the managed tests load its output.

The build example keeps the package cache and `dotnet` CLI home in the workspace. Build outputs go in `bin/` and `obj/`.

### 1. The native pairing library

ADR-029 compiles one pairing implementation twice, so this builds the firmware's own
`components/secure_remote/pairing/**` sources against the crypto the firmware links. It needs CMake, Ninja
and the llvm-mingw toolchain already in `../.tools/`; the CMake build directory stays outside the repository
and the two DLLs land in `native/bin/`, which is gitignored.

```bash
export PATH="$(cd ../.tools && pwd)"/llvm-mingw-20261020-ucrt-x86_64/bin:$PATH
cmake -S clients/windows/native -B ../build-win-pairing -G Ninja \
      -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang
cmake --build ../build-win-pairing
```

Two artifacts come out, and the split is deliberate (ADR-028 §4):

- `kvmpad_pairing.dll` — what the application ships. The CLIENT half of the exchange only, drawing randomness
  from `BCryptGenRandom`.
- `kvmpad_pairing_test.dll` — **a test fixture; never ship it.** It adds a driver for the DEVICE state
  machine and the seeded generator from `test/pairing/det_rng.c`, which is what makes the ADR-029 interop
  vectors reproducible. Both belong in a test and neither belongs in an application that controls someone's
  computer, so they are kept apart at build time rather than behind a runtime switch. The shipping DLL
  exports no `kvmpad_test_*` entry point and does not compile `det_rng.c` at all; see ADR-028 §4 for what
  that does and does not guarantee about the DEVICE code itself.

`kvmpad_pairing.dll` has to sit beside `KVMpad.Client.exe` at run time. Without it the application starts and
can still connect to an already-paired device; only pairing is unavailable, and it says so with the build
command rather than failing with a bare loader error.

### 2. The managed solution

PowerShell:

```powershell
$env:NUGET_PACKAGES = [IO.Path]::GetFullPath('../.tools/nuget')
$env:DOTNET_CLI_HOME = [IO.Path]::GetFullPath('../.tools/dotnet-home')
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = '1'
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_NOLOGO = '1'

dotnet build clients/windows/KVMpad.Client.sln -c Release
dotnet test  clients/windows/KVMpad.Client.sln -c Release
```

Run both commands from the repository root with the .NET 10 SDK for Windows x64.

A single test class, while narrowing something down:

```powershell
dotnet test clients/windows/test/KVMpad.Client.Core.Tests/bin/Release/net10.0/KVMpad.Client.Core.Tests.dll `
  --filter "FullyQualifiedName~PairingTests"
```

## Device connection and appearance

For a USB extended desktop, start the USB monitor on the KVMpad, then open **Tools → USB display…** in CLIENT. This works over USB independently of the network connection. The dialog shows the desktop size and, when different, the active USB signal size. Select a resolution offered by Windows and choose **Apply**. The change applies to the current session. **Windows display settings** opens the system controls for choosing Extend or arranging the displays.

When the USB signal is already 1024 × 600 and Windows still uses a 640 × 480 desktop, choose **Use 1024 × 600** if Windows offers it. After a signal change the dialog waits for Windows to list the new desktop mode. If it remains unavailable, the dialog shows both sizes. Choose **Refresh** to check the offer again. After a successful **Apply**, the open dialog continues to read the active display. If Windows reconnects the KVMpad at a different size, the dialog shows the requested and current desktop sizes. Applying another change requires pressing **Apply** again.

We do not recommend the xfz1986 Windows display driver (1.2.25.2). The package uses an untrusted test certificate, and we cannot verify which source code produced the binary. [Windows display compatibility](../../docs/COMPATIBILITY.md) For either **640 × 480** or **1024 × 600** in the same dialog, connect CLIENT to the paired KVMpad with its USB monitor already active, then choose the size and **Apply**. CLIENT requests the signal size over the existing session, follows the same physical board across USB reconnection and applies the matching Windows desktop mode. Release remote input control before changing the signal. The feature works with the standalone USB monitor and the portrait USB Monitor Console extension.

When the KVMpad sits to the left of or above another monitor, CLIENT can keep their shared edge in place as the KVMpad desktop grows or shrinks. The other displays keep their positions. If the new size would still overlap a display or break the layout, the dialog links to Windows display settings.

For command-line use with the GUI session disconnected, run `KVMpad.Client.exe --usb-resolution DEVICE-ID 1024x600` or select `640x480`. Use the paired ID listed by `--pack-devices`. A native USB signal can carry a smaller Windows desktop. After applying a change, check both the desktop and USB signal sizes.

If the KVMpad already advertises the desired USB signal, run `KVMpad.Client.exe --usb-match-signal USB_SERIAL 1024x600` from the active Windows desktop. Use the 12-digit USB serial shown in **Tools → USB display…**. This command uses the local Windows display API and also accepts `640x480`. It verifies the board serial and signal, applies an offered Windows desktop size, then checks both dimensions. A matching desktop requires no mode change.

On **Devices**, select the paired board and choose **Connect**. The CLIENT starts the HOST for that connection. When Cursor Magnifier requests a display, Windows opens its monitor picker. Choose the monitor, move the pointer onto it and confirm its name on the KVMpad.
Open **Host input** to request control, start capture, send a shortcut, or type text. The connection status stays visible on every tab.
If discovery is unavailable, choose **Advanced…**, select the saved KVMpad, enter its address and port, and choose **Connect**.
For first-time setup at a known address, select **Pair a new KVMpad**, open the pairing window on the board and choose **Continue to PIN**.
Connect, Extension Studio and the standalone HOST look up the paired board before connecting. They use its saved address when discovery is unavailable. Advanced uses the address and port you enter.
If Windows selects a proxy and the TCP connection is refused, the CLIENT reports the proxy route. The KVMpad handshake has not started at that point.

Windows controls use the system light, dark or contrast palette. Icons keep their text labels and scale
with display DPI. On KVMpad, open the gear in the top bar and choose **Settings → Appearance**.
Select **Day**, **Night** or **High contrast**. The same pane controls the panel backlight.
[Shared control icons](../../design/assets/control-icons)

## Host layouts

CLIENT uses the keyboard layout reported by the board. If the board does not report a layout, CLIENT uses `HostLayoutId` from its settings file.

`HostLayoutCatalog` (Core, `TypeText/HostLayoutCatalog.cs`) indexes `layouts/kbd_layouts.json` once — id, name,
geometry and the two flags per entry, plus the byte range of that entry — and builds a character table only for
the layout actually in use, so the 4.5 MB file costs one read and no bulk deserialization. A character is one
key press, or two where the layout reaches it through a dead key; `TypeTextPlanner` presses and releases each
of them in turn, so any prefix of a run is still balanced. The level the generator records is the modifier
combination that reaches the character: bit 0 Shift, bit 1 AltGr (the right Alt usage).

If the file is missing or is not the file this client expects, the catalog stays empty, says so in one sentence
that never quotes the file, and the layout in use can type only the keys every layout shares (space, tab,
Enter). Everything else is reported as unavailable rather than guessed at (FR-702).

## Rules that apply here

- The CLIENT never emits USB HID. It sends typed intents; the DEVICE's `InputArbiter` and `HidScheduler`
  decide what reaches the HOST.
- No `ws://`, and no setting that skips certificate verification (TECH-041).
- Keystrokes, typed text, usages, tokens and pairing codes are never written to a log, a file or an exception
  message (TECH-046, SEC-105, FR-705). Diagnostics carry counts.
- Every exit from capture mode releases what is held, in reverse press order.
- Feedback-loop guard: when the KVMpad is plugged into the CLIENT machine itself, everything the client forwards
  comes back from the KVMpad as local input. The client therefore drops Raw Input from a KVMpad's own mouse, refuses
  capture while any KVMpad (USB `VID_303A&PID_400C`, `KvmpadUsbIdentity`) is attached to the CLIENT machine, and stops
  capture (`ReleaseReason.LoopGuard`) when one is plugged in during capture. The keyboard hook cannot tell devices
  apart, so refusing capture is the only safe answer. Pairing, status and type text are not affected.
- The pairing protocol is not reimplemented here. `components/secure_remote/pairing/**` is read-only from
  this directory: `native/` compiles those sources, and a change needed there is a change to TASK-100, not a
  local patch. The same goes for `test/pairing/`, whose fixture and vectors the native test DLL reuses.
- Every authored source file carries the three-line license header (ADR-025), checked by
  `python -I tools/check_license_headers.py`. `KVMpad.Client.sln` has no comment syntax and is exempt as a
  generated file.
## Offline Extension Studio

The standalone Extension SDK includes the editor, CLI, examples and Python libraries for the target interpreter. Extract the ZIP, open its directory and run `python -I -B start.py` with the Python version listed in `runtime.json`. The SDK guide walks through creating, previewing and staging a checklist. [SDK guide and build command](../../tools/extension_sdk/README.md)

Choose **Tools → Extension Studio…** in the Windows client, or run
`py -3 tools/kvmpack/kvmpack.py studio` from the repository root. Studio uses Python 3 with Tk,
`jsonschema` and Pillow. Install its dependencies in the interpreter selected by `py -3`.
The packaged client includes the editor, schema files, simulator and four example projects.

Open a source folder or an unsigned `.kvmpack` from **File**, or start from a template. Choose a widget
type below the canvas, choose its **Parent** and select **Add widget**. A Column, Row or Grid can hold
children. Drag a widget on the layout canvas or enter its coordinates relative to its parent in **Widget**.
The Cursor Magnifier template declares its display controls. Flight Yoke demonstrates joystick bindings for the host-side test harness.
Use **Apply widget changes** to update a label,
move an action to an event, or bind a telemetry source or state variable to a property. The Manifest, UI and
Bindings tabs edit the complete JSON documents. **Validate** checks the same rules as the pack CLI.

Select a Button and choose **Import PNG…** to add an icon and apply the current widget edits.
Use **Button icon** to reuse a package image, or select the empty entry to remove it from the button.
The package stores the original PNG. Studio and the device display it beside the caption at up to
32 × 32 pixels, preserving its proportions and transparency. Buttons smaller than 64 pixels wide
or 40 pixels high show the caption alone.

**Simulate** opens a local 1024 × 600 browser surface. Try the checklist buttons, drag the yoke,
release a spring-loaded control, or choose **Demo telemetry** to feed the instrument indicators synthetic
samples. Choose **Appearance → Day**, **Night** or **High contrast** to inspect the current state in another
palette. **System** follows the operating system. **Home** resets the preview. The simulator keeps control
and capture actions local to the preview.
Use **Permissions** to review HOST and USB requirements. **File → Export diagnostics** writes counts,
declared permissions and validation results.

**Package…** writes a portable archive. **File → Save source as…** saves an editable copy in a new
folder. **Install via card…** writes an installer into the selected FAT microSD root and reads it back
to verify the bytes of the validated archive. The CLI command
`py -3 tools/kvmpack/kvmpack.py stage SOURCE --card MOUNT` uses the same path. Each version has its own
installer filename. Eject the card,
insert it in the device, then choose **Extensions → Install from card** on the panel and review the
requested permissions.

For a LAN transfer, pair the board with the Windows CLIENT. Insert a writable card and open
**Extensions → Receive from paired client** on the board. In Studio, choose **Install to paired device…**,
select the paired device and send the archive for review. The board displays the validated pack and its
permissions. Approve on the panel to install. The CLIENT reports success after the board commits.

From the checkout use `py -3 tools/kvmpack/kvmpack.py devices` to list paired IDs, then
`py -3 tools/kvmpack/kvmpack.py install --device PAIRED-ID extension.kvmpack`. The CLI uses the
installed CLIENT executable on PATH or a local Release/Debug build. Signed archive uploads can use
`--verify-key trusted-public.pem` for the desktop validation pass.

Listing paired devices reads the local credential store. Before an install or a standalone HOST
connection, the CLIENT browses for the paired mDNS instance for up to three seconds and uses its
current address and port. If discovery is unavailable or the instance is absent, the CLIENT uses
the saved endpoint. Both paths authenticate with the original certificate pin and pairing token.

Transfer requests share the authenticated, certificate-pinned `/v1/ws` CLIENT transport. A dedicated
connection completes `hello`, then uses `pack.offer` (size and SHA-256), `pack.data` (offset and up to
256 hex-encoded bytes), `pack.end`, `pack.poll` and `pack.cancel`. The device replies with `pack.status`
and the persisted offset. A window waits 60 seconds for a paired sender. Transfer allows 30 seconds
without progress and five minutes total, followed by 90 seconds for physical review. The board accepts
one transfer at a time, bounded to 256 KiB. Closing the connection cancels uncommitted receipt or review.
The final physical approval starts the storage commit. If the connection closes during commit,
check the device library for the result. Denial, cancellation, expiry and validation failures are distinct.

## Prepare an H.264 video for offline playback

Choose **Tools → Prepare card video…** in the Windows client. Select the original video, a destination
folder and a playback mode, then choose **Prepare playback**. The importer copies the original,
verifies its SHA-256 digest and writes the selected companion beside it. An existing identical
original can be reused when preparing another mode. Copy the original and its companions together
to the FAT32 or exFAT card root. In **Extensions → Video files**, select the video and its playback mode.
The importer follows the Windows app colors, including custom high-contrast selections in the playback menu.

An MP4 trimmed between keyframes can retain earlier H.264 samples needed to decode the start of the clip.
The importer prepares the visible portion of the edit and preserves its playback timing. If direct decoding
on the board runs out of memory, the playback screen identifies the mode to prepare in the Windows client.

| Playback mode | Companion suffix | Input |
| --- | --- | --- |
| Exact | `.kvc` | H.264 MP4 or Annex B |
| Reduced | `.kvr` | H.264 MP4 or Annex B |
| Large bank | `.kvb` | H.264 MP4 or Annex B |
| Temporal blend | `.kvf` | Constant-frame-rate H.264 MP4 with presentation timestamps |
| Temporal smart loose | `.kvs` | Constant-frame-rate H.264 MP4 with presentation timestamps |

For example, preparing both temporal modes for `bear.mp4` yields `bear.mp4.kvf` and
`bear.mp4.kvs`. Each contains the display frames and timings for that mode. Reduced, Large bank and both temporal modes
use the KVMpad native reader included in the client's `media` folder and require H.264 cropping
aligned to four source pixels on each edge. If your build omits
the reader, the importer asks you to select an existing KVMpad `card-pixels.exe`.

The importer uses Python 3.11 or newer with Tk and standard FFmpeg 5.1 or newer. Put both
`ffmpeg.exe` and `ffprobe.exe` on PATH before starting the Windows client. The Exact path uses
the Python standard library and the installed FFmpeg tools. Source files may be MP4 with a single
H.264 video track or Annex B `.h264` / `.264`. Supported video is progressive 8-bit SDR YUV420,
up to 1920 × 1080, with square or unspecified sample aspect and BT.601, BT.709 or unspecified matrix.
The device's half-size conversion produces 960 × 540 pixels for a 1920 × 1080 source.
The companion retains every presented picture and its timing.
For reordered frames in MP4, the stock reader uses the file offset reported by ffprobe to associate
each displayed picture with its sample timing.
The board accepts original files up to 2,147,483,647 bytes. The FAT32 companion limit is 4,294,967,295 bytes.

To prepare only the companion next to an original from the checkout, run
`py -3 tools/live_view/prepare_card_video.py "C:\Videos\clip.mp4"`.
Reduced and large-bank modes use the native reader supplied with `--reader` or built with `--ffmpeg-build`.

To include the native reader when building the Windows client from source, first build the
handoff FFmpeg host archives with `tools/live_view/build_ffmpeg.py`. From the repository root,
run `py -3 clients/windows/native/build-card-video-reader.py <host-build-directory>`. A normal
client build or publish copies the resulting `card-pixels.exe` into `media`. For an existing
build outside the checkout, pass `-p:CardVideoReader=<absolute-exe-path>` to `dotnet publish`.
