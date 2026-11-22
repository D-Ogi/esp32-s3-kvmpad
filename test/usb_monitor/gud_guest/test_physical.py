# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
import ctypes as c
import json
import os
import socket
import tempfile
import unittest
from contextlib import contextmanager
from pathlib import Path
from unittest.mock import Mock, patch

from ack_scanout import acknowledge, measure, measure_console, measure_modeset
from host_identity import check_target
from physical import MotionRun, parse_bitmap, publish, run, usb_filter, validate_ack


def capture(edge=False):
    lines = [b"SNAP_BEGIN div=4", b"I (1013) UI: SNAP_SOURCE scanout"]
    crc = 2166136261
    for y in range(150):
        row = []
        for x in range(256):
            color = 0x1234
            if 48 <= x < 208 and 21 <= y < 141:
                white = (((x - 48) * 4 // 13 + (y - 21) * 4 // 11) % 2) == 1
                if edge and x >= 203 and y >= 139:
                    white = not white
                color = 0xffff if white else 0
            crc = (crc * 16777619 & 0xffffffff) ^ color
            row.append(f"{color:04X}")
        lines.append(f"SNAP_ROW {y} {''.join(row)}".encode())
    lines.append(f"SNAP_END w=256 h=150 crc={crc:08X} err=ESP_OK".encode())
    return b"\n".join(lines)


class DescriptorLibrary:
    """Two simulated host devices. Only the selected descriptor may be opened."""
    def __init__(self, serial):
        ptr = c.c_void_p
        self.devices = (ptr * 2)(11, 42)
        self.opened = []
        self.closed = []
        self.freed = False
        self.exited = False
        raw = bytes.fromhex("120100020000004034127856000101020301")

        def list_devices(ctx, out):
            c.cast(out, c.POINTER(c.POINTER(ptr)))[0] = c.cast(self.devices, c.POINTER(ptr))
            return 2

        def descriptor(dev, buf):
            c.memmove(buf, raw, 18)
            return 0

        def string(handle, idx, buf, cap):
            value = {2: b"KVMpad GUD", 3: serial}[idx]
            c.memmove(buf, value, len(value))
            return len(value)

        self.libusb_init = lambda ctx: 0
        self.libusb_exit = lambda ctx: setattr(self, "exited", True)
        self.libusb_get_device_list = list_devices
        self.libusb_free_device_list = lambda devices, unref: setattr(self, "freed", True)
        self.libusb_get_bus_number = lambda dev: 2
        self.libusb_get_device_address = lambda dev: 44 if dev == 42 else 1
        self.libusb_get_device_descriptor = descriptor
        self.libusb_open = lambda dev, handle: self.opened.append(dev) or 0
        self.libusb_close = lambda handle: self.closed.append(True)
        self.libusb_get_string_descriptor_ascii = string


class PhysicalFixture(unittest.TestCase):
    def test_modeset_scanout_edges_and_return(self):
        for name, left, top, width, height in (("vga", 192, 84, 640, 480),
                                                ("svga", 112, 0, 800, 600),
                                                ("return", 192, 84, 640, 480)):
            with self.subTest(name=name):
                # Generate bounds independently, preserving a different color outside the viewport.
                rows = [[0x1234] * 1024 for _ in range(600)]
                for y in range(height):
                    for x in range(width):
                        white = ((x // 13) ^ (y // 11)) & 1
                        rows[y + top][x + left] = 0xffff * (white ^ (name == "return"))
                phase = name + "-full"
                with patch("ack_scanout.read_scanout", return_value=(rows, "12345678")):
                    self.assertEqual(measure_modeset(b"", phase)["mismatch"], 0)
                    self.assertEqual(measure_modeset(b"", name + "-edge")["mismatch"], 189)
                    if name == "return":
                        self.assertEqual(measure_modeset(b"", "vga-full")["mismatch"], width * height)
                    for y in range(top + height - 9, top + height):
                        for x in range(left + width - 21, left + width):
                            rows[y][x] ^= 0xffff
                    phase = name + "-edge"
                    self.assertEqual(measure_modeset(b"", phase)["mismatch"], 0)
                    rows[top + height - 1][left + width - 1] ^= 1
                    metrics = measure_modeset(b"", phase)
                    self.assertEqual((metrics["examined"], metrics["mismatch"]), (width * height, 1))
                    with self.assertRaises(ValueError):
                        validate_ack(dict(phase=phase), dict(phase=phase, **metrics))

    def test_console_input_reports_gate_each_ready_file(self):
        serial = "A1B2C3D4E5F6"
        messages = [
            f"GUD_TARGET 1234:5678 {serial}", "GUD_BIND upstream",
            "GUD_VT_BITMAP tty1 1 1 2 800010", "GUD_INPUT_READY", "GUD_LCD_READY tty1",
            "GUD_INPUT_PASS", "GUD_VT_BITMAP tty2 1 1 2 800004", "GUD_LCD_READY tty2",
            "GUD_CONSOLE_COMPLETE",
        ]

        def acknowledge_synthetic_ready(path, record):
            publish(path, record)
            metrics = dict(divisor=1, examined=640 * 480, mismatch=0, lit=record["font_lit"])
            publish(path.with_name(f"ack-{record['phase']}.json"), dict(record, **metrics))

        for omitted, gates in [(None, ["grabbed", "passed"]), ("GUD_INPUT_READY", []),
                               ("GUD_INPUT_PASS", ["grabbed"])]:
            with self.subTest(omitted=omitted), tempfile.TemporaryDirectory() as folder:
                work = Path(folder)
                with self.fake_serial(messages, omitted) as listener:
                    with patch("physical.publish", side_effect=acknowledge_synthetic_ready), patch("physical.subprocess.Popen") as popen:
                        process = popen.return_value.__enter__.return_value
                        process.poll.return_value = process.wait.return_value = 0
                        args = ([], work, "loopback", "1234:5678", serial, 2, listener, True)
                        if omitted:
                            with self.assertRaisesRegex(ValueError, "Unconfirmed evdev gate"):
                                run(*args, console_test=True, input_events=True)
                        else:
                            run(*args, console_test=True, input_events=True)
                actual = [json.loads(path.read_text("utf-8"))["input_gate"]
                          for path in sorted(work.glob("ready-*.json"))]
                self.assertEqual(actual, gates)

    @staticmethod
    @contextmanager
    def fake_serial(messages, omitted):
        client, guest = socket.socketpair()
        with client, guest:
            stream = "".join(line + "\n" for line in messages if line != omitted)
            guest.sendall(stream.encode("ascii"))
            guest.shutdown(socket.SHUT_WR)
            listener = Mock()
            listener.accept.return_value = (client, None)
            yield listener

    def test_console_scanout_matches_font_and_black_viewport(self):
        # Synthetic font with one lit pixel at each end of the first message.
        raster = "800010"
        ready = dict(phase="tty1", **parse_bitmap(f"GUD_VT_BITMAP tty1 1 1 2 {raster}", "tty1"))
        for changed in (False, True):
            lines = [b"SNAP_BEGIN div=1", b"SNAP_SOURCE scanout"]
            crc = 2166136261
            for y in range(600):
                row = [0] * 1024
                if y == 84:
                    row[192] = row[211] = 65535
                if changed and y == 563:
                    row[831] = 0x10     # Corrupt the last viewport pixel, keep lit count unchanged.
                for p in row:
                    crc = ((crc * 16777619) & 0xffffffff) ^ p
                if any(row):
                    lines.append(f"SNAP_ROW {y} {''.join(f'{p:04X}' for p in row)}".encode())
                else:
                    lines.append(f"SNAP_RLE {y} *04000000".encode())
            lines.append(f"SNAP_END w=1024 h=600 crc={crc:08X} err=ESP_OK".encode())
            metrics = measure_console(b"\n".join(lines), ready)
            self.assertEqual((metrics["lit"], metrics["examined"], metrics["mismatch"]), (2, 307200, int(changed)))
            if changed:
                with self.assertRaises(ValueError):
                    validate_ack(ready, dict(ready, **metrics))
            else:
                validate_ack(ready, dict(ready, **metrics))
        for line in ("GUD_VT_BITMAP tty1 1 1 3 800010", "GUD_VT_BITMAP tty1 1 1 2 800001",
                     "GUD_VT_BITMAP tty2 1 1 2 800010"):
            with self.assertRaises(ValueError):
                parse_bitmap(line, "tty1")

    def test_premature_motion_control(self):
        with tempfile.TemporaryDirectory() as folder:
            work = Path(folder)
            motion = MotionRun(work, "fresh", "loopback", "ffff:ffff", "A1B2C3D4E5F6")
            for phase, signal in (("ready", "start"), ("held", "ack")):
                (work / f"{signal}-motion.json").write_text(json.dumps(dict(motion.identity, phase=phase)))
                with self.assertRaisesRegex(ValueError, "Premature"):
                    motion.gate(phase, signal, None, 1)
                self.assertFalse((work / f"{phase}-motion.json").exists())

    def test_target_preflight(self):
        for vidpid, serial, valid, opens in [
            ("1234:5678", b"A1B2C3D4E5F6", True, [42]),
            ("1111:2222", b"A1B2C3D4E5F6", False, []),
            ("1234:5678", b"A1B2C3D4E5F7", False, [42]),
        ]:
            with self.subTest(vidpid=vidpid, serial=serial):
                lib = DescriptorLibrary(serial)
                with patch("host_identity.c.CDLL", return_value=lib), patch("ctypes.util.find_library", return_value="mock"):
                    if valid:
                        check_target("qemu", 2, 44, vidpid, "A1B2C3D4E5F6")
                    else:
                        with self.assertRaises(ValueError):
                            check_target("qemu", 2, 44, vidpid, "A1B2C3D4E5F6")
                self.assertEqual(lib.opened, opens)
                self.assertEqual(len(lib.closed), len(opens))
                self.assertTrue(lib.freed and lib.exited)

    def test_filter(self):
        for identity in ["ffff:ffff", "303a:2987", "303a:2986", "303a:400a", "303a:400c", "0000:1234"]:
            with self.assertRaises(ValueError):
                usb_filter(1, 4, identity)
        for addr in [0, 128]:
            with self.assertRaises(ValueError):
                usb_filter(2, addr, "1234:5678")

    def test_samples_and_edge(self):
        for phase in ["full", "edge"]:
            snapshot = capture(phase == "edge")
            ready = dict(phase=phase, run="fixture", source="scanout", vidpid="1234:5678", serial="A1B2C3D4E5F6")
            validate_ack(ready, dict(ready, **measure(snapshot, phase)))
            self.assertEqual(measure(snapshot, "full" if phase == "edge" else "edge")["mismatch"], 10)
            for bad in [snapshot.replace(b"scanout", b"lvgl"), snapshot + b"\n" + snapshot,
                        snapshot.replace(b"1234", b"1235", 1)]:
                with self.assertRaises(ValueError):
                    measure(bad, phase)

    def test_scanout_ack_files(self):
        with tempfile.TemporaryDirectory() as folder:
            work = Path(folder)
            data = capture(True)
            ready = dict(phase="edge", run="fresh", source="scanout", vidpid="1234:5678", serial="A1B2C3D4E5F6")
            (work / "ready-edge.json").write_text(json.dumps(ready), encoding="utf-8")
            image = work / "edge.log"
            image.write_bytes(data)
            os.utime(image, (10, 10))
            with self.assertRaisesRegex(ValueError, "predates"):
                acknowledge(work, "edge", image)
            self.assertFalse((work / "ack-edge.json").exists())
            image.write_bytes(data)
            ack = acknowledge(work, "edge", image)
            self.assertEqual(json.loads((work / "ack-edge.json").read_text("utf-8")), ack)
            self.assertEqual((ack["examined"], ack["mismatch"], ack["inverted_samples"]), (19200, 0, 10))


if __name__ == "__main__":
    unittest.main()
