# Windows HOST bridge

Run this interactive application on the PC attached to the board's USB port. The Windows CLIENT starts the HOST using the selected device's existing PIN pairing. The HOST window grants display, simulator and OBS access for one board and one active profile. **Stop session** or closing the window ends all current grants.

## Start the HOST

Use Python with Tk, `websockets==17.4` and `cryptography==50.0.2`. The CLIENT finds the workspace Python environment or uses `py.exe -3`. Set `KVMPAD_HOST_PYTHON` before starting the CLIENT to select another interpreter. The native picker requires Visual Studio C++ x64 build tools and Windows SDK 10.0.26100.0 or later. Windows must implement the capture picker's monitor-identity interface. WIC encodes JPEG and the picker encodes Cinepak. For H.264, put FFmpeg with the `libx264` encoder on the CLIENT process's `PATH`.

From the workspace above the repository, build the picker and run its physical-coordinate tests.

```powershell
$src = "esp32-s3-kvmpad/hosts/windows"
$bin = ".tools/tmp/host-capture"
& "$src/build-capture.ps1" -OutputDirectory $bin -TestGeometry -TestStudio
```

The CLIENT looks for `kvmpad-capture.exe` in the application's `host` directory and then in the workspace build directory shown above. To include it in builds and publishes, place the built helper in `clients/windows/native/bin`, or pass `-p:HostPicker=<absolute-exe-path>` to MSBuild. To use another build at runtime, set `KVMPAD_HOST_PICKER` to its full path before starting the CLIENT. [Windows HOST launcher](../../clients/windows/src/KVMpad.Client.App/HostCompanion.cs)

1. In Windows KVMpad CLIENT, select the paired device and connect. For a new device, enable remote access on the board, tap **Pair a client** on its LCD, then choose **Discover** and **Pair…** in the CLIENT. Enter the PIN in the CLIENT and accept on the LCD.
2. After the connection is authenticated, the CLIENT starts the HOST on the LAN interface leading to the board. You can stop it from the CLIENT or restart it for the current connection. The CLI accepts the same stored device ID with `KVMpad.Client.exe --host <device-id>`.
3. On the board, open **Extensions → Device tools → HOST Bridge** to see the active endpoint. The same pairing applies to installed profiles. Cursor Magnifier opens the Windows monitor picker for each sharing request. Simulator and application requests use the HOST permission prompt.

The CLIENT keeps the device token in Windows Credential Manager. It validates the board's TLS certificate against the saved fingerprint, then supplies that public certificate to the local HOST process. The HOST creates a temporary TLS identity for this run. The authenticated CLIENT sends its public leaf and port to the board using `host.attach` on its existing `/v1/ws` session. The board obtains the PC address from that connection. Closing the session clears the in-memory endpoint and stops active grants. Starting the HOST again reuses the saved pairing.

