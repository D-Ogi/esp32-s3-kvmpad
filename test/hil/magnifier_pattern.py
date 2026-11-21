# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Verify the local MAG-13 fixture through UART and save its checked native-size PNG."""
import argparse
from io import BytesIO
from pathlib import Path
import re
import struct
import sys
import time

from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE), str(HERE.parent / "live_view")]
import kvm_serial
from check_lcd_pattern import verify
from ui_snapshot import rgb565_to_rgb888

WIDTH, HEIGHT = 1024, 600
SCAN = re.compile(rb"MAG13_SCAN fb_available=(\d+) fb_equal=(\d+) vsync_delta=(\d+) bounce_delta=(\d+)")
SOURCE = re.compile(rb"\bSNAP_SOURCE (\w+)\b")
FAULT = re.compile(rb"Guru Meditation|Backtrace:|assert failed|Brownout detector|CORRUPT HEAP|Rebooting\.\.\.|ESP-ROM:|BOOT_OK")


def scan_stats(line):
    match = SCAN.search(line)
    if match:
        available, equal, vsync, bounce = map(int, match.groups())
        if available in (1, 2, 3) and equal and equal & ~available == 0 and vsync and bounce:
            return available, equal, vsync, bounce
    return None


def verified_snapshot(lines):
    pixels = bytearray()
    digest = 2166136261
    scan = None
    begun = False
    scanout = False
    y = 0
    colors = [bytes(rgb565_to_rgb888(i)) for i in range(65536)]
    for line in lines:
        if FAULT.search(line):
            raise ValueError("Firmware fault or restart during the chart")
        if line.startswith(b"SNAP_BEGIN"):
            if begun or line != b"SNAP_BEGIN div=1":
                raise ValueError("Expected exactly one native snapshot")
            begun = True
        elif SCAN.search(line):
            current = scan_stats(line)
            if not begun or scan or not current:
                raise ValueError("Missing or repeated guarded framebuffer match")
            scan = current
        elif source := SOURCE.search(line):
            if not scan or scanout or source[1] != b"scanout":
                raise ValueError("The guarded chart did not return the submitted LCD scanout")
            scanout = True
        elif line.startswith(b"SNAP_ROW "):
            match = re.fullmatch(rb"SNAP_ROW (\d+) ([0-9A-F]{4096})", line)
            if not scanout or not match or int(match[1]) != y or y >= HEIGHT:
                raise ValueError("Unexpected chart row or missing framebuffer verification")
            for (p,) in struct.iter_unpack(">H", bytes.fromhex(match[2].decode("ascii"))):
                digest = ((digest * 16777619) & 0xffffffff) ^ p
                pixels.extend(colors[p])
            y += 1
        elif line.startswith(b"SNAP_END "):
            expected = f"SNAP_END w={WIDTH} h={HEIGHT} crc={digest:08X} err=ESP_OK".encode()
            if not scanout or y != HEIGHT or line != expected:
                raise ValueError("Incomplete chart or snapshot checksum mismatch")
            screen = Image.frombytes("RGB", (WIDTH, HEIGHT), bytes(pixels))
            verify(screen)
            return screen, digest, scan
    raise ValueError("Chart transfer ended without a verified snapshot")


def uart_lines(port):
    deadline = time.monotonic() + 420
    partial = bytearray()
    while time.monotonic() < deadline:
        partial.extend(port.read(8192))
        complete = partial.split(b"\n")
        partial = complete.pop()
        for line in complete:
            line = bytes(line).strip()
            yield line
            if line.startswith(b"SNAP_END "):
                return
        if len(partial) > 8192:
            raise ValueError("Unexpected UART line length")
    raise TimeoutError("Timed out receiving the local LCD chart")


def capture(port_name, output):
    if output.exists():
        raise FileExistsError(output)
    with kvm_serial.open_port(port_name) as port:
        def boot_count():
            reply, ok = kvm_serial.run_cmd(port, "info", 5)
            value = re.search(r"\bboot_count=(\d+)\b", reply)
            if not ok or not value:
                raise ValueError("Missing boot count")
            return int(value[1])
        boot = boot_count()
        text, ok = kvm_serial.run_cmd(port, "ui media-pattern", 8)
        if not ok or "UI_MEDIA_PATTERN status=ESP_OK" not in text:
            raise ValueError("The diagnostic firmware must be on the Home screen")
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                reply, ok = kvm_serial.run_cmd(port, "ui media-pattern-scan", 2)
                if ok and scan_stats(reply.encode()):
                    break
                time.sleep(.1)
            else:
                raise ValueError("No matching LCD framebuffer with advancing scanout counters")
            port.reset_input_buffer()
            # The LVGL task rechecks the chart before copying the submitted framebuffer.
            port.write(b"ui media-pattern-snap\r\n")
            port.flush()
            lines = uart_lines(port)
            try:
                screen, digest, scan = verified_snapshot(lines)
            except ValueError:
                # Finish the console transfer before asking the board to return Home.
                for _ in lines:
                    pass
                raise
        finally:
            _, stopped = kvm_serial.run_cmd(port, "ui media-stop", 6)
            reply, ok = kvm_serial.run_cmd(port, "ui shell status", 4)
            if not stopped or not ok or "UI_SHELL active=yes" not in reply or boot_count() != boot:
                raise ValueError("The panel did not confirm its return to Home in the same boot")
    png = BytesIO()
    screen.save(png, format="PNG")
    with output.open("xb") as file:
        file.write(png.getbuffer())
    print(f"MAG-13 verified {output}, SNAP_SOURCE scanout, boot {boot}, RGB565 FNV {digest:08X}, "
          f"available/equal masks {scan[0]}/{scan[1]}, "
          f"VSYNC/bounce deltas {scan[2]}/{scan[3]}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="new PNG path outside the repository")
    parser.add_argument("--port", default="COM3")
    args = parser.parse_args()
    capture(args.port, args.output)
