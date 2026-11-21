# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Verify the moving, final and held panel snapshots without a display or UART."""
from contextlib import nullcontext
from functools import lru_cache
import io
from itertools import groupby
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "hil"))
import usb_monitor_pattern as trial
from ui_snapshot import decode_row


@lru_cache
def scanout(order, compact=False):
    values = {"R": 0xf800, "G": 0x07e0, "B": 0x001f, "W": 0xffff, "K": 0}
    digest = 2166136261
    lines = [b"SNAP_BEGIN div=4", b"SNAP_SOURCE scanout"]
    for y in range(150):
        row = bytearray()
        for x in range(256):
            color = 0
            if 48 <= x < 208 and 21 <= y < 141:
                color = values[order[int(x >= 128) + 2 * int(y >= 81)]]
            digest = (digest * 16777619 & 0xffffffff) ^ color
            row.extend(color.to_bytes(2, "big"))
        if compact and y % 2 == 0:
            words = (bytes(row[i:i + 2]).hex().upper() for i in range(0, len(row), 2))
            runs = [(color, sum(1 for _ in group)) for color, group in groupby(words)]
            data = "".join(f"*{count:04X}{color}" if count >= 3 else color * count for color, count in runs)
            lines.append(f"SNAP_RLE {y} {data}".encode())
        else:
            lines.append(f"SNAP_ROW {y} {row.hex().upper()}".encode())
    lines.append(f"SNAP_END w=256 h=150 crc={digest:08X} err=ESP_OK".encode())
    return b"\r\n".join(lines) + b"\r\n"


