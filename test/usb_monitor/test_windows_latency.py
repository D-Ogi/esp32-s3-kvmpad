# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Check Windows paint timing bounds and their relation to a verified board frame."""
import io
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "hil"))
import usb_monitor_latency as trial
from usb_monitor_pattern import verify_uart


def clock(ticks):
    return Mock(frequency=1000, now=Mock(side_effect=ticks))


def picture(order, mode="640x480"):
    colors = {"R": 0xf800, "G": 0x07e0, "B": 0x001f, "W": 0xffff}
    lines = [b"SNAP_BEGIN div=16", b"SNAP_SOURCE scanout"]
    digest = 2166136261
    width, height = map(int, mode.split("x"))
    ox, oy = (192, 84) if width == 640 else (0, 0)
    for y in range(37):
        row = bytearray()
        for x in range(64):
            px, py = x * 16 - ox, y * 16 - oy
            value = colors[order[int(px >= width//2) + 2*int(py >= height//2)]] if 0 <= px < width and 0 <= py < height else 0
            row.extend(value.to_bytes(2, "big"))
            digest = ((digest * 16777619) & 0xffffffff) ^ value
        lines.append(f"SNAP_ROW {y} {row.hex().upper()}".encode())
    lines.append(f"SNAP_END w=64 h=37 crc={digest:08X} err=ESP_OK".encode())
    return lines


class LatencyTest(unittest.TestCase):
    def test_receiver_stages_bound_to_verified_frame(self):
        port = Mock()
        board = trial.Board(port, portrait_console=True)
        header = "USB_MONITOR_LATENCY unit=us samples=7 sequence=13 peak_total=8000\n"
        last = "USB_MONITOR_LAST receive=1 queue=2 decode=3 ready=4 refresh=5\n"
        serial = Mock()
        with patch.dict(sys.modules, kvm_serial=serial):
            serial.read_until.return_value = (header + last + last.replace("LAST", "MEAN"), True)
            with patch.object(board, "presented", side_effect=[9, 9]), patch.object(board, "verify"):
                result = trial.measure_step(board, clock([100, 105]), 8, 98, "WRGB", {"samples":6, "sequence":9})
            self.assertEqual(result["receiver"], {"sequence":13, "samples":7,
                "stages_us":{"receive":1, "queue":2, "decode":3, "ready":4, "refresh":5}})
            self.assertEqual((result["lower_ms"], result["upper_ms"]), (0,7))
            for previous in ({"samples":5, "sequence":9}, {"samples":6, "sequence":13}):
                with patch.object(board, "presented", return_value=9), patch.object(board, "verify"):
                    with self.assertRaisesRegex(ValueError, "did not advance"):
                        trial.measure_step(board, clock([100, 105]), 8, 98, "WRGB", previous)
            for response in [(header + last, False), (header + last.replace("decode=3", "decode=-1"), True),
                             (header.replace("samples=7", "samples=0") + last, True), (header, True)]:
                serial.read_until.return_value = response
                with self.assertRaisesRegex(ValueError, "Missing receiver timing"):
                    board.receiver_timing()

    def test_initial_wait_skips_snapshots_until_a_frame_arrives(self):
        board = Mock()
        board.presented.return_value = 0
        with patch.object(trial.time, "monotonic", side_effect=[0, 1, 9]), patch.object(trial.time, "sleep"):
            with self.assertRaisesRegex(ValueError, "first frame"):
                trial.confirm_initial(board)
        board.verify.assert_not_called()

    def test_portrait_trial_associates_receiver_sample_after_color_check(self):
        board = Mock()
        board.presented.side_effect = [2, 2, 2, 3, 3]
        before = {"samples":1, "sequence":3}
        after = {"samples":2, "sequence":5}
        board.receiver_timing.side_effect = [before, after]
        pattern = Mock(stdin=io.BytesIO(), stdout=io.BytesIO(
            b"PATTERN_READY 640x480 RGBW\nPATTERN_STEP 1 WRGB 1000 1000\n"))
        result = {"samples":[]}
        trial.run_steps(pattern, board, clock([999, 1001, 1002, 1010]), 1, result, receiver_stages=True)
        self.assertEqual(result["samples"], [{"presented":3, "order":"WRGB", "lower_ms":0,
                                              "upper_ms":10, "receiver":after}])
        self.assertEqual([item[0] for item in board.method_calls],
                         ["presented", "verify", "presented", "receiver_timing", "presented",
                          "presented", "verify", "receiver_timing", "presented"])

    def test_native_frame_reaches_the_panel_edges(self):
        for order in ("RGBW", "WRGB"):
            rows = picture(order, "1024x600")
            self.assertTrue(all(good == total for good, total in verify_uart(rows, order, 16, "1024x600")))
            with self.assertRaisesRegex(ValueError, "quadrant"):
                verify_uart(picture(order), order, 16, "1024x600")
        pattern = Mock(stdout=io.BytesIO(b"PATTERN_READY 640x480 RGBW\n"))
        with self.assertRaisesRegex(ValueError, "different resolution"):
            trial.run_steps(pattern, Mock(), Mock(), 1, {}, "1024x600")

    def test_native_desktop_must_match_signal(self):
        board_name = rb"\\.\DISPLAY8"
        requested = "1024x600"
        current = "640x480"
        signal = requested
        changes = []

        def invoke(cmd, **kwargs):
            nonlocal current
            if "--latency-pattern-version" in cmd:
                return Mock(returncode=0, stdout=b"KVM_USB_LATENCY_PATTERN 1\n")
            if "--native-latency-version" in cmd:
                return Mock(returncode=0, stdout=b"KVM_USB_NATIVE_LATENCY 1\n")
            if cmd[0] == "client":
                changes.append(cmd)
                current = requested
                return Mock(returncode=0)
            if "--composite" not in cmd:
                return Mock(returncode=1, stdout=b"")
            if "--modes" in cmd:
                row = f"board=USB source={board_name.decode()} current={current} hz=60 bpp=32 active_signal={signal}\n"
                return Mock(returncode=0, stdout=row.encode())
            self.assertEqual(cmd[-2:], ["--latency-target", requested])
            accepted = current == signal == requested
            target = b"board GDI source=" + board_name + b" origin=(640,0)\nLATENCY_TARGET 1024x600\n"
            return Mock(returncode=0 if accepted else 1, stdout=target if accepted else b"")

        with patch.object(trial.subprocess, "run", side_effect=invoke), patch.object(trial.time, "sleep"):
            with self.assertRaisesRegex(ValueError, "desktop 640x480, USB signal 1024x600"):
                trial.find_display("probe", "012345ABCDEF", mode=requested)
            signal = "unknown"
            with self.assertRaisesRegex(ValueError, "signal unknown"):
                trial.find_display("probe", "012345ABCDEF", mode=requested, match_client="client")
            self.assertEqual(changes, [])
            signal = requested
            prefix, name = trial.find_display("probe", "012345ABCDEF", 2, requested, "client")
            self.assertEqual(changes, [["client", "--usb-match-signal", "012345ABCDEF", requested]])
            self.assertEqual(prefix, ["probe", "--composite", "012345ABCDEF"])
            self.assertEqual(name, board_name.decode())

    def test_probe_that_omits_the_validated_display_is_identified(self):
        replies = [b"KVM_USB_LATENCY_PATTERN 1\n", b"KVM_USB_NATIVE_LATENCY 1\n",
                   b"LATENCY_TARGET 640x480\n"]
        with patch.object(trial.subprocess, "run", side_effect=[Mock(returncode=0, stdout=r) for r in replies]):
            with self.assertRaisesRegex(ValueError, "omitted the Windows display name"):
                trial.find_display("probe", "012345ABCDEF")

    def test_vga_probe_requires_known_matching_signal_and_same_windows_source(self):
        name = rb"\\.\DISPLAY8"
        for signal, same, should_pass in (("unknown", True, False), ("1024x600", True, False),
                                          ("640x480", False, False), ("640x480", True, True)):
            def invoke(cmd, **kwargs):
                if cmd[-1] == "--latency-pattern-version":
                    return Mock(returncode=0, stdout=b"KVM_USB_LATENCY_PATTERN 1\n")
                if "--composite" in cmd or cmd[-1] == "--native-latency-version":
                    return Mock(returncode=1, stdout=b"")
                if cmd[-1] == "--modes":
                    src = name if same else rb"\\.\DISPLAY9"
                    return Mock(returncode=0, stdout=b"source=" + src + b" current=640x480 hz=60 active_signal=" + signal.encode())
                return Mock(returncode=0, stdout=b"board GDI source=" + name + b" origin=(640,0)\n")
            with self.subTest(signal=signal, same=same), patch.object(trial.subprocess, "run", side_effect=invoke):
                if should_pass:
                    self.assertEqual(trial.find_display("probe", "012345ABCDEF")[1], name.decode())
                else:
                    with self.assertRaises(ValueError):
                        trial.find_display("probe", "012345ABCDEF")

    def test_opened_monitor_returns_to_extensions_even_if_desktop_refuses(self):
        digest = "ab" * 32
        serial = Mock()
        serial.run_cmd.side_effect = [(text, True) for text in (
            "cert_sha256=" + digest, "USB_MONITOR mounted=0 suspended=0",
            "USB_MONITOR_OPEN size=640x480 started=1", "USB_MONITOR mounted=1",
            "shell -> ESP_OK", "UI_SHELL active=no", "UI_SHELL active=yes", "UI_CARD browser=ESP_OK")]
        with patch.dict(sys.modules, kvm_serial=serial):
            with self.assertRaisesRegex(ValueError, "desktop"):
                with trial.monitor_session(None, digest) as opened:
                    self.assertTrue(opened)
                    raise ValueError("desktop")
        self.assertEqual([call.args[1] for call in serial.run_cmd.call_args_list][-4:],
                         ["ui shell", "ui shell status", "ui shell status", "ui open card"])

    def test_existing_display_and_wrong_board(self):
        digest = "ab" * 32
        serial = Mock()
        with patch.dict(sys.modules, kvm_serial=serial):
            for state in ("mounted=1 suspended=0", "mounted=0 suspended=1"):
                serial.run_cmd.side_effect = [("cert_sha256=" + digest, True), ("USB_MONITOR " + state, True)]
                with trial.monitor_session(None, digest) as opened:
                    self.assertFalse(opened)
            serial.run_cmd.side_effect = [("cert_sha256=" + "cd" * 32, True)]
            with self.assertRaisesRegex(ValueError, "remote status"):
                with trial.monitor_session(None, digest):
                    self.fail("wrong board")
        self.assertEqual(serial.run_cmd.call_count, 5)

    def test_qpc_interval_and_scanout_time_excluded(self):
        b = Mock()
        b.presented.side_effect = [5, 5, 5, 5, 6, 6, 6, 7, 7]
        p = Mock(stdout=io.BytesIO(b"PATTERN_READY 640x480 RGBW\nPATTERN_STEP 1 WRGB 1000 1000\n"
                                   b"PATTERN_STEP 2 RGBW 5000 1000\n"), stdin=io.BytesIO())
        times = clock([900, 1010, 1020, 1030, 1080, 1100, 1110, 1150,
                       4900, 5010, 5020, 5040, 5100, 5150])
        result = {"samples": []}
        trial.run_steps(p, b, times, 2, result)
        self.assertEqual(p.stdin.getvalue(), b"NEXT\nNEXT\n")
        self.assertEqual([(r["lower_ms"], r["upper_ms"]) for r in result["samples"]], [(80, 150), (20, 150)])
        self.assertEqual([c.args for c in b.verify.call_args_list], [("RGBW",), ("WRGB",), ("RGBW",)])

    def test_ambiguous_counter_is_rejected(self):
        for counts in ([9], [5], [8, 9]):
            board = Mock()
            board.presented.side_effect = counts
            with self.subTest(counts=counts), self.assertRaises(ValueError):
                trial.measure_step(board, clock([105, 107]), 7, 100, "WRGB")

    def test_fast_first_reply_still_verifies_the_pattern(self):
        board = Mock()
        board.presented.side_effect = [8, 8]
        self.assertEqual(trial.measure_step(board, clock([108, 117]), 7, 100, "WRGB")["lower_ms"], 0)
        board.verify.side_effect = ValueError("quadrant mismatch")
        board.presented.side_effect = [8]
        with self.assertRaisesRegex(ValueError, "quadrant"):
            trial.measure_step(board, clock([108, 117]), 7, 100, "WRGB")

    def test_rejects_wrong_clock_step_and_order(self):
        for fields in ("1 WRGB 400 1000", "1 WRGB 100 2000", "1 RGBW 100 1000", "2 WRGB 100 1000"):
            board = Mock()
            board.presented.side_effect = [4, 4]
            stream = f"PATTERN_READY 640x480 RGBW\nPATTERN_STEP {fields}\n".encode()
            with self.subTest(fields=fields), self.assertRaises(ValueError):
                trial.run_steps(Mock(stdin=io.BytesIO(), stdout=io.BytesIO(stream)), board, clock([90, 120]), 1,
                                {"samples": []})

    def test_real_snapshot_shape_and_crc(self):
        for order in ("RGBW", "WRGB"):
            rows = picture(order)
            self.assertTrue(all(good == total and total > 0 for good, total in verify_uart(rows, order, div=16)))
            with self.assertRaisesRegex(ValueError, "quadrant"):
                verify_uart(rows, "WRGB" if order == "RGBW" else "RGBW", div=16)
            with self.assertRaises(ValueError):
                verify_uart(rows[:-1], order, div=16)
            rows[1] = b"SNAP_SOURCE lvgl"
            with self.assertRaisesRegex(ValueError, "submitted"):
                verify_uart(rows, order, div=16)


if __name__ == "__main__":
    unittest.main()
