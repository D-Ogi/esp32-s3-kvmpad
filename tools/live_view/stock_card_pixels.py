# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Read presented H.264 frames from local FFmpeg tools and reproduce the LCD's integer conversion."""

from array import array
from contextlib import contextmanager, ExitStack
from fractions import Fraction
import json
from pathlib import Path
import subprocess
import sys

# Match clip(component / 256) in media_decode.c. Arithmetic shift and C truncation
# differ only for negative results, which both clip to zero.
CLIP = bytes(max(0, min(255, n)) for n in range(-512, 768))
UNKNOWN_PTS = -(1 << 63)
CARD_DEMUXERS = {".mp4": "mov", ".h264": "h264", ".264": "h264"}
PICTURE_FIELDS = (
    "width,height,pix_fmt,sample_aspect_ratio,color_range,color_space,color_transfer,color_primaries"
)
FRAME_FIELDS = PICTURE_FIELDS + ",interlaced_frame,best_effort_timestamp,duration,pkt_duration,pkt_pos"


def validate_side_data(data):
    kind = data.get("side_data_type", "").lower()
    if "display matrix" in kind or float(data.get("rotation", 0)) != 0:
        raise ValueError("Video display transforms cannot be represented in the card cache.")
    if any(s in kind for s in ("mastering display", "content light", "hdr", "dovi", "dolby vision")):
        raise ValueError("HDR metadata is unsupported. Use SDR BT.601 or BT.709.")


def validate_picture(info):
    w, h = int(info["width"]), int(info["height"])
    if not (2 <= w <= 1920 and 2 <= h <= 1080):
        raise ValueError("Video dimensions exceed the 1920 × 1080 source limit.")
    fmt = info.get("pix_fmt")
    if fmt not in ("yuv420p", "yuvj420p"):
        raise ValueError("Exact card preparation requires 8-bit planar YUV 4:2:0 H.264.")
    sar = info.get("sample_aspect_ratio", "N/A")
    if sar not in ("N/A", "unknown", "0:1") and Fraction(sar.replace(":", "/")) != 1:
        raise ValueError("Non-square sample aspect ratio cannot be represented in the card cache.")
    if info.get("interlaced_frame", "0") != "0":
        raise ValueError("Interlaced pictures are unsupported.")
    matrix = info.get("color_space", "unknown")
    level = info.get("color_range", "unknown")
    primaries = info.get("color_primaries", "unknown")
    transfer = info.get("color_transfer", "unknown")
    if (
        matrix not in ("unknown", "bt709", "bt470bg", "smpte170m")
        or level not in ("unknown", "tv", "pc")
        or primaries not in ("unknown", "bt709", "bt470bg", "smpte170m", "bt470m")
        or transfer
        not in ("unknown", "bt709", "bt470bg", "smpte170m", "gamma22", "gamma28", "iec61966-2-1")
    ):
        raise ValueError("Unsupported color metadata. Use SDR BT.601 or BT.709.")
    return w, h, fmt, level == "pc" or fmt == "yuvj420p", matrix == "bt709"


def half_row(plane, stride, height, y, count):
    # For the last unpaired chroma row/column production repeats the edge sample.
    a = plane[2 * y * stride : (2 * y + 1) * stride]
    j = min(2 * y + 1, height - 1) * stride
    b = plane[j : j + stride]
    even = count if 2 * count <= stride else count - 1
    row = bytearray(
        (p + q + r + s + 2) >> 2
        for p, q, r, s in zip(
            a[0 : 2 * even : 2], a[1 : 2 * even : 2], b[0 : 2 * even : 2], b[1 : 2 * even : 2]
        )
    )
    if even < count:
        row.append((a[-1] + b[-1] + 1) >> 1)
    return row