@lru_cache
def portrait_scanout(mode, order, div, *, reversed_rotation=False, other_pane=False):
    # Draw directly in the physical LCD's coordinates. After the 7B rotates the
    # logical portrait image, its NE, SE, NW, SW quadrants occupy the physical
    # NW, NE, SW, SE respectively. Both package pages use the same Fit viewport.
    left, top, w, h = {"640x480": (41, 9, 436, 582), "1024x600": (88, 8, 342, 584)}[mode]
    if other_pane:
        left += 512
    physical_order = (2, 0, 3, 1) if reversed_rotation else (1, 3, 0, 2)
    colors = {"R": 0xf800, "G": 0x07e0, "B": 0x001f, "W": 0xffff, "K": 0}
    width, height = 1024 // div, 600 // div
    rows = [f"SNAP_BEGIN div={div}".encode(), b"I (123) ui_platform: SNAP_SOURCE scanout"]
    digest = 2166136261
    for row in range(height):
        data = bytearray()
        y = row * div - top
        for col in range(width):
            x = col * div - left
            value = 0x8410  # neutral shell around the synthetic image
            if 0 <= x < w and 0 <= y < h:
                quadrant = int(x >= w // 2) + 2 * int(y >= h // 2)
                value = colors[order[physical_order[quadrant]]]
            data.extend(value.to_bytes(2, "big"))
            digest = ((digest * 16777619) & 0xffffffff) ^ value
        rows.append(f"SNAP_ROW {row} {data.hex().upper()}".encode())
    rows.append(f"SNAP_END w={width} h={height} crc={digest:08X} err=ESP_OK".encode())
    return tuple(rows)


class PortraitConsoleTest(unittest.TestCase):
    def test_color_phases_in_physical_fit_viewport(self):
        for mode in ("640x480", "1024x600"):
            for div in (4, 16):
                for order in ("RGBW", "WRGB"):
                    with self.subTest(mode=mode, div=div, order=order):
                        rows = portrait_scanout(mode, order, div)
                        counts = trial.verify_uart(rows, order, div, viewport=mode, portrait_console=True)
                        self.assertEqual(len(counts), 4)
                        self.assertTrue(all(good == total and total > 0 for good, total in counts))

    def test_invalid_portrait_frames(self):
        for mode in ("640x480", "1024x600"):
            for div in (4, 16):
                cases = {
                    "previous RGBW instead of final WRGB": portrait_scanout(mode, "RGBW", div),
                    "one incorrect quadrant": portrait_scanout(mode, "WRGR", div),
                    "reverse rotation": portrait_scanout(mode, "WRGB", div, reversed_rotation=True),
                    "black viewport": portrait_scanout(mode, "KKKK", div),
                    "pattern outside monitor viewport": portrait_scanout(mode, "WRGB", div, other_pane=True),
                }
                for name, rows in cases.items():
                    with self.subTest(mode=mode, div=div, name=name), self.assertRaisesRegex(ValueError, "quadrant"):
                        trial.verify_uart(rows, "WRGB", div, viewport=mode, portrait_console=True)

    def test_standalone_and_console_are_distinct(self):
        rows = scanout("WRGB").splitlines()
        with self.assertRaisesRegex(ValueError, "quadrant"):
            trial.verify_uart(rows, "WRGB", viewport="640x480", portrait_console=True)
        with self.assertRaisesRegex(ValueError, "quadrant"):
            trial.verify_uart(portrait_scanout("640x480", "WRGB", 4), "WRGB")

    def test_portrait_requires_a_complete_physical_scanout(self):
        rows = portrait_scanout("1024x600", "WRGB", 16)
        for name, invalid_rows in (
                ("LVGL snapshot", (rows[0], b"SNAP_SOURCE lvgl", *rows[2:])),
                ("missing confirmation", (rows[0], *rows[2:])),
                ("truncated", rows[:-1])):
            with self.subTest(name=name), self.assertRaises(ValueError):
                trial.verify_uart(invalid_rows, "WRGB", 16, viewport="1024x600", portrait_console=True)


class FinalFrameTest(unittest.TestCase):
    def observe(self, final, *, stalled=False, held=None, resend=False, held_link=None, probe_exit=0,
                late_picture="decoded=has_nonblack", repeated=False, retry_picture=None, desktop=True, compact=False):
        phases = {}
        process = Mock()
        process.stdout = io.BytesIO(b"PATTERN_READY 640x480 RGBW\nPATTERN_FINAL 640x480 WRGB\n")
        if not desktop:
            process.stdout = io.BytesIO(b"PATTERN_UNAVAILABLE input_desktop\n")
        process.stdin = io.BytesIO()
        process.poll.return_value = probe_exit
        process.wait.return_value = probe_exit
        port = Mock()
        clock = 0.0
        final_reads = 0

        def command(_, text, timeout):
            if text == "ui usb-monitor-status picture":
                if final_reads < 2:
                    return "USB_MONITOR_PICTURE decoded=has_nonblack", True
                if late_picture is None:
                    return "usage: ui usb-monitor-status", True
                return f"USB_MONITOR_PICTURE {late_picture}", True
            self.assertEqual(text, "ui usb-monitor-status")
            # The source sends one image before READY. It can then go completely quiet.
            counter = int(process.stdout.tell() > 0)
            if process.stdin.tell() and not stalled:
                counter += 1
                if resend and final_reads >= 2:
                    counter += final_reads - 1 if repeated else 1
            if held_link is not None and final_reads >= 1:
                return f"USB_MONITOR mounted=0 presented={counter} suspended={int(held_link == 'suspended')}", True
            return f"USB_MONITOR mounted=1 presented={counter}", True

        def request(data):
            nonlocal final_reads
            self.assertEqual(data, b"ui snap 4 compact\r\n")
            if process.stdin.tell():
                self.assertEqual(process.stdin.getvalue(), b"FINAL\n")
                self.assertGreaterEqual(clock, 2)
                if final_reads:
                    self.assertGreaterEqual(clock, 7)
                order = held if final_reads and held is not None else final
                if final_reads >= 2 and retry_picture is not None:
                    order = retry_picture
                final_reads += 1
            else:
                order = "RGBW"
            port.read.side_effect = io.BytesIO(scanout(order, compact)).read

        def sleep(delay):
            nonlocal clock
            clock += delay

        port.write.side_effect = request
        serial = Mock()
        serial.open_port.return_value = nullcontext(port)
        serial.run_cmd.side_effect = command
        self.phases = phases
        with patch.dict(sys.modules, {"kvm_serial": serial}), \
             patch.object(trial.subprocess, "Popen", return_value=nullcontext(process)), \
             patch.object(trial.time, "monotonic", side_effect=lambda: clock), \
             patch.object(trial.time, "sleep", side_effect=sleep):
            trial.check_live(Path("synthetic-probe"), "000000000001", "fixture", phases)
        self.assertTrue(process.stdin.closed)

    def test_distinct_static_picture(self):
        self.observe("WRGB")
        for key in ("initial_RGBW", "static_WRGB", "held_WRGB"):
            self.assertEqual(self.phases[key], dict.fromkeys(("NW", "NE", "SW", "SE"),
                                                            {"matched": 1728, "total": 1728}))
        self.assertEqual(self.phases["hold_presentations"], 0)

    def test_driver_can_repeat_static_picture(self):
        self.observe("WRGB", resend=True)
        self.assertEqual(self.phases["hold_presentations"], 1)

    def test_compact_scanout_and_held_picture(self):
        self.observe("WRGB", compact=True)
        self.assertEqual(self.phases["static_WRGB"], self.phases["held_WRGB"])
        self.assertEqual(decode_row(b"SNAP_RLE 0 ABCD*00031234FFFF", 5),
                         (0, bytes.fromhex("ABCD123412341234FFFF")))
        for payload in (b"*00000000", b"*FFFF0000", b"*0003", b"*0003000G", b"*00030000FFFF"):
            with self.subTest(payload=payload), self.assertRaises(ValueError):
                decode_row(b"SNAP_RLE 0 " + payload, 3)

    def test_final_picture_disappears_while_sending_held_snapshot(self):
        for latest in ("decoded=all_black reference=all_black sequence=116", "decoded=absent"):
            with self.subTest(latest=latest), self.assertRaisesRegex(ValueError, "disappeared during LCD readback"):
                self.observe("WRGB", resend=True, late_picture=latest)
            self.assertIn("held_WRGB", self.phases)
            self.assertEqual(self.phases["picture_after_readback"], latest)

    def test_legacy_and_busy_firmware(self):
        for latest in (None, "error=ui_busy"):
            with self.subTest(latest=latest):
                self.observe("WRGB", late_picture=latest)
                self.assertNotIn("held_WRGB_retry", self.phases)
                self.observe("WRGB", resend=True, late_picture=latest)
                self.assertEqual(self.phases["held_WRGB_retry"], self.phases["held_WRGB"])
                with self.assertRaisesRegex(ValueError, "quadrant mismatch"):
                    self.observe("WRGB", resend=True, late_picture=latest, retry_picture="KKKK")
                with self.assertRaisesRegex(ValueError, "repeated presentations"):
                    self.observe("WRGB", resend=True, late_picture=latest, repeated=True)

    def test_isolated_desktop(self):
        with self.assertRaisesRegex(ValueError, "active Windows desktop"):
            self.observe("WRGB", desktop=False)
        self.assertEqual(self.phases, {})

    def test_static_picture_can_survive_suspend(self):
        self.observe("WRGB", held_link="suspended")
        self.assertEqual(self.phases["hold_presentations"], 0)

    def test_disconnect_after_final_picture(self):
        with self.assertRaisesRegex(ValueError, "Open USB monitor"):
            self.observe("WRGB", held_link="detached")
        self.assertIn("static_WRGB", self.phases)

    def test_final_picture_lost_during_hold(self):
        for held in ("RGBW", "KKKK"):
            with self.subTest(held=held), self.assertRaisesRegex(ValueError, "WRGB quadrant mismatch"):
                self.observe("WRGB", held=held)
            self.assertEqual(list(self.phases), ["initial_RGBW", "static_WRGB"])

    def test_frozen_or_black_desktop(self):
        for content in ("RGBW", "KKKK"):
            with self.subTest(content=content), self.assertRaisesRegex(ValueError, "WRGB quadrant mismatch"):
                self.observe(content)
            self.assertEqual(list(self.phases), ["initial_RGBW"])

    def test_no_new_frame(self):
        with self.assertRaisesRegex(ValueError, "No new USB picture.*WRGB"):
            self.observe("WRGB", stalled=True)

    def test_probe_lost_target(self):
        with self.assertRaisesRegex(ValueError, "maintain the target"):
            self.observe("WRGB", probe_exit=1)


if __name__ == "__main__":
    unittest.main()
