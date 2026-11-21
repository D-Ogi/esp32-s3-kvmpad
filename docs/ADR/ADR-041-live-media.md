# ADR-041 — Live magnifier formats and H.264 playback on the 7B
Status: PROPOSED
Date: 2026-11-19
Approver: project owner

## Requirements

The on-device magnifier offers labeled magnification and video format selectors in the active
control palette. Before sharing, a local 1920×1080 example shows the requested crop around the
pointer and the output dimensions on the KVMpad. It uses the geometry sent to the HOST during
negotiation. Edge clamping and padding depend on the monitor the operator selects.
After the PC operator selects a source, the panel wraps its full name above
**Allow display**. **Stop** and **Home** remain available during approval. The 44-pixel header
controls fit above the 960×540 Cinepak picture. [Day setup](../../../evidence/build/magnifier-framing/day-setup.png) · [Night format selector](../../../evidence/build/magnifier-framing/night-codec-list.png) · [High-contrast approval](../../../evidence/build/magnifier-framing/high-contrast-review-m.png)

The owner requires H.264 for live images as well as file playback and accepts Cinepak at 960×540 for the magnifier. Extension Platform v1.3.1 §§16–17 specifies JPEG for its first image channel. The H.264 handoff archive has SHA-256 `670069f52aacb43b683d349adbae88ca51be26924b391b7c3be786eec1438730`. All 8,332 listed files matched its manifest. The archive in `../spec-source/` contains FFmpeg 6.1.1 sources, reference patches, weights, training records, compressed samples and per-plane/frame hashes. Upstream source and media licenses apply individually.

| Profile | Input and output | Acceptance |
|---|---|---|
| JPEG magnifier | Host crops 1024² / 512² / 256² source pixels for Overview / Pixel 1× / Zoom 2×. Encode a 512² JPEG for all three. In Pixel 1× show it at content (24,28), physical (24,60). | MAG-01..13, including physical pixel geometry, mixed DPI, protected desktop, clear-on-stop and HID latency. JPEG introduces color loss. |
| H.264 magnifier | First reference profile uses independent baseline Annex B access units at 512², each with SPS, PPS and an IDR. One encoded access unit per message. The negotiated profile can skip any whole access unit. | Reconstruct each delivered unit after a gap and reconnect, check coded geometry and resource limits before allocation, compare pixels with the host and qualify the device decoder. |
| Cinepak magnifier | An independent 960×540 color frame with one intra strip, full codebooks and V1 or mixed V1/V4 vectors. Center it at content (32,14), physical (32,46). At 1× the host crops 960×540 source pixels. Put Home, capture indicator and Stop in the system bar. | Compare 540 RGB565 rows with an independent decode, test cancellation, revocation, queue overload and LCD timing with HID and Wi-Fi active. |
| Local H.264 playback | Open standard MP4 or Annex B from microSD. One decoder instance. Compare tightly packed Y/U/V planes in presentation order before conversion to RGB565. MP4/CFR/PTS enables the optional temporal modes. | Exercise all eight handoff inputs. First qualify exact, reduced reconstruction and the large filter bank. Measure file I/O, parsing, CABAC/CAVLC, motion/residual reconstruction, reference memory, color conversion and actual presentation intervals on the board. |

## Live transport and admission

The versioned reference is in `tools/live_view/framing.py`. The ESP32-S3 receiver in `components/live_view/live_message.c` validates a complete binary message before dispatch. A RAM test on unit 1 received a Cinepak message, decoded its 540 rows, then rejected a replay and a message after Stop. The Windows HOST service must obtain OS capture permission for the selected resource, and the panel must approve the profile's current capture session. Only then may both peers select a mutually supported format over the authenticated control channel. Keep one stream per device. On Stop, denied capture, protected desktop, disconnect, revoke, card loss or profile exit, invalidate the stream, discard buffered images and return control to the firmware UI. Never log pixels, store capture frames on microSD, or resume capture solely because a connection returned.

