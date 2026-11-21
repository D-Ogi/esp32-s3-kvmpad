# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""RAM-only, demand-driven frame production for the foreground Windows HOST."""
import asyncio
import os
import re
import subprocess
import struct
import time
import zlib
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "live_view"))
from framing import FORMATS, HEADER, MAX_PAYLOAD, encode, negotiate

MODES = {"ZOOM_2X": 1, "PIXEL_1X": 2, "OVERVIEW": 4}
CINEPAK_BYTES = 10 + 12 + 4 + 256 * 6 + 4 + 960 * 540 // 16
PICKER_TIMEOUT = 45
CURSOR_ENTRY_TIMEOUT = 22  # Native pointer-entry limit is 20 s, followed by its terminal pipe record.


CROP_PREFIX = struct.Struct("<4siiiiHHHHB3s")


class CaptureError(PermissionError):
    """Local explanations for foreground display. Never echo picker output."""
    reasons = {
        "approval_timeout": "The display approval request timed out.",
        "picker": "The Windows display picker closed before confirming a monitor.",
        "launch": "Windows could not start the configured capture helper.",
        "arguments": "The capture helper rejected its launch arguments.",
        "parent": "The capture helper could not verify its parent process.",
        "desktop": "Windows switched away from the interactive desktop.",
        "closed": "The Windows capture window or its control pipe closed.",
        "topology": "The display configuration changed. Select the monitor again.",
        "suspended": "Windows suspended the display capture session.",
        "itemclosed": "Windows closed the selected capture item.",
        "window": "Windows could not initialize the foreground capture window.",
        "select": "Windows could not complete the display selection.",
        "cancel": "Display selection was canceled.",
        "monitor": "Windows could not identify the selected monitor. Select a display in the picker.",
        "pickedwindow": "A window was selected. Choose a display in the Windows picker.",
        "wrongdisplay": "This HOST is restricted to another monitor. Start sharing again and choose the monitor named in the approval prompt.",
        "displayenv": "KVMPAD_HOST_DISPLAY is invalid. Use a Windows display name such as DISPLAY2, or remove the setting.",
        "name": "Windows did not return a usable monitor device name.",
        "gpu": "Windows could not create the graphics device for capture.",
        "pool": "Windows could not open the frame pool for the selected monitor.",
        "start": "Windows could not start capture on the selected monitor.",
        "cursor": "The pointer left the selected display. Start sharing again to select a display.",
        "cursorpos": "Windows could not read the physical pointer position.",
        "cursorwait": "The pointer did not reach the selected display in time.",
        "geometry": "The captured frame does not match the selected monitor's physical geometry.",
        "frame": "Windows could not read or encode the requested display frame.",
        "selection_timeout": "Windows display selection timed out.",
        "frame_timeout": "The capture helper timed out waiting for a display frame.",
    }

    def __init__(self, stage, hresult=None):
        self.stage = stage if stage in self.reasons else "picker"
        message = self.reasons.get(stage, self.reasons["picker"])
        if stage in self.reasons and type(hresult) is int and 0 < hresult <= 0xffffffff:
            message += f" (HRESULT 0x{hresult:08X})"
        super().__init__(message)

    @classmethod
    def from_record(cls, record, fallback):
        match = re.fullmatch(rb"KCE1 ([a-z]{1,12}) ([0-9a-fA-F]{8})\r?\n", record)
        if match:
            stage = match[1].decode("ascii")
            if stage in cls.reasons:
                return cls(stage, int(match[2], 16))
        return cls(fallback)


