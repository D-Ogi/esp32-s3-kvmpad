# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Check the four Windows USB test colors in a snapshot taken by the 7B firmware."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
from threading import Timer
import time
import uuid

class DesktopUnavailable(ValueError):
    def __init__(self):
        super().__init__("The pattern requires the active Windows desktop. The session may be locked or inaccessible.")


def pattern_output(process):
    for line in process.stdout:
        if line.strip() == b"PATTERN_UNAVAILABLE input_desktop":
            raise DesktopUnavailable()
        yield line


def verify_pixels(size, pixel_at, order="RGBW", viewport="640x480", *, portrait_console=False):
    sizes = {(1024, 600): 1, (512, 300): 2, (256, 150): 4, (64, 37): 16}
    if size not in sizes:
        raise ValueError(f"Unexpected 7B snapshot size {size}")
    div = sizes[size]
    layouts = {"640x480": (192, 84, 320, 240), "1024x600": (0, 0, 512, 300)}
    ox, oy, cell_w, cell_h = layouts[viewport]
    if portrait_console:
        # USB Monitor Console 1.0, both pages. The header places the 584x438
        # viewport at logical (8,40). LVGL's Fit uses 8-bit fixed-point scale.
        source_w, source_h = 2 * cell_w, 2 * cell_h
        scale = min(584 * 256 // source_w, 438 * 256 // source_h)
        w, h = source_w * scale // 256, source_h * scale // 256
        ox, oy = 8 + (584 - w) // 2, 40 + (438 - h) // 2
        cell_w, cell_h = w // 2, h // 2
    palette = {"R": (255, 0, 0), "G": (0, 255, 0), "B": (0, 0, 255), "W": (255, 255, 255)}
    counts = []
    for i, name in enumerate(order):
        color = palette[name]
        # Sample each quadrant well inside its JPEG edges and clear of the marker.
        x = ox + cell_w * (i % 2)
        y = oy + cell_h * (i // 2)
        left, right = x + cell_w // 5, x + cell_w - cell_w // 5
        top, bottom = y + cell_h // 5, y + cell_h - cell_h // 5
        if portrait_console:
            # Inverse of logical (599 - physical_y, physical_x), with exclusive ends.
            left, right, top, bottom = top, bottom, 600 - right, 600 - left
            left, right, top, bottom = ((p + div - 1) // div for p in (left, right, top, bottom))
        else:
            left, right, top, bottom = (p // div for p in (left, right, top, bottom))
        good = 0
        for row in range(top, bottom):
            for col in range(left, right):
                if all(abs(a - b) <= 32 for a, b in zip(pixel_at((col, row)), color)):
                    good += 1
        counts.append((good, (right - left) * (bottom - top)))
    if any(good * 100 < total * 98 for good, total in counts):
        raise ValueError(f"{order} quadrant mismatch, matching pixels NW/NE/SW/SE: {counts}")
    return counts


def verify(screen):
    return verify_pixels(screen.size, screen.convert("RGB").getpixel)


def verify_uart(lines, order="RGBW", div=4, viewport="640x480", *, portrait_console=False):
    from ui_snapshot import decode_row, rgb565_to_rgb888

    if div not in (4, 16):
        raise ValueError("Unsupported pattern sampling interval")
    width, height = 1024 // div, 600 // div
    pixels = bytearray()
    digest = 2166136261
    rows = 0
    started = scanout = False
    for line in lines:
        if line.startswith(b"SNAP_BEGIN"):
            if started or line != f"SNAP_BEGIN div={div}".encode():
                raise ValueError("Unexpected snapshot header")
            started = True
        elif b"SNAP_SOURCE " in line:
            if not started or scanout or b"SNAP_SOURCE scanout" not in line:
                raise ValueError("Expected one submitted LCD framebuffer")
            scanout = True
        elif line.startswith((b"SNAP_ROW ", b"SNAP_RLE ")):
            index, raw = decode_row(line, width)
            if not scanout or index != rows or rows >= height:
                raise ValueError("Missing scanout confirmation or invalid LCD row")
            pixels.extend(raw)
            for (value,) in struct.iter_unpack(">H", raw):
                digest = ((digest * 16777619) & 0xffffffff) ^ value
            rows += 1
        elif line.startswith(b"SNAP_END"):
            if rows != height or line != f"SNAP_END w={width} h={height} crc={digest:08X} err=ESP_OK".encode():
                raise ValueError("Incomplete LCD snapshot or checksum mismatch")
            def pixel_at(position):
                x, y = position
                value, = struct.unpack_from(">H", pixels, 2 * (width * y + x))
                return rgb565_to_rgb888(value)
            return verify_pixels((width, height), pixel_at, order, viewport, portrait_console=portrait_console)
    raise ValueError("Board did not complete the LCD snapshot")


def panel_lines(port):
    pending = bytearray()
    deadline = time.monotonic() + 22
    while time.monotonic() < deadline:
        pending.extend(port.read(8192))
        *complete, pending = pending.split(b"\n")
        for line in complete:
            yield bytes(line).strip()
        if len(pending) > 2048:
            raise ValueError("Oversized UART line")


def check_probe(probe, serial, final_frame=False):
    option = "--final-pattern-version" if final_frame else "--pattern-version"
    expected = b"KVM_USB_FINAL_PATTERN 2" if final_frame else b"KVM_USB_PATTERN 1"
    version = subprocess.run([str(probe), option], capture_output=True, timeout=5)
    if version.returncode or version.stdout.splitlines() != [expected]:
        name = "the final static pattern" if final_frame else "RGBW pattern version 1"
        raise ValueError(f"The Windows probe does not support {name}. Rebuild the Windows monitor probe.")
    # Display numbers can change when reconnecting. Print the current USB-identified target before opening the UART.
    topology = subprocess.run([str(probe), serial], stdout=subprocess.PIPE, timeout=10)
    text = topology.stdout.decode("ascii", errors="replace")
    print(text, end="", flush=True)
    if topology.returncode:
        raise ValueError("Windows could not verify this board as a separate 640x480 extended display.")
    display = re.search(r"(?m)^board GDI source=(\\\\\.\\DISPLAY\d{1,5}) origin=", text)
    return display[1] if display else None


def save_result(path, result):
    if path is None:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_path = tempfile.mkstemp(dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as temp:
            json.dump(result, temp, ensure_ascii=True, indent=2)
            temp.write("\n")
        os.replace(temp_path, path)
    finally:
        Path(temp_path).unlink(missing_ok=True)


def quadrant_counts(counts):
    return {field: {"matched": good, "total": total} for field, (good, total) in
            zip(("NW", "NE", "SW", "SE"), counts)}


def check_live(probe, serial, port_name, final_result=None):
    import kvm_serial

    def presented(port):
        reply, ok = kvm_serial.run_cmd(port, "ui usb-monitor-status", 2)
        match = re.search(r"(?m)^USB_MONITOR mounted=([01]) [^\r\n]*?\bpresented=(\d+)\b[^\r\n]*", reply)
        if not ok or not match or (match[1] == "0" and not re.search(r"\bsuspended=1\b", match[0])):
            raise ValueError("Open USB monitor on the board and extend the Windows desktop")
        return int(match[2])

    def decoded_picture(port):
        reply, ok = kvm_serial.run_cmd(port, "ui usb-monitor-status picture", 2)
        if not ok:
            raise ValueError("The board did not answer the decoded-picture query")
        match = re.search(r"(?m)^USB_MONITOR_PICTURE (decoded=(?:absent|all_black|has_nonblack)"
                          r"(?: reference=(?:absent|all_black|has_nonblack|failed) sequence=\d+)?"
                          r"|error=ui_busy)[ \t\r]*$", reply)
        if match:
            return match[1]
        if "usage: ui " in reply:
            return "unavailable on this firmware"
        raise ValueError("Unexpected decoded-picture reply")

    def wait_for_picture(port, initial, order):
        deadline = time.monotonic() + 5
        while presented(port) <= initial:
            if time.monotonic() >= deadline:
                raise ValueError(f"No new USB picture reached the LCD for {order}")
            time.sleep(.1)

    def snapshot(port, order):
        decoded = decoded_picture(port)
        print("Selected USB picture before scanout:", decoded, flush=True)
        port.reset_input_buffer()
        port.write(b"ui snap 4 compact\r\n")
        port.flush()
        try:
            return verify_uart(panel_lines(port), order)
        except ValueError as exc:
            raise ValueError(f"{exc}. USB decoder observation before snapshot: {decoded}") from exc

    final_frame = final_result is not None
    command = "--pattern-final" if final_frame else "--pattern"
    with kvm_serial.open_port(port_name) as port:
        initial = presented(port)
        with subprocess.Popen([str(probe.resolve()), serial, command], stdout=subprocess.PIPE,
                              stdin=subprocess.PIPE if final_frame else None) as pattern:
            watchdog = Timer(130 if final_frame else 45, pattern.kill)
            watchdog.start()
            try:
                for line in pattern_output(pattern):
                    if line.strip() == b"PATTERN_READY 640x480 RGBW":
                        break
                else:
                    raise ValueError("Windows did not show the pattern on the requested board")
                wait_for_picture(port, initial, "RGBW")
                counts = snapshot(port, "RGBW")
                if final_frame:
                    final_result["initial_RGBW"] = quadrant_counts(counts)
                    initial = presented(port)
                    pattern.stdin.write(b"FINAL\n")
                    pattern.stdin.flush()
                    if not any(line.strip() == b"PATTERN_FINAL 640x480 WRGB" for line in pattern_output(pattern)):
                        raise ValueError("Windows did not paint the final WRGB picture")
                    # Let the previously animated pictures drain. Windows keeps the final layout static.
                    time.sleep(2)
                    wait_for_picture(port, initial, "WRGB")
                    final_result["static_WRGB"] = quadrant_counts(snapshot(port, "WRGB"))
                    before_hold = presented(port)
                    time.sleep(5)
                    # An idle driver may send no further frames, or resend the same desktop.
                    # Read the actual scanout again in either case.
                    before_read = presented(port)
                    final_result["held_WRGB"] = quadrant_counts(snapshot(port, "WRGB"))
                    after_hold = presented(port)
                    if not before_hold <= before_read <= after_hold:
                        raise ValueError("USB monitor presentation counter restarted during the hold")
                    final_result["hold_presentations"] = after_hold - before_hold
                    # The framebuffer was sampled before its rows were sent over UART.
                    # Windows must keep showing WRGB until this last observation finishes.
                    latest = decoded_picture(port)
                    final_result["picture_after_readback"] = latest
                    if latest.startswith(("decoded=absent", "decoded=all_black")):
                        raise ValueError(f"The final USB picture disappeared during LCD readback: {latest}")
                    if not latest.startswith("decoded=has_nonblack"):
                        # Older or busy firmware may have no usable picture diagnostic.
                        # A stable counter brackets a readback of the same presentation.
                        before_retry = presented(port)
                        if before_retry < after_hold:
                            raise ValueError("USB monitor presentation counter restarted during the hold")
                        if before_retry != before_read:
                            final_result["held_WRGB_retry"] = quadrant_counts(snapshot(port, "WRGB"))
                            after_retry = presented(port)
                            if after_retry != before_retry:
                                raise ValueError("Could not verify the last USB picture during repeated presentations")
                            final_result["hold_presentations"] = after_retry - before_hold
                    pattern.stdin.close()
                if pattern.wait(timeout=5 if final_frame else 35) != 0:
                    raise ValueError("Windows could not maintain the target throughout the test")
                return counts
            except (ValueError, OSError) as exc:
                if pattern.poll() == 3 and not isinstance(exc, DesktopUnavailable):
                    raise DesktopUnavailable() from exc
                raise
            finally:
                watchdog.cancel()
                if pattern.poll() is None:
                    pattern.kill()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--snapshot", type=Path, help="existing board PNG, for color analysis only")
    source.add_argument("--probe", type=Path, help="USB monitor Windows test executable")
    parser.add_argument("--serial", help="12-digit KVMpad USB serial for the live test")
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--result", type=Path, help="write the latest live trial status and counts as JSON")
    parser.add_argument("--final-frame", action="store_true", help="also verify a distinct final static picture on the board")
    args = parser.parse_args()
    if args.probe and not re.fullmatch("[a-fA-F0-9]{12}", args.serial or ""):
        parser.error("--probe requires the board's --serial")
    if (args.result or args.final_frame) and args.snapshot:
        parser.error("--result and --final-frame require a live --probe test")
    # Make sibling HIL modules available when Python runs in isolated mode.
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    if args.snapshot:
        from PIL import Image
        with Image.open(args.snapshot) as screen:
            print("Matching quadrant pixels in PNG NW/NE/SW/SE", verify(screen))
    else:
        result = {"trial_id": uuid.uuid4().hex, "started_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
                  "board_usb_serial": args.serial.upper(), "status": "RUNNING"}
        try:
            save_result(args.result, result)
            print("USB monitor trial", result["trial_id"], flush=True)
            probe = args.probe.resolve()
            result["windows_display_at_start"] = check_probe(probe, args.serial, args.final_frame)
            phases = result.setdefault("phases", {}) if args.final_frame else None
            counts = check_live(probe, args.serial, args.port, phases)
            result["status"] = "PASS"
            result["quadrants"] = quadrant_counts(counts)
            print("PASS Windows USB to board scanout RGBW", counts, flush=True)
            if phases:
                print("PASS final static USB picture WRGB, verified again after holding for five seconds.",
                      "Additional board presentations:", phases["hold_presentations"], flush=True)
        except (KeyboardInterrupt, OSError, ValueError, subprocess.TimeoutExpired) as exc:
            result["status"] = "INCONCLUSIVE" if isinstance(exc, DesktopUnavailable) else "FAIL"
            result["error"] = "Test interrupted" if isinstance(exc, KeyboardInterrupt) else " ".join(str(exc).split())[:384]
            print(result["status"], result["error"], file=sys.stderr, flush=True)
        result["finished_utc"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
        try:
            save_result(args.result, result)
        except OSError as exc:
            parser.exit(1, f"Could not save the USB monitor test result: {exc}\n")
        sys.exit(0 if result["status"] == "PASS" else 1)