* Preserve `KVS1`, version 1, type 1 for JPEG. For the proposed additional formats use `KVS2`, version 2, type 2 for H.264 intra and type 3 for Cinepak intra. All other combinations are rejected. A codec change requires a new negotiated stream ID.
* One binary WSS message holds the 32-byte header specified in package §17.3 followed by exactly 1..98,304 bytes. The desktop reference computes IEEE CRC32 over bytes 0..27 followed by the payload, excluding the CRC slot. The additional formats require the independent-frame bit. The receiver checks the negotiated dimensions, ID, length and a wrap-safe sequence before dispatch. Codec syntax and actual dimensions must also be validated by the decoder. TLS, consent, rate limits and session revocation belong to the live service.
* Keep at most one frame waiting and one being decoded, with separate admission against available memory. An oversized frame triggers a drop and encoder adjustment. HID releases preempt media. Dependent pictures would need a distinct negotiated profile and keyframe recovery after loss. Targets from §16 are 5 FPS initially, 10 FPS stretch and capture-to-panel p95 below 250 ms. Qualify the selected profile under concurrent load. For local 24 FPS playback the frame deadline is 41.667 ms.

A HOST that advertises `present_v:1` and `crop_v:1` can pace capture using LCD feedback. The device requests a ceiling of 10 FPS. The HOST starts at 5 FPS or the requested ceiling, whichever is lower. Only one crop record and its picture may be outstanding. After LVGL finishes the final flush and completes the picture refresh, the device returns `FRAME_DONE` with the stream ID, frame sequence and `shown:true`. A discarded picture returns `shown:false`. The HOST samples the current desktop again when it has credit. Freeze retires the outstanding frame through the sequence in `FROZEN.last_frame`. Resume starts at the initial rate. Three seconds without feedback ends an active stream. Receivers that omit the extension retain their existing wire protocol. [Protocol and measurements](../../test/reports/2026-11-19_magnifier-pacing_022168e4.yaml)

## Memory and implementation

### Firmware build with four USB interfaces

Use ESP-IDF 6.1 and the Xtensa FFmpeg archives from the supplied H.264 source package. The
media configuration selects size optimization and keeps the signed player in a 4 MiB OTA slot.
The existing data partition can supply the optional Korean font alongside the Chinese and Japanese
dictionaries and fonts. [Language image selection](ADR-042-optional-languages.md)
From the repository root, use a fresh build directory and a separate SDKCONFIG.
For example, with the decoder libraries in `../ffmpeg-build/xtensa`, run these commands in PowerShell.

```powershell
$decoder = (Resolve-Path ../ffmpeg-build/xtensa).Path
idf.py -B ../build-7b-media -DIDF_TARGET=esp32s3 "-DSDKCONFIG=$PWD/../build-7b-media/sdkconfig" "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.hpe;sdkconfig.media" "-DKVM_MEDIA_FFMPEG=$decoder" build
python test/host_unit/check_hpe_descriptors.py ../build-7b-media/kvmpad.elf --interfaces 4
```

The descriptor check reads the linked firmware. The standard media profile uses physical
touch for local permission review. [USB configuration](../../components/usb_hid_device/README.md)
· [Decoder build and relinking](../../THIRD_PARTY_NOTICES.md)

### Prepared local playback

Selecting a movie under **Video files** shows its source dimensions. For MP4, the heading also shows the average frame rate when the track declares one. The rate after playback measures pictures presented by the LCD. The card worker reads the selected source in the background, so **Play** and **Back** are available immediately. If the header cannot be read within the preview memory budget, the playback choices stay available.

For a local movie that exceeds the decoder's RAM budget, the desktop can decode the source once using the same H.264 backend, reconstruction mode and RGB565 conversion used by the firmware. The card holds the original and a lossless display companion next to it. Selecting the original under **Extensions → Video files** exposes **Play** for a prepared exact reconstruction. The device then plays offline. The companion header binds it to the full source SHA-256, length and mode. Each frame has its original presentation time and a CRC over the displayed pixels. The reader reconstructs bands into a staging surface and waits for LCD refresh before advancing to the next frame.

In the Windows client, choose **Prepare card video…**, select the original and choose the destination folder or card. The exact mode uses Python 3.11+ and FFmpeg/ffprobe on PATH. From the repository root, the equivalent command is:

```sh
python tools/live_view/prepare_card_video.py /path/to/test_1920x1080.h264
```

For all three spatial reconstruction modes, or for inputs whose presentation timing requires the reference demuxer, use the native build made by `tools/live_view/build_ffmpeg.py`:

```sh
python tools/live_view/prepare_card_video.py /path/to/test_1920x1080.h264 --ffmpeg-build /path/to/native-ffmpeg --cc gcc
```

Place `test_1920x1080.h264` and `test_1920x1080.h264.kvc` together at the card root. The same command accepts MP4. For reduced or large-bank reconstruction use `--mode reduced` or `--mode bank` and the corresponding adjacent `.kvr` or `.kvb` companion. The decoder can also open the original directly when its working set fits.

For constant-rate MP4 with presentation timestamps, the native reader also accepts `--mode rr_blend` and `--mode rr_smart_loose`. These write `.kvf` and `.kvs` companions. Choose **Temporal blend** or **Smart loose** in the updated file menu. The temporal modes require a compatible MP4 timeline.

The card test set contains the same short scene at 960×540, 640×360 and 320×180, each at 25, 15, 10 and 5 fps. The unsuffixed MP4s use a 25 fps source. In **Extensions → Video files**, select the MP4 and choose **Play**, **Reduced** or **Large bank**. Use **Next** for later files. The three prepared modes present half the source width and height. At the end, the panel shows the filename, selected mode, displayed dimensions, frame count and measured display rate. It divides the number of intervals between completed picture refreshes by the time from the first to the last refresh.

For the 320×180 Exact test clips, native playback on unit 1 followed the 5, 10 and 15 fps sources. On the 25 fps clip, prepared playback displayed 23.58 fps and native playback displayed 16.15 fps. For 640×360 Exact, both paths completed all four clips on unit 1. The device prefers a valid prepared companion and falls back to the H.264 file if the companion is missing or fails validation at open. The result screen identifies the path used as **Prepared companion** or **Native H.264**. After a playback error it also shows how many pictures completed a display refresh.

The table gives measured LCD rates for the same four 640×360 Exact files. The native measurements come from boot 390 and the prepared measurements from boot 343. The default hardware regression repeats the four native cases and the prepared 1080p case. Its `--rate-matrix` option measures 39 prepared combinations. [Unit 1 measurements](../../test/reports/2026-11-19_device-media_022168e4.yaml) · [Hardware regression](../../test/hil/card_playback_regression.py)

| Source fps | Pictures displayed on each path | Native H.264 LCD fps | Prepared companion LCD fps |
|---:|---:|---:|---:|
| 5 | 15 | 3.473 | 5.151 |
| 10 | 29 | 3.918 | 10.306 |
| 15 | 44 | 4.333 | 11.545 |
| 25 | 73 | 4.641 | 11.789 |

Prepared playback retains the last complete picture on screen while it reconstructs the next one in a second RGB565 surface. Once LVGL selects a picture, the worker may read the following picture into the released surface while the selected one is being drawn. It waits for the selected picture to finish flushing before publishing the next one, including at end of file. The original 1080p clip uses two 960×540 surfaces totaling 2,073,600 bytes. The card worker lends the reader a 16 KiB input window. Each compressed band uses the existing zlib library with bounded storage on the worker stack. For delta bands the reader computes the frame CRC as it writes the reconstructed pixels. All 73 original pictures matched in each spatial mode. Under identical Xtensa QEMU settings, the new reader used about 38% fewer counter ticks and 752 fewer bytes of stack. Physical display rate also depends on SD reads and LCD refresh. [Card worker](../../main/pack_panel.c) · [Band inflater](../../components/pack_runtime/pack_port.c) · [Verification report](../../test/reports/2026-11-19_device-media_022168e4.yaml)

On an engineering build, `ui media-card-run kvp_640x360_5fps.mp4 reduced timed` selects the same file and button as a physical tap. The `timed` policy measures normal playback with the reader's frame CRC enabled. The `auto` policy also compares each prepared picture's CRC with the movie rectangle in the panel framebuffers. The `native` policy exercises direct H.264 decoding with the companion present on the card. After `CARD_HIL_DONE`, issue `ui media-card-home` and wait for `CARD_HIL_HOME ready=1` before starting the next clip.