To restrict a physical magnifier trial to Monitor 2, set `$env:KVMPAD_HOST_DISPLAY = 'DISPLAY2'` in PowerShell before launching the CLIENT. The HOST approval names the required monitor. Select it in the Windows picker, move the pointer onto it and confirm its full name on the LCD. Selecting another monitor ends that request. The setting accepts uppercase `DISPLAY` followed by one to five ASCII digits, optionally prefixed with `\\.\`. An empty or malformed setting prevents capture. To restore unrestricted monitor selection, run `Remove-Item Env:KVMPAD_HOST_DISPLAY` and launch the CLIENT again.

Use the matching native helper for a restricted trial. It checks the selected monitor before moving its indicator or asking the operator to move the pointer. With an older helper, the HOST checks the earliest monitor identity the helper supplies and requires a matching final identity before offering LCD approval.

## Capture and application providers

- On the LCD, open **Extensions → Installed extensions**, select **Cursor Magnifier** and tap **Open**. To install the supplied profile, use **Extensions → Install from card**, select the `cursor-magnifier` installer, review the required **HOST screen capture** permission and tap **Install and open**. If the card lacks the installer, open **Extensions → Receive from paired client** on the LCD and send the package from Windows Extension Studio using **Install to paired device…**. Review and approve it on the LCD. [Paired package transfer](../../clients/windows/README.md#offline-extension-studio)
- In Cursor Magnifier, choose **Overview**, **1:1 native** or **2× Zoom** and tap **Start live view**. With the paired HOST connected, Windows opens its monitor picker. Select a whole monitor, move the pointer onto it and confirm its name with **Allow display** on the LCD. Canceling the picker ends the request. A custom capture helper requires **Allow once** before its own selection step.
- The first request uses JPEG. To change the format during selection, tap **Settings** on the KVMpad. Choose JPEG, H.264 or Cinepak and tap **Start sharing**. The magnifier remembers the format until the board restarts. If the HOST is offline when you open the magnifier, the board shows these settings and waits for **Start sharing**.
- Coordinates are physical pixels on the selected monitor. Overview samples twice the output extent; Pixel 1× uses the output extent; Zoom 2× samples half. The native helper clamps at edges and pads smaller monitors with black to preserve scale. Rotation or a topology change invalidates the mapping. Moving the cursor onto a different display ends capture. Select that monitor in a new session. Windows controls cursor composition and content protection. Secure-desktop changes, display loss and lock terminate capture. Protected windows may appear black inside an otherwise permitted monitor.
- Freeze closes the graphics-capture session and frame pool while retaining the approved item. Resume uses the same item. Stop, Home, card removal, disconnect, OS picker closure and profile exit invalidate the session. The receiver discards its old image. The HOST requests one frame at a time. The initial ceiling is five frames per second. With negotiated presentation feedback the HOST can adjust between one and ten frames per second. PC capture/encoding buffers live in memory.
- The top bar shows the approved display and **RX to LCD**, the time from arrival of a complete image at the board's media queue to its LCD refresh. After one second of fresh pictures, the bar also shows completed refreshes per second. The panel marks the last picture stale after 1.5 seconds and clears it after six seconds without a fresh picture. **Freeze** holds the picture until you resume or stop. After a stale interval or **Resume**, the first fresh picture shows **RX to LCD** and starts a new rate measurement. **Monitor** clears the old picture and returns to capture settings. Press **Start sharing** again to choose and approve another monitor.
- Open Flight Instruments with **MSFS** or **X-Plane**. The six channels are IAS in kt, indicated altitude in ft, magnetic heading in degrees, vertical speed in ft/min, pitch in degrees (nose up positive) and roll in degrees (right wing down positive). The board requests 20 Hz for attitude and 10 Hz for the other instruments. The configured producer, simulator and link determine the delivered rate. A sample becomes stale at one second and unavailable after three. The flight yoke uses its separately granted USB joystick.
- **X-Plane 12.1.4+** uses the v2 local API on `127.0.0.1:8086`. The HOST resolves exact read-only DataRef names and reads values on the same TCP connection. The built-in simulator controls resolve `sim/flight_controls/flaps_up` and `flaps_down` and send a zero-duration activation on the same connection. The HOST reports the resulting aircraft configuration as unverified. [X-Plane local Web API](https://developer.x-plane.com/article/x-plane-web-api/)
- **MSFS** uses an out-of-process x64 SimConnect helper. Pass `-SimConnectSdk <SimConnect SDK directory>` to `build-capture.ps1` with the vendor `include/SimConnect.h` and `lib/SimConnect.lib` installed. Place the matching vendor runtime DLL where Windows can load it. The CLIENT looks for `kvmpad-msfs.exe` beside the capture picker. To use another copy, set `KVMPAD_HOST_MSFS` to the executable's full path before starting the CLIENT. The helper requests six Float64 SimVars on the user aircraft, asks the SDK for unit conversions, and maps the two flap actions to `FLAPS_INCR` and `FLAPS_DECR`. An accepted event remains unverified until observed in the simulator. [SimConnect requests](https://docs.flightsimulator.com/html/Programming_Tools/SimConnect/API_Reference/Events_And_Data/SimConnect_RequestDataOnSimObject.htm)
- **OBS Studio** uses authenticated obs-websocket v5 on `127.0.0.1:4455`. Enable the WebSocket server and authentication in OBS. In the **KVMpad HOST** window, enter the password in **Local OBS WebSocket password** and choose **Enable OBS**. For scene selection or input muting, set `KVMPAD_HOST_OBS_SCENE` or `KVMPAD_HOST_OBS_INPUT` to the corresponding OBS name before starting the CLIENT. Physical buttons in **HOST** request recording status, start/stop, scene selection and input mute/unmute. Each command needs foreground approval. The HOST reads application state after a successful request. Disconnects and uncertain outcomes never trigger an automatic retry. [obs-websocket protocol](https://github.com/obsproject/obs-websocket/blob/master/docs/generated/protocol.md)

## Studio HID test harness

The four-interface test fixture includes a Studio HID collection for exercising the USB OBS agent in the current Windows user session. The standard 7B exposes a boot keyboard and mouse. To control OBS from that firmware, use the paired HOST connection and approve OBS access in the HOST window.

```powershell
& "$bin/kvmpad-studio-io.exe" --list
& .tools/venv/Scripts/python -B "$src/studio.py" --io "$bin/kvmpad-studio-io.exe" --serial AABBCCDDEEFF --obs-input Microphone
```

Select the printed serial of the intended board. The PC checks VID/PID 303A:400C, revision 0101, interface 3, vendor usage FF61:0001, report lengths, feature 0x24 and exact serial before opening a read/write handle. Enter the OBS password and click **Allow once** for the named buttons on that USB session. Start and Stop recording are available when OBS confirms its recording status. Supplying a local input name also enables mute. Only a physical Studio tile request invokes an operation. Agent loss never automatically changes recording state.

The endpoint reassembles up to eight 51-byte fragments, validates the zero padding and CRC16, negotiates fresh nonces, sends the supported bank and answers board heartbeats. Transport ACK, accepted request, confirmed OBS state and final result are distinct. It retains one result per session and rejects stale request IDs. A retry cannot execute recording twice. Stop, disconnect, changed device nonce, loss of heartbeats or the session deadline disarm the endpoint. Reopen it and authorize the serial again to continue. [HPE report layout](../../components/usb_hid_device/src/hpe_reports.c)

## kvmbridge v1 wire

Use TLS 1.2 or later with mutual certificate authentication on all three WebSocket paths. The HOST pins the SHA-256 of the exact board certificate supplied by the paired CLIENT. The board pins the temporary HOST leaf attached to that CLIENT session. Device-to-HOST control messages and telemetry use UTF-8 JSON text. Each image is one binary WS message with a 32-byte KVS header and 1–98,304 payload bytes. Compression extensions are disabled. Never persist the session token or place it in the URL.

| Operation | Request | Response |
|---|---|---|
| Begin on `/control` | `{"type":"HELLO","protocol":"kvmbridge","version":1,"profile":"org.kvmpad.utility.cursor-magnifier"}` | `{"type":"SESSION","version":1,"session":"<32 lowercase hex>"}` |
| Begin an authored profile session | `{"type":"HELLO","protocol":"kvmbridge","version":1,"profile":"net.example.cockpit","scope":"host.telemetry.read"}` | `SESSION` echoes the requested scope. Use `host.screen.region` for a capture session. The HOST displays the requesting profile ID during approval. |
| Open both scopes for a composite profile | Include `"composite_v":1` in each scoped `HELLO` on separate `/control` connections with the same profile ID. | Each `SESSION` echoes `composite_v` and its scope and returns its own 128-bit token. |
| Request capture | `{"type":"START","seq":1,"mode":"PIXEL_1X","formats":["h264-intra","jpeg"]}` | `{"type":"STARTED","codec":"h264-intra","stream":123,"mode":"PIXEL_1X","resource":"..."}` after PC approval and picker |
| Bind data channel | Open `/image` or `/telemetry` with the same board identity, then send `{"type":"BIND","session":"<session>"}` | Receive complete binary images or JSON samples. Only the negotiated path can bind once. The panel confirms the picked display before binding. |
| Freeze / resume | `{"type":"FREEZE","seq":2}` or `{"type":"RESUME","seq":3}` | `FROZEN` or `RESUMED` with the request sequence |
| Subscribe | In a telemetry session send `{"type":"SUBSCRIBE","seq":1,"source":"sim.xplane","topics":[{"name":"sim.airspeed_kt","rate_hz":10}]}` | `{"type":"SUBSCRIBED","source":"sim.xplane"}` then bind `/telemetry` |
| Application command | On the OBS or simulator control profile send `{"type":"ACTION","seq":1,"source":"obs","action":"obs.record.status"}` | `ACCEPTED` then `RESULT` with matching sequence, status and any observed state |
| Stop one scope | `{"type":"STOP","seq":2}` or close its control or data connection | The scope's control and data connections close. A new grant begins at `HELLO` and requires a fresh decision. |

The firmware requests each declared telemetry topic once, including topics used by multiple widgets. A composite profile can hold one telemetry grant and one display grant at the same time. The HOST reserves a scope at `HELLO`, presents each request separately and lists both active grants in the foreground window. The telemetry decision names the source and topics. The capture decision names the mode and codec, then opens the system picker. Declining one request or losing one scope's connection leaves the other grant active. **Stop session**, **Revoke and exit**, and loss of the interactive desktop revoke both.

Simultaneous scopes require `composite_v:1` on both sessions and an exact match on the paired board and profile. Each grant keeps its own sequence, expiry, token and data path. Duplicate scopes, a different profile and concurrent action sessions are refused, including while approval is pending. A peer using the original handshake can open one session. The original instruments and magnifier retain their version 1 handshake. The built-in Studio IDs `org.kvmpad.studio.obs` and `org.kvmpad.studio.sim` are reserved for physical command buttons.

Sequences for control requests start at 1 and advance by exactly one. Stop preempts a pending request or application operation. A denied request returns `DENIED` where the link permits, then ends the session. The desktop request limit is 4,096 bytes. The firmware response limit is 1,024 bytes. A fragmented message must complete in two seconds within 64 fragments. Each active data channel is bound to the current profile, session and certificate. All grants expire within one hour.

If the picker returns a window, the HOST returns `DENIED` with `"reason":"pickedwindow"` before the board asks for display approval. The panel prompts you to select a whole monitor and try again. If the pointer never reaches the selected display, the HOST returns `DENIED` with `"reason":"cursorwait"`. If it enters and leaves before the HOST announces the monitor, the reply is `DENIED` with `"reason":"cursor"`. After the announcement, leaving the display revokes the capture grant and closes its control and bound image sockets with WebSocket code 4001.

The HOST allows 45 seconds for monitor selection. Flows with an explicit **Allow once** prompt have a separate 45-second approval deadline. If the pointer is elsewhere when selection finishes, the HOST window asks you to move it onto the selected monitor. The native picker gives you 20 seconds to do so. When the pointer enters the monitor, the HOST window directs you to **Allow display** on the KVMpad. The HOST allows two additional seconds for the pointer-entry result and applies a 117-second overall startup deadline. The board waits up to 125 seconds for the reply. An expired PC approval, a timed-out picker and a canceled selection produce distinct on-device messages.

The HOST advertises `progress_v: 1` in the capture session. A board can request the same field in `START` to display Windows picker instructions on the LCD. When it opens the picker, the HOST sends `START_PROGRESS` with the request sequence and stage `picker`. It sends stage `pointer` only when the selected monitor is waiting for the mouse. Both stages share the original startup deadline. The local approval button appears when `STARTED` identifies the selected display. Peers that omit the option keep the single final reply.

When both peers also negotiate `pointer_label_v: 1`, the pointer instruction can name the selected monitor. Its optional `resource` is the Windows display device name returned by the picker. The HOST shows that name locally even when the board uses the older progress messages. If a name was shown, the final display selection must match it before KVMpad offers **Allow display**. Earlier helpers can still send the generic instruction.

When KVMpad returns presentation feedback, the HOST displays the confirmed frame rate. If KVMpad
reports two consecutive frames that it could not display after a successful presentation, the HOST
clears the rate and latency figures and shows **Waiting for KVMpad to display a new image**.
The first newly confirmed picture restores the sharing status. The second supplies a fresh
rate measurement.

The sharing status and **Stop sharing** button appear on the selected monitor after the picker closes. After a cursor exit, the KVMpad clears the previous image. When the old session has closed, **Start sharing** becomes available again with the same codec and zoom. Move the pointer back to the intended monitor, tap **Start sharing**, and approve the new session on the PC and the KVMpad.

If the selected monitor moves or changes size, or Windows returns inconsistent physical coordinates, the LCD prompts you to select the monitor again. If Windows cannot read the physical pointer position, the LCD reports that separately. During selection the HOST sends a fixed `DENIED` reason (`topology`, `geometry` or `cursorpos`). After selection it closes both authenticated sockets with code 4003 for a mapping failure or 4004 for unavailable pointer coordinates. Both codes have an empty reason field. Start a fresh session after the Windows display configuration settles.

Windows can stop delivering capture samples while the selected display is still. When a request waits 250 ms on an already-running capture session, the helper reopens the frame pool and session once on the same approved item. It checks the pointer again and waits for a new Windows sample. If the helper has not returned a frame within the HOST's two-second read deadline, the HOST closes the control and bound image sockets with code 4002. The panel reports that Windows timed out waiting for an image from the selected monitor. Both 4001 and 4002 carry an empty reason. Windows error details stay on the PC.

Telemetry contains `type`, `seq`, `topic`, `source`, `unit`, `value`, `ts_mono_ms` and `quality`. The value is a finite number or JSON null for `UNAVAILABLE` and negotiated `ERROR`. Timestamps are the HOST sample clock modulo 2³². The HOST also sends `age_ms`, the elapsed time from measurement to transmission, capped at 3,000 ms. The board adds local elapsed time since receipt. When an older peer omits the age, repeated source timestamps retain their first observed age. Only the selected subset of the six named simulator channels is accepted for the session.

The host's framing reuses the desktop reference. The device checks session stream ID, payload size, sequence and CRC before decode. It admits the receive buffer, decode buffers and presenter separately. An admission failure ends the session. Cinepak uses one 960×540 intra strip, a full 256-entry color V1 book and raster-order vector indices. JPEG and H.264 use 512×512. H.264 sends a baseline Annex B access unit with SPS/PPS/IDR on every frame. A new codec or monitor requires a new session.

## Verification

Run the following from the workspace root after building the picker. Synthetic tests exercise TLS and fixed local provider protocols on loopback. The picker's test entry points read synthetic pixels from stdin and do not open the desktop.

```powershell
$env:KVMPAD_TEST_PICKER = (Resolve-Path "$bin/kvmpad-capture.exe").Path
$env:KVMPAD_TEST_SURFACE = (Resolve-Path "$bin/kvmpad-test-surface.exe").Path
$env:KVMPAD_TEST_STUDIO_DLL = (Resolve-Path "$bin/kvmpad-test-studio.dll").Path
& .tools/venv/Scripts/python -B "$src/test_host.py"
& .tools/venv/Scripts/python -B "$src/test_paired_host.py"
```

The paired HOST tests verify startup, per-run certificates and grant revocation when the CLIENT control pipe closes. The broker tests cover invalid certificate pins, missing client certificates, profile isolation, duplicate and oversized requests, fragmented messages, interrupted consent, Stop from either side, Freeze/Resume, six DataRef reads and a flap request, OBS challenge/feedback, sample aging and all three encoders. Composite tests run the production broker over loopback mutual TLS. They exercise overlapping foreground reviews, grant isolation, picture credits during telemetry faults and recovery, and HOST Stop with four live sockets. With the offscreen surface fixture and FFmpeg, they also validate independently decoded JPEG pixels during a six-DataRef X-Plane subscription and after the board ends telemetry. Native geometry checks cover rotated and negative-offset monitors and edge clamping. Hardware qualification uses a paired CLIENT and a physical board to check HOST attachment, Windows picker acceptance and denial, protected desktop, the target simulator and OBS versions, on-panel telemetry, capture-to-panel timing and sustained load. Building the MSFS helper requires the external SDK.

For a physical capture check, select a display showing a moving test pattern. Run a separate session for H.264 (512×512) and Cinepak (960×540). Record the LCD's fps and **RX to LCD** during motion, then check **Freeze**, **Resume** and **Stop**. With diagnostic console access, `ui host-status` returns attachment, link and sharing status, the live-panel flag, completed picture count, fps in tenths and the latest RX-to-LCD time in milliseconds. Sample the picture count twice during motion and check that sharing clears on Stop. These readings cover the board's receive-to-refresh path. Measuring optical capture-to-panel latency requires an external reference.

From the repository, run `python -B test/hil/observe_magnifier.py --port COM3 --output cinepak-monitor2.jsonl --duration 60 --seconds 180` for a one-minute measurement. The timeout includes setup. For the one-hour stream use `--duration 3600 --seconds 3750` and a new output file. Record the codec selected on the panel and the monitor approved in Windows alongside the samples. The UART reports the number of completed pictures and the latest board latency. It does not identify the Windows monitor or the codec. The observer requires the connected link state, active sharing and an advancing picture count. A lost response, counter rollback, inactive stream or a gap of more than three seconds between observed advances starts a fresh window.

### Offline cursor and frame checks (EXT-060 and EXT-061)

`-TestGeometry` also builds a standalone D3D11 WARP fixture. It creates a texture from a deterministic grayscale chart in memory. It needs no display selection, network connection or board. Run it from the workspace root with FFmpeg on PATH.

```powershell
& .tools/venv/Scripts/python -B "$src/test_capture_surface.py" --surface "$bin/kvmpad-test-surface.exe"
& .tools/venv/Scripts/python -B "$src/test_host.py" OfflineCaptureTests
```

Add `--driver hardware` to run the same fixture on a physical rendering adapter. Its offscreen D3D11 device creates textures from test-owned pixels and rejects a software adapter in hardware mode. The default driver is WARP.

The coordinate test assigns an opaque, unique BGRA value to each source pixel. A producer rotates non-square rasters by 0°, 90°, 180° or 270° into synthetic upright capture surfaces. An independent inverse mapping checks each output pixel, including black padding on odd display sizes and samples immediately around the clamp threshold. The production GPU copies only the selected rectangle to a staging texture. The CPU reads its actual row pitch and samples nearest neighbors. The picker refuses a frame whose full texture extent differs from the selected item's current physical dimensions. A display-change message ends the sharing session.

The encoder fixture exercises the production crop, GPU-to-CPU copy, nearest-neighbor scaling, encoders and KVS framing. Its inputs represent a landscape monitor at 150% with a negative origin, a portrait monitor at 125% and a 399×301 monitor at 100%. The cursor visits the interior and opposite corners of each monitor, then crosses the selected monitor boundary. For every combination of codec and zoom, the test checks every BGRA pixel before encoding, verifies the KVS CRC, decodes the payload independently with FFmpeg and checks luminance samples against source coordinates. Timings cover synthetic frame production on the PC. The HOST measures the receiver separately when it negotiates presentation feedback.

Failure cases cover denied foreground consent, an unidentified monitor, content-size mismatch, crossing a display boundary and revocation during encoding. Separate broker tests hold an image send pending and force a high-entropy H.264 frame over the 96 KiB limit. Production requires a per-monitor-v2 DPI context and shows **Edge clamped** in the HOST sharing window when a crop reaches an edge or needs padding. Physical picker behavior, OS-reported cursor coordinates on a mixed-DPI desktop and capture-to-panel latency still require interactive qualification.

The board receives the selected display label and mode in the control exchange, then the stream ID, sequence, output dimensions and a wrapping 32-bit HOST monotonic timestamp in each KVS frame. The timestamp records local frame demand before IPC. Its differences give HOST request intervals. Peers that negotiate cursor metadata also receive the physical cursor position, actual crop rectangle, source padding and clamped flag for each accepted image. A capture-to-LCD latency measurement needs an independent shared time reference or optical timing. Synthetic rotation checks begin at the upright texture boundary. Actual Windows picker selection, OS cursor coordinates and WGC orientation delivery on a rotated monitor still need an interactive check.

### Optional cursor metadata

Sections 16 and 17 of v1.3.1 place nominal crop dimensions and mode on the control channel and reserve the KVS header flags. The Windows HOST advertises `"crop_v":1` in `SESSION` for the magnifier profile. A participating board sends `"crop_v":1,"source_crop":[width,height]` alongside one chosen codec in `START`. The HOST echoes those two fields in `STARTED`. Dimensions are physical source pixels before clamping and scaling. With an older HOST or a board requesting the original contract, each image uses the existing binary KVS message on its own.

After physical approval and image-channel binding, the negotiated HOST sends one JSON text message of the form `{"type":"CROP1","data":"…"}` immediately before its binary KVS frame on the same image WebSocket. The 96 lowercase hexadecimal digits represent the following 48 bytes in little-endian order.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | ASCII `KCM1` |
| 4, 8 | 4 each | Signed physical cursor x, y in the virtual desktop |
| 12, 16 | 4 each | Signed physical left, top of the actual source rectangle |
| 20, 22 | 2 each | Actual width and height before scaling |
| 24, 26 | 2 each | Left and top black padding in source pixels. The right or bottom receives the extra pixel for an odd difference. |
| 28 | 1 | Clamped or padded, 0 or 1 |
| 29 | 3 | Reserved, zero |
| 32, 36, 40 | 4 each | Stream ID, frame sequence and the KVS frame CRC from KVS offset 28 |
| 44 | 4 | IEEE CRC32 over bytes 0–43 |

The board allows one pending record, at most 160 UTF-8 bytes, and requires the matching complete image within two seconds. It checks geometry, both CRCs, stream identity and advancing frame sequence before handing the pair to the presenter. TLS authenticates both messages. Freeze acknowledges the last sent sequence, or −1 before the first image. The board consumes queued pairs through that boundary before resuming presentation. Stop, grant expiry and a broken pair end the session. Frame copies carry no metadata outside this negotiated session. The picker samples coordinates for the same frame and rechecks consent after encoding. Its local `F` command still returns only image data, while `M` prepends the 32-byte geometry prefix.

The panel shows **Edge clamped** for a completed picture limited by a display edge. Beside the 512×512 image it shows the active mode, cursor position, source rectangle and source padding. A paused frame retains its matching coordinates. Stopping the session clears them. Older peers provide the image without geometry.

The deterministic wire test builds `test/live_view/crop_wire_check.c` with `components/kvmbridge/crop_wire.c`, `components/live_view/live_message.c` and `components/remote_protocol/src/remote_json.c`. Run `test_crop_metadata.py --crop-check <receiver>` for negotiation, corruption and replay; pass the same option to `test_capture_surface.py` for comparisons against all three native encoders.

### Optional presentation feedback

A capture `SESSION` advertises `"present_v":1`. To opt in, the board sends `"present_v":1,"max_fps":10` with `crop_v:1`, one codec and its mode-specific source crop in `START`. The accepted integer range for `max_fps` is 1 through 10. `STARTED` echoes `present_v` and `max_fps` and adds `start_fps`, the smaller of five and the requested ceiling. Each extension is explicitly selected. With original KVS or crop-only peers the HOST keeps a five-FPS ceiling and accepts no reverse image messages after `BIND`. A board connecting to an older HOST requests only advertised features.

After a complete CROP1 and KVS pair, the board sends `{"type":"FRAME_DONE","stream":123,"frame":0,"shown":true}` as one JSON text message on the same image WebSocket. Send `shown:true` after the first completed display refresh for that frame, or `false` when the receiver definitively discards it. The stream and frame IDs must exactly match the outstanding image. This feedback uses the image sequence, independently of the control request rate limit. The HOST keeps one image outstanding. It requests fresh pixels after feedback and ends the grant after three seconds without feedback. A malformed, duplicate or unexpected completion ends both channels.

Each grant starts at its agreed initial rate. Six consecutive capture-to-feedback observations within 80% of the next faster interval raise the target by one FPS, up to the ceiling. A discard or a missed target interval lowers the target immediately, down to one FPS. The HOST ignores a locally encoded frame older than one second and ends the grant after six consecutive unusable captures. H.264 raises CRF after an oversized frame and steps quality back up after twelve consecutive payloads under half budget. All negotiated dimensions and KVS headers remain fixed for the session.

For crop-aware sessions the one-second freshness budget starts when the HOST requests a frame and includes the time spent sending its CROP1 metadata. If the metadata write finishes after that deadline, the HOST closes the grant before sending the expired image. The receiver discards the incomplete pair.

For live H.264, the HOST keeps one FFmpeg encoder open between frame requests at the same quality. Freeze releases the encoder. Resume starts it on demand, and each picture carries its own decoder headers and keyframe. FFmpeg builds that exit at startup on the packet statistics option use one process per frame.

Freeze retires the outstanding frame at the acknowledged sequence fence. One matching late completion is allowed and cannot release a newer frame. Resume resets the initial rate and continues the image sequence. Stop, revoke and disconnection invalidate the outstanding credit and both sockets. The HOST reads a single monotonic clock for its rate decision. The interval includes capture, encode, transport and the receiver's reported presentation work.

When the board confirms presentations, the HOST shows its target rate and the rate confirmed by the board across the latest 32 displayed pictures. After 20 confirmations it also shows the 95th percentile from the HOST frame request to receipt of the LCD acknowledgement. Both timestamps come from the HOST clock and include the return trip. Freeze and Resume clear the sample window. Optical latency requires a separate measurement.

Run `test_presentation_pacing.py --surface <kvmpad-test-surface.exe> --ffmpeg <ffmpeg>` with the synthetic native test build. Its WARP texture uses a cursor indexed by a 10 ms Windows clock, permitting an independent freshness check when the loopback receiver withholds a credit. The receiver simulates refresh completion. Physical LCD cadence, end-to-end optical latency and HID responsiveness under load require a board measurement.

### Loopback pixel and lifecycle check

`test_loopback_pixels.py` runs the production HOST listener, broker, image channel and encoders in a separate Windows process. The port binds only to `127.0.0.1`. A private input pipe stands in for the foreground decision, and the separately built synthetic surface stands in for the picker. The client supplies temporary certificates, checks the HOST leaf certificate, decodes both frames with FFmpeg and compares the decoded raster hashes across original and metadata sessions. The metadata path also runs the production C parser.

```powershell
& .tools/venv/Scripts/python -B "$src/test_loopback_pixels.py" --surface "$bin/kvmpad-test-surface.exe" --crop-check "$bin/crop_wire_check.exe" --ffmpeg ffmpeg
```

The test covers Freeze and Resume in each session, while exercising board Stop, the HOST Stop callback, image-channel disconnect and refusal of an expired session token. A new connection waits for a new consent decision.

### Flight Instruments source recovery

A foreground telemetry grant is bound to one named simulator provider and its approved topics. X-Plane keeps a separate loopback HTTP connection for each subscribed gauge (up to six). It resolves each DataRef name on that connection and discards its numeric ID when the connection fails. A slow altitude or airspeed read therefore leaves the two independent attitude reads running. MSFS reopens the read helper after EOF, timeout or a failed simulator connection while the same foreground subscription remains active. The helper requests FLOAT64 values in the canonical units and normalizes the attitude signs. An unanswered read or a stopped simulator lets the previous sample age to `STALE` at one second and `UNAVAILABLE` at three. A missing X-Plane DataRef is immediately unavailable. A nonnumeric, non-finite or unrepresentable measurement invalidates the affected channel immediately. An explicit SimConnect exception or malformed packet invalidates the whole submitted request. A successful read restores the channel.

Section 15.3 defines a fourth quality, `ERROR`. Telemetry sessions advertise `"quality_v":1` in `SESSION`. The receiver opts in by including the field in `SUBSCRIBE` and checks the echo in `SUBSCRIBED`. Negotiated faults carry `"quality":"ERROR","value":null` with the time of the fault. They expire after three seconds without another observation. Existing peers receive `UNAVAILABLE` with a null value for a fault. An instrument with a fault hides its indicator and displays **Data error**. The system data status reports **Data error** when any granted source reports a fault, including when another source is stale. A clocked-out or disconnected source displays **Data unavailable** after expiry.

For a partial MSFS subscription, the adapter first sends `S` and a one-byte topic mask. Bits 0 through 5 correspond to IAS, altitude, magnetic heading, vertical speed, pitch and roll. The helper echoes both bytes, registers only those SimVars and returns the selected doubles in bit order for each `Q`. A full subscription retains the original six-double `Q` exchange. A partial grant requires the updated helper. Each restarted read-only helper registers its scope again. Stopping or losing the paired session ends its readers.

Build `build-capture.ps1 -OutputDirectory $bin -TestTelemetry` and run the following from the workspace root:

```powershell
& .tools/venv/Scripts/python -B "$src/test_loopback_telemetry.py" --helper "$bin/kvmpad-test-simconnect.exe"
```

The test runs the production HOST listener and both simulator adapters over local mutual TLS. The X-Plane HTTP fixture intentionally delays IAS by 180 ms and changes all six IDs on restart. The MSFS test executable compiles the production native helper against a test implementation of the SimConnect calls. This checks requested SimVars, units, selected fields, attitude sign conversion and IPC. Official SDK ABI compatibility and behavior in a running simulator still require a simulator-equipped Windows machine.