def render(raw, width, height, full, bt709):
    w, h = width // 2, height // 2
    cw, ch = (width + 1) // 2, (height + 1) // 2
    y_end = width * height
    view = memoryview(raw)
    yplane, uplane, vplane = view[:y_end], view[y_end : y_end + cw * ch], view[y_end + cw * ch :]
    cy = 256 if full else 298
    if full:
        rv, gu, gv, bu = (403, 48, 120, 475) if bt709 else (359, 88, 183, 454)
    else:
        rv, gu, gv, bu = (459, 55, 136, 541) if bt709 else (409, 100, 208, 516)
    luma = [cy * (value - (0 if full else 16)) + 128 for value in range(256)]
    rgb = array("H")
    for y in range(h):
        if y % 2 == 0:
            u = half_row(uplane, cw, ch, y // 2, (w + 1) // 2)
            v = half_row(vplane, cw, ch, y // 2, (w + 1) // 2)
            chroma = [
                (rv * (b - 128), -gu * (a - 128) - gv * (b - 128), bu * (a - 128)) for a, b in zip(u, v)
            ]
        samples = half_row(yplane, width, height, y, w)
        for x, luminance in enumerate(samples):
            r, g, b = chroma[x // 2]
            yy = luma[luminance]
            rgb.append(
                ((CLIP[512 + ((yy + r) >> 8)] >> 3) << 11)
                | ((CLIP[512 + ((yy + g) >> 8)] >> 2) << 5)
                | (CLIP[512 + ((yy + b) >> 8)] >> 3)
            )
    if sys.byteorder != "little":
        rgb.byteswap()
    return rgb.tobytes()


def microseconds(value, base):
    value = int(value) * base.numerator * 1000000
    n = (2 * abs(value) + base.denominator) // (2 * base.denominator)
    return -n if value < 0 else n


@contextmanager
def command(args):
    with subprocess.Popen(
        args,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    ) as process:
        try:
            yield process.stdout
            process.stdout.close()
            if process.wait() != 0:
                raise ValueError(f"{Path(args[0]).stem} could not decode the video.")
        finally:
            if process.poll() is None:
                process.kill()


def sample_durations(probe, track):
    """Read MP4 sample durations in decoding order and identify each sample by file offset."""
    previous = position = None
    presented_ticks = 0
    count = 0
    while line := probe.readline(513):
        if len(line) > 512:
            raise ValueError("Packet metadata exceeds the inspection limit.")
        fields = dict(item.split(b"=", 1) for item in line.strip().split(b"|") if b"=" in item)
        if b"pos" not in fields or b"dts" not in fields:
            continue
        try:
            dts, pos = int(fields[b"dts"]), int(fields[b"pos"])
        except ValueError:
            raise ValueError("Reordered MP4 samples require file offsets and decoding timestamps.") from None
        if position is not None:
            interval = dts - previous
            presented_ticks += interval
            yield position, interval
        previous = dts
        # An MP4 edit can retain an entire GOP for decoding before its first displayed picture.
        position = None if b"D" in fields.get(b"flags", b"") else pos
        count += 1
    if position is not None:
        duration = track.get("duration_ts")
        complete = str(count) == str(track.get("nb_frames"))
        yield position, int(duration) - presented_ticks if complete and duration not in (None, "N/A") else 0


@contextmanager
def frames(source, ffmpeg="ffmpeg", ffprobe="ffprobe"):
    source = Path(source).resolve()
    demux = CARD_DEMUXERS.get(source.suffix.lower())
    if not demux or not source.is_file():
        raise ValueError("Choose a local .mp4, .h264 or .264 file.")
    options = [
        "-max_streams",
        "8",
        "-max_pixels",
        "2359296",
        "-protocol_whitelist",
        "file",
        "-f",
        demux,
    ]
    probe = [ffprobe, "-v", "error", *options]
    with command(
        [
            *probe,
            "-select_streams",
            "v",
            "-show_entries",
            "stream=index,codec_name,time_base,has_b_frames,duration_ts,nb_frames,"
            + PICTURE_FIELDS
            + ":stream_side_data=side_data_type,rotation",
            "-of",
            "json",
            "-i",
            source,
        ]
    ) as output:
        data = output.read(32769)
        if len(data) > 32768:
            raise ValueError("Stream metadata exceeds the inspection limit.")
        streams = json.loads(data)["streams"]
    if len(streams) != 1 or streams[0].get("codec_name") != "h264":
        raise ValueError("Choose a file with a single H.264 video track.")
    stream = streams[0]
    for side in stream.get("side_data_list", ()):
        validate_side_data(side)
    w, h, fmt, _, _ = validate_picture(stream)
    base = Fraction(stream["time_base"])
    if base <= 0:
        raise ValueError("Invalid video time base.")
    plane_bytes = w * h + 2 * ((w + 1) // 2) * ((h + 1) // 2)
    with ExitStack() as stack:
        durations = None
        if demux == "mov" and int(stream.get("has_b_frames", 0)) > 0:
            packets = stack.enter_context(
                command([
                    *probe, "-select_streams", "v:0", "-show_packets", "-show_entries", "packet=dts,pos,flags",
                    "-of", "compact=p=1:nk=0", "-i", source,
                ])
            )
            durations = sample_durations(packets, stream)
        meta = stack.enter_context(
            command(
                [
                    *probe,
                    "-select_streams",
                    "v:0",
                    "-show_frames",
                    "-show_entries",
                    "frame=" + FRAME_FIELDS + ":frame_side_data=side_data_type,rotation",
                    "-of",
                    "compact=p=1:nk=0",
                    "-i",
                    source,
                ]
            )
        )
        raw = stack.enter_context(
            command(
                [
                    ffmpeg,
                    "-nostdin",
                    "-v",
                    "error",
                    "-xerror",
                    *options,
                    "-err_detect",
                    "explode",
                    "-noautorotate",
                    "-i",
                    source,
                    "-map",
                    f"0:{int(stream['index'])}",
                    "-fps_mode",
                    "passthrough",
                    "-c:v",
                    "rawvideo",
                    "-pix_fmt",
                    "+",
                    "-f",
                    "rawvideo",
                    "pipe:1",
                ]
            )
        )

        def records():
            pending = {}
            while line := meta.readline(4097):
                if len(line) > 4096:
                    raise ValueError("Frame metadata exceeds the inspection limit.")
                parts = line.decode("ascii").strip().split("|")
                fields = {}
                for part in parts[1:]:
                    if "=" not in part:
                        continue
                    key, value = part.split("=", 1)
                    name = key.rsplit(":", 1)[-1]
                    if name in ("side_data_type", "rotation"):
                        validate_side_data({name: value})
                    else:
                        fields[key] = value
                if parts[0] != "frame":
                    continue
                fw, fh, pf, full, bt709 = validate_picture(fields)
                if (fw, fh, pf) != (w, h, fmt):
                    raise ValueError("The stream changes dimensions or pixel format.")
                pixels = raw.read(plane_bytes)
                if len(pixels) != plane_bytes:
                    raise ValueError("Incomplete YUV picture returned by FFmpeg.")
                ts = fields.get("best_effort_timestamp", "N/A")
                pts = UNKNOWN_PTS if ts == "N/A" else microseconds(ts, base)
                ticks = fields.get("duration", fields.get("pkt_duration", "N/A"))
                if durations is not None:
                    # A frame's display PTS and its sample's duration come from different clocks.
                    # Join by file offset, as the device does before the decoder reorders pictures.
                    try:
                        position = int(fields["pkt_pos"])
                    except (KeyError, ValueError):
                        raise ValueError("FFprobe did not report the reordered picture's file offset.") from None
                    while position not in pending:
                        sample = next(durations, None)
                        if sample is None or len(pending) >= 64 or sample[0] in pending:
                            raise ValueError("Could not match the reordered MP4 sample to its picture.")
                        pending[sample[0]] = sample[1]
                    ticks = pending.pop(position)
                    if ticks <= 0:
                        ticks = "N/A"
                duration = microseconds(ticks, base) if ticks != "N/A" else 0
                yield (
                    duration if 0 < duration <= 1000000 else 41667,
                    pts,
                    render(pixels, w, h, full, bt709),
                )
            if raw.read(1):
                raise ValueError("Decoder and metadata frame counts differ.")

        yield w // 2, h // 2, records()