In `auto`, `CARD_BLACKOUT` also samples 48 points inside the picture at LVGL flush and refresh events. It counts a black interval between lit pictures as confirmed when both framebuffers contain black at the sample points. It reports intervals identified from the calibrated front-buffer swap order separately as inferred. The instrument measures the time between observed changes in framebuffer memory. Eye or camera observation also covers panel and backlight behavior. [LCD instrumentation](../../main/media_panel.c) · [Black repaint regression](../../test/live_view/panel_test.c)

Unit 1 displayed all 73 pictures of the original 1920×1080 test movie at 960×540 with its 12,802,144-byte exact companion. Each picture matched the expected RGB565 CRC in the LCD scan-out buffer during the instrumented test. On the earlier two-surface release, the owner opened the original with **Play** and saw the moving image. The first and last refreshes were 34.478 seconds apart (2.09 FPS), compared with 2.88 seconds in the source. [Hardware results](../../test/reports/2026-11-19_device-media_022168e4.yaml)

The historical 39-case timing run used one work surface for 960×540 output. Its 1080p rates were 1.761 fps for Exact, 1.822 for Reduced and 1.786 for Large bank. The owner observed black flashes between the high-resolution pictures. Lower-resolution playback with two surfaces stayed steady. The CRCs from that build cover completed pictures only. [Historical timing and owner observation](../../test/reports/2026-11-19_device-media_022168e4.yaml)

On the updated two-surface build, unit 1 played the original five times with framebuffer instrumentation, including three Reduced runs, one Exact run and one Large bank run. The 365 completed pictures matched their CRCs. The sampler found zero black intervals in 360 consecutive lit pairs. Each run also recorded 48 samples for which front-buffer ownership was ambiguous. [Per-run observations](../../test/reports/2026-11-19_device-media_022168e4.yaml)

With the extra framebuffer scans disabled, unit 1 completed all 1,668 picture refreshes across 39 two-surface runs. The 25 fps original measured **2.007 fps in Exact, 2.059 in Reduced and 2.019 in Large bank** at a displayed 960×540. All runs returned Home with boot count 343. [Full timing matrix](../../test/reports/2026-11-19_device-media_022168e4.yaml)

On boot 400 the same 1080p clip completed all 73 LCD refreshes in each mode, at **2.433 fps in Exact, 2.511 in Reduced and 2.466 in Large bank**. In Exact, the card worker spent 29.26 s in the frame reader. That included 10.24 s refilling its input window from the card. Of the 11.28 s measured between picture selection and refresh completion, 10.99 s overlapped the next frame read. These measurements use the original source and its 960×540 display companion on unit 1. [Physical stage timings](../../../evidence/build/card-stage-hardware/01-test_1920x1080.h264-exact.log)

The signed two-surface release booted from validated OTA0 on unit 1 and mounted native USB. Its OTA1 fallback passed hash verification. Opening `test_1920x1080.h264` from the normal menu and selecting **Play** displayed all 73 pictures in Exact mode at 960×540. The completion screen reported 36.005 seconds and **2.00 fps**. The device returned Home at the same boot count of 344. Telemetry and two screen captures document this run. Visual confirmation of steady playback on the updated release is still pending. [Release installation, menu playback and captures](../../test/reports/2026-11-19_device-media_022168e4.yaml)

Before the decoder memory fix, direct playback on boot 343 completed all four 320×180 and 640×360 clips in Reduced and Large bank, and all four 320×180 clips in Exact. The 640×360 Exact runs stopped after 12 pictures on that build. After the fix, unit 1 completed all 161 pictures in those four Exact runs. The older 960×540 5 fps MP4 and original 1080p direct Reduced and Large bank trials exhausted memory before the first picture. [Direct-decoding results](../../test/reports/2026-11-19_device-media_022168e4.yaml)