def source_crop(codec, mode):
    fmt, factor = FORMATS[codec], MODES[mode]
    return [fmt.width * factor // 2, fmt.height * factor // 2]


def required_display():
    value = os.environ.get("KVMPAD_HOST_DISPLAY")
    if value is None:
        return None
    name = value.removeprefix("\\\\.\\")
    if not re.fullmatch(r"DISPLAY[0-9]{1,5}", name):
        raise CaptureError("displayenv")
    return "\\\\.\\" + name


def capture_review(codec, mode, *, system_picker=False):
    width, height = source_crop(codec, mode)
    image = FORMATS[codec]
    zoom = {"OVERVIEW": "Overview", "PIXEL_1X": "Pixel 1x", "ZOOM_2X": "Zoom 2x"}[mode]
    format_name = {"jpeg": "JPEG", "h264-intra": "H.264", "cinepak-intra": "Cinepak"}[codec]
    display = required_display()
    target = display[4:] if display else "an entire monitor"
    instruction = f"Choose {target} in the Windows picker and move the pointer onto it." if system_picker else (
        f"After Allow once, choose {target} and move the pointer onto it.")
    return ("Share the area around the pointer with the paired KVMpad?\n"
            f"{zoom} · {format_name} · Requested {width} × {height} px · On KVMpad {image.width} × {image.height} px\n"
            + instruction)


def crop_record(prefix, message, codec, mode):
    magic, x, y, left, top, w, h, px, py, edge, reserved = CROP_PREFIX.unpack(prefix)
    cw, ch = source_crop(codec, mode)
    if (magic != b"KCM1" or reserved != bytes(3) or not (0 < w <= cw and 0 < h <= ch)
            or not (left <= x < left+w and top <= y < top+h) or left+w-1 > 2147483647 or top+h-1 > 2147483647
            or (px, py) != ((cw-w)//2, (ch-h)//2)
            or edge != int(w < cw or h < ch or x-left != cw//2 or y-top != ch//2)):
        raise ValueError("Invalid cursor geometry")
    stream, sequence = struct.unpack_from("<II", message, 8)
    image_crc, = struct.unpack_from("<I", message, HEADER.size)
    record = prefix + struct.pack("<III", stream, sequence, image_crc)
    return (record + struct.pack("<I", zlib.crc32(record))).hex()


class FrameRate:
    """Demand rate, scoped to one image grant. Timings use only the HOST monotonic clock."""
    def __init__(self, maximum=5):
        self.maximum = maximum
        self.reset()

    def reset(self):
        self.fps = min(5, self.maximum)
        self.healthy = 0

    def observe(self, seconds, shown):
        if not shown or seconds > 1 / self.fps:
            capacity = int(.85 / max(seconds, .001))
            self.fps = max(1, min(self.fps - 1, capacity))
            self.healthy = 0
        elif self.fps < self.maximum and seconds < .8 / (self.fps + 1):
            self.healthy += 1
            if self.healthy >= 6:
                self.fps += 1
                self.healthy = 0
        else:
            self.healthy = 0


async def terminate(process):
    if process.poll() is None:
        try:
            process.kill()
        except ProcessLookupError:
            pass
    await asyncio.to_thread(process.wait)
    try:
        process.stdin.close()
    except OSError:
        pass  # The provider can exit with a request still in the buffered input pipe.
    finally:
        process.stdout.close()


def child(*args, diagnostics=False):
    # Older helpers return only the final monitor name. Opt in on the helper's
    # own process so a newer helper also keeps working with an older broker.
    env = {**os.environ, "KVMPAD_PICKER_PROGRESS": "1", "KVMPAD_PICKER_LABEL": "1"} if diagnostics else None
    return subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE if diagnostics else subprocess.DEVNULL,
                            env=env, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))


def exchange(process, command, size):
    process.stdin.write(command)
    process.stdin.flush()
    data = process.stdout.read(size)
    if len(data) != size:
        raise EOFError("Provider stopped")
    return data


class Capture:
    def __init__(self, picker, ffmpeg):
        self.picker = picker
        self.ffmpeg = ffmpeg
        self.process = None
        self.encoder = None
        self.sized_packets = True
        self.encoder_step = None
        self.encoder_frame = 0
        self.revoked = False
        self.selected = False
        self.small_frames = 0
        self.diagnostic = None

    async def picker_failure(self):
        fallback = "frame" if self.selected else "picker"
        if self.diagnostic is not None:
            try:
                record = await asyncio.wait_for(asyncio.shield(self.diagnostic), .5)
            except TimeoutError:
                return CaptureError(fallback)
            return CaptureError.from_record(record, fallback)
        return CaptureError(fallback)

    async def open(self, codec, mode, on_cursor_wait=None):
        if self.revoked or self.process is not None:
            raise PermissionError("Capture grant expired or already used")
        display = required_display()
        required_record = (display + "\n").encode("ascii") if display else None
        self.mode = mode
        self.codec = codec
        fmt = FORMATS[codec]
        self.width, self.height = fmt.width, fmt.height
        self.quality_step = 0
        try:
            self.process = child(self.picker, codec, str(MODES[mode]), "--parent", str(os.getpid()), diagnostics=True)
        except OSError as exc:
            raise CaptureError("launch") from exc
        self.diagnostic = asyncio.create_task(asyncio.to_thread(self.process.stderr.readline, 64))
        timeout_stage = "selection_timeout"
        try:
            label = await asyncio.wait_for(asyncio.to_thread(self.process.stdout.readline, 96), PICKER_TIMEOUT)
            display_record = rb"\\\\\.\\DISPLAY[0-9]{1,5}\n"
            hint = None
            if label.startswith(b"KCP2 CURSOR "):
                hint = label[len(b"KCP2 CURSOR "):]
                if not re.fullmatch(display_record, hint):
                    raise CaptureError("name")
                if required_record and hint != required_record:
                    raise CaptureError("wrongdisplay")
            if hint is not None or label == b"KCP1 CURSOR\n":
                timeout_stage = "cursorwait"
                if on_cursor_wait and not self.revoked and (hint is not None or not required_record):
                    if hint is None:
                        await on_cursor_wait()
                    else:
                        await on_cursor_wait(hint[:-1].decode("ascii"))
                label = await asyncio.wait_for(asyncio.to_thread(self.process.stdout.readline, 96), CURSOR_ENTRY_TIMEOUT)
            if not label:
                raise await self.picker_failure()
            if self.revoked or not re.fullmatch(display_record, label):
                raise CaptureError("name")
            if required_record and label != required_record:
                raise CaptureError("wrongdisplay")
            if hint is not None and label != hint:
                raise CaptureError("name")
        except TimeoutError as exc:
            self.revoke()
            raise CaptureError(timeout_stage) from exc
        except (Exception, asyncio.CancelledError):
            self.revoke()
            raise
        self.selected = True
        return label.strip().decode("ascii")

    async def frame(self, stream, sequence):
        message, _ = await self._frame(stream, sequence, False)
        return message

    async def frame_with_crop(self, stream, sequence):
        return await self._frame(stream, sequence, True)

    async def require_selected(self):
        if self.revoked or not self.selected:
            raise PermissionError("Capture ended")
        if self.process.poll() is not None:
            raise await self.picker_failure()

    async def _frame(self, stream, sequence, metadata):
        await self.require_selected()
        timestamp = int(time.monotonic() * 1000) & 0xffffffff
        def read_frame():
            proc = self.process
            prefix = exchange(proc, b"M", CROP_PREFIX.size) if metadata else None
            def read(n):
                if not metadata:
                    return exchange(proc, b"F", n)
                data = proc.stdout.read(n)
                if len(data) != n:
                    raise EOFError("Picker closed")
                return data
            if self.codec == "jpeg":
                count, = struct.unpack("<I", read(4))
                if count > MAX_PAYLOAD:
                    raise ValueError("Invalid local JPEG size")
                payload = proc.stdout.read(count)
                if len(payload) != count:
                    raise EOFError("Picker closed")
            else:
                payload = read(CINEPAK_BYTES if self.codec == "cinepak-intra" else self.width*self.height*4)
            return payload, prefix
        try:
            payload, prefix = await asyncio.wait_for(asyncio.to_thread(read_frame), 2)
        except TimeoutError as exc:
            raise CaptureError("frame_timeout") from exc
        except (EOFError, OSError) as exc:
            raise await self.picker_failure() from exc
        await self.require_selected()
        if self.codec == "h264-intra":
            payload = await self.encode_h264(payload)
        await self.require_selected()
        if not payload:
            self.quality_step = min(self.quality_step + 1, 4)
            self.small_frames = 0
            return None, None
        if self.codec == "h264-intra" and self.quality_step:
            self.small_frames = self.small_frames + 1 if len(payload) < MAX_PAYLOAD//2 else 0
            if self.small_frames >= 12:
                self.quality_step -= 1
                self.small_frames = 0
        message = encode(self.codec, stream, sequence, timestamp, payload)
        return message, crop_record(prefix, message, self.codec, self.mode) if metadata else None

    async def encode_h264(self, pixels):
        if len(pixels) != self.width * self.height * 4:
            raise ValueError("Incomplete H.264 input frame")
        if self.encoder and self.encoder_step != self.quality_step:
            await self.close_encoder()
        try:
            return await self.encode_packet(pixels)
        except (EOFError, BrokenPipeError):
            # Older FFmpeg builds exit on the packet statistics option before reading frame zero.
            if not self.sized_packets or self.encoder_frame or self.revoked:
                raise
            self.sized_packets = False
            return await self.encode_packet(pixels)

    async def start_encoder(self):
        packet_options = ("-stats_mux_pre", "pipe:1", "-stats_mux_pre_fmt", "{n} {size}", "-flush_packets", "1")
        start = asyncio.create_task(asyncio.to_thread(child, self.ffmpeg, "-hide_banner", "-loglevel",
            "error", "-nostdin", "-f", "rawvideo", "-pix_fmt", "bgra", "-s", f"{self.width}x{self.height}",
            "-probesize", "32", "-fpsprobesize", "0", "-i", "pipe:0", "-threads", "1", "-c:v", "libx264",
            "-preset", "ultrafast", "-tune", "zerolatency", "-profile:v", "baseline", "-pix_fmt", "yuv420p",
            "-crf", str(23 + self.quality_step * 5), "-x264-params", "keyint=1:repeat-headers=1:annexb=1",
            *(packet_options if self.sized_packets else ("-frames:v", "1")), "-f", "h264", "pipe:1"))
        try:
            self.encoder = await asyncio.shield(start)
        except asyncio.CancelledError:
            await terminate(await start)
            raise
        self.encoder_step = self.quality_step
        self.encoder_frame = 0

    def write_picture(self, process, pixels):
        process.stdin.write(pixels)
        if self.sized_packets:
            process.stdin.flush()
            # The raw H.264 muxer writes one Annex B packet immediately after its statistics line.
            line = process.stdout.readline(40)
            if not line:
                raise EOFError("H.264 encoder closed")
            record = re.fullmatch(rb"([0-9]{1,12}) ([0-9]{1,10})\r?\n", line)
            if not record or int(record[1]) != self.encoder_frame or not int(record[2]):
                raise ValueError("Invalid H.264 encoder packet header")
            size = int(record[2])
            if size > MAX_PAYLOAD:
                return None
            payload = process.stdout.read(size)
            if len(payload) != size:
                raise ValueError("Incomplete H.264 encoder packet")
            return payload
        process.stdin.close()
        payload = process.stdout.read(MAX_PAYLOAD + 1)
        if len(payload) > MAX_PAYLOAD:
            return None
        if process.wait() or not payload:
            raise RuntimeError("Encoder unavailable")
        return payload

    async def encode_packet(self, pixels):
        if self.revoked:
            raise PermissionError("Capture ended")
        if not self.encoder:
            await self.start_encoder()
        keep = False
        try:
            if self.revoked:
                raise PermissionError("Capture ended")
            packet = await asyncio.wait_for(asyncio.to_thread(self.write_picture, self.encoder, pixels), 2)
            keep = self.sized_packets and packet is not None and not self.revoked
            self.encoder_frame += 1
            return packet
        finally:
            if not keep:
                await self.close_encoder()

    async def close_encoder(self):
        process, self.encoder = self.encoder, None
        if process:
            await terminate(process)

    def revoke(self):
        self.revoked = True
        for process in (self.process, self.encoder):
            if process and process.poll() is None:
                try:
                    process.kill()
                except ProcessLookupError:
                    pass

    async def pause(self):
        try:
            self.process.stdin.write(b"P")
            self.process.stdin.flush()
        except OSError as exc:
            raise await self.picker_failure() from exc
        await self.close_encoder()

    async def wait_closed(self):
        result = await asyncio.to_thread(self.process.wait)
        if result and not self.revoked:
            raise await self.picker_failure()

    async def close(self):
        self.revoke()
        await self.close_encoder()
        if self.process:
            await terminate(self.process)
        if self.diagnostic is not None:
            await self.diagnostic
            self.process.stderr.close()