The 73-frame `test_1920x1080.h264` occupies 1,130,868 bytes on the card. Its measured decoder allocation peak on the host was 19,326,673 bytes in exact mode and 13,043,410 bytes in reduced mode, before LCD buffers. The decoder retains motion data and reconstructed pictures as references. A separate experimental firmware build can page the reduced and large-bank decoder's working memory to a temporary file on microSD. Both modes reproduced the 73 reference pictures in host tests. With a 512 KiB page cache the large-bank run transferred 1,388,576,768 bytes to and from the scratch file. The one-bit SDMMC bus at 20 MHz has a theoretical ceiling of 2,500,000 bytes/s. That traffic alone would take at least 555 seconds. The installed release uses the prepared companion for this sample.

The installed firmware held both language engines open. At steady state the UART reported 103,144 to 207,724 bytes of free PSRAM across the recorded snapshots. Internal free memory was about 41 KiB. A full additional RGB565 surface would require 524,288 bytes at 512² or 1,036,800 bytes at 960×540. Free totals do not establish the largest contiguous block. At activation measure available internal/DMA/PSRAM blocks, then reserve the input, decoder and presentation working sets or decline that profile.

The Cinepak presenter retains two indexed pictures totaling 275,492 bytes and converts four rows at a time to RGB565 through the LVGL display owner. Two 7,680-byte stripes support drawing within the 960×540 picture. The standalone row decoder also accepts a fixed 11,776-byte codebook and scan-line workspace. Opening the media tools releases inactive IME dictionaries. The receiver checks memory at admission. In the latest test on unit 1, two synthetic pictures matched their expected CRCs at physical (32,46). The LCD completed 30 subsequent refreshes with 5.802 s between the first and last completion. The host capture and Wi-Fi paths require a separate test. [Language engine lifetime](../../main/ime_glue.c)

| Work item | Owner boundary | Verification remaining |
|---|---|---|
| MEDIA-01 · reference data | Desktop test tools | The three spatial modes reproduced all 7,101 component hashes across eight inputs. On 32- and 64-bit hosts the two temporal modes reproduced 998 CFR MP4 pictures and their presentation times across four inputs. All 2,994 temporal component hashes matched on each ABI. |
| MEDIA-02 · filter arithmetic | Portable core, ESP32-S3 | 8,192 reference vectors passed on PC and board. The bank provides interpolation after syntax and motion decoding. |
| MEDIA-03 · device file decoder | SD reader, decoder, presentation task | The two-surface prepared path completed the original at 2.007–2.059 fps in three spatial modes. Direct 1080p decoding with the existing Xtensa backend exceeds the measured PSRAM budget. Source-rate playback remains open. |
| MEDIA-04 · live HOST | HOST capture, encoder, authenticated WSS | The Windows source picker, cursor crop, foreground Stop, negotiation and three codecs are implemented. The desktop and integration tests used synthetic images. Verify real multi-monitor capture and scaling on the HOST. |
| MEDIA-05 · device live decoder | Image receiver, decoder, LVGL adapter | At boot 388, each local codec run completed 20/20 measured pictures. For requests of 5 and 10 fps respectively, 960×540 Cinepak displayed 4.786 and 5.100 fps, 512×512 H.264 displayed 3.257 and 3.275 fps, and 512×512 JPEG displayed 4.093 fps at both settings. Desktop-to-panel rate and input latency over an active Wi-Fi share remain open. [Board timings](../../../evidence/hardware/20261120-magnifier-cadence-ccc6a04b2eb8.cadence.json) |
| MEDIA-06 · release | Hardware/host acceptance | One-hour stream and eight-hour mixed load, card removal, re-pair/revoke, sleep/wake and OTA recovery. |

Opening a movie from the card shows its filename, playback state, elapsed media time when available, and frame count. Prepared files also show a progress bar and total frame count. Pause holds the displayed picture and suspends the presentation clock. The local scale controls remain available during the pause.

**Fit** is the default. It centers the complete decoded picture in the 1024 � 540 video area below the controls and preserves its aspect ratio. **1:1** maps one decoded pixel to one LCD pixel. For example, a prepared 320 � 180 source can have a 160 � 90 decoded picture. Fit presents it at 960 � 540 and 1:1 presents it at 160 � 90. The selected size carries over to later files until the device restarts. A four-row RGB565 strip handles enlargement without allocating a second full-size output picture.

File playback keeps the last picture visible for its duration before opening the playback summary. **Play again** reopens that file in the same reconstruction mode and starts at the first picture. **Browse files** returns to the current card page. During playback, **Stop** cancels the reader and returns to the same file's mode choices when the worker releases the file. **Home** returns directly to the mouse screen, including while a stop is pending. Removing the card also returns to Home.

On an engineering build, a running `ui media-card-run` fixture accepts `ui media-card-action stop` or `home` after a picture is displayed. After Stop, `play` chooses the same mode from the file detail. After successful completion, `again` selects Play again. These actions require the corresponding visible, enabled control in the current fixture.

In ESP32-S3 media builds the zlib decoder fills long single-byte repeats and copies long,
nonoverlapping matches directly from completed output. It keeps the upstream copy loop for
other overlaps and its usual history-window handling. Existing KVC1 companions use this path.
On the same 73-picture 1080p companions, the release reader used 8.54% fewer fixed-clock
Xtensa emulator ticks in Exact mode, 9.55% fewer in Reduced and 9.03% fewer in Large bank.
These timings cover the reader with memory-backed input. Card I/O and physical LCD refresh
require separate measurements.

The summary reports **LCD** fps from completed refreshes and **Source** fps from the
presentation timestamps of the same pictures in the decoder or prepared companion. For
variable-rate video, Source is the average over the first-to-last picture interval. It is
omitted for a single picture or a missing, duplicated or decreasing timestamp. The engineering
result includes this interval as `media_window_us` alongside the LCD interval `window_us`.

With the engineering firmware at Home and the prepared clips on the card, run
`python test/hil/card_playback_regression.py measurements.json --port COM3 --rate-matrix`.
This runs Exact, Reduced and Large bank on each of the 12 MP4s and on the original 1080p
clip. Each result records the measured LCD rate and median, 95th and 99th percentile times
between completed picture refreshes. The runner checks the reported media timespan and frame
count against the fixture. The default run also checks the LCD pixels during prepared 1080p
playback. The rate matrix uses the timed policy to keep pixel verification off the timing path.

For prepared clips the engineering rate matrix also exports `prepared_stages`. Its reader record measures source verification, the first buffered companion read, and subsequent refills. The worker record measures time waiting for a free surface, reading and reconstructing frames, waiting for presentation, draining the final refresh, and holding the last picture. Subtract `refill_us` from `read_us` to estimate reconstruction and other non-refill work. The display record measures the interval from selecting a picture to completing its refresh. Parts of this interval overlap the worker reading the next frame. The worker reports those overlaps separately. Older recordings retain their original timing fields.

Use `--temporal-rates` to run Temporal blend and Smart loose on the same 12 MP4 files
with their .kvf and .kvs companions on the card. Both switches together exercise 63
file/mode combinations. The refresh log reports the number of intervals used for
percentiles, the number observed, and the longest gap across the entire playback.
Percentiles cover the last 256 intervals for longer clips. Older firmware leaves their
scope unreported in the exported JSON.

Direct H.264 playback uses the decoder's duration, with the existing 41,667 µs fallback when timing
is unavailable or out of bounds. Prepared playback uses the duration in the final KVC1 record.
Both paths measure from the completed LCD refresh. The Stop control remains responsive during
this interval. The FPS counter measures from the first completed picture refresh to the last.

The regenerated pan-cut sample has its own digest and quality references. The historical pan-cut bitstream and historical all-intra/flat fixtures are unavailable. Keep timing from x86 and results from standalone RAM tests distinct from interactive performance. [Measured results and reproduction](../../test/reports/2026-11-19_extension-media_022168e4.yaml)

The per-file heap measurements, spatial reconstruction results and headless presentation evidence are recorded in the [device media report](../../test/reports/2026-11-19_device-media_022168e4.yaml).

On unit 1 at boot 388, the 512×512 magnifier test pattern matched all 262,144 reference pixels in the submitted 1024×600 framebuffer. Its one-pixel border was clear on all four sides and the capture status, Stop and Home labels matched their reference positions. The test captured a local pattern over UART. [Board screenshot](../../../evidence/hardware/20261120-magnifier-cadence-ccc6a04b2eb8.png)
