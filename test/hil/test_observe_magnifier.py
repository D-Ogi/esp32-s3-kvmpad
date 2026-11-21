# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
import io
import json
import unittest
from contextlib import redirect_stdout
from unittest.mock import patch

import observe_magnifier as observer


class Clock:
    def __init__(self):
        self.now = 0.0

    def monotonic(self):
        return self.now

    def sleep(self, interval):
        assert interval > 0
        self.now += interval


def status(frame, link=2):
    return (f"ui host-status\r\nUI_HOST attached=1 link={link} sharing=1 live_panel=1 "
            f"lcd_frames={frame} fps_x10=20 rx_to_lcd_ms=229 capture=streaming\r\nkvmpad>", True)


class MagnifierObservationTests(unittest.TestCase):
    def sample(self, reply, seconds, **options):
        clock, port, output = Clock(), object(), io.StringIO()

        def run_cmd(actual_port, command, timeout):
            self.assertIs(actual_port, port)
            self.assertEqual(command, "ui host-status")
            return reply(int(clock.now))

        with patch.object(observer, "time", clock), patch.object(observer.kvm_serial, "run_cmd", run_cmd), \
                redirect_stdout(io.StringIO()):
            passed = observer.observe(port, output, seconds, **options)
        return passed, [json.loads(line) for line in output.getvalue().splitlines()]

    def test_connected_release_status_counts_presented_frames(self):
        passed, rows = self.sample(lambda t: status(90 + 2*t), seconds=8)
        self.assertTrue(passed)
        self.assertEqual(rows[0]["link"], 2)
        self.assertEqual(rows[-1]["capture"], "streaming")
        self.assertEqual((rows[-1]["fps_x10"], rows[-1]["rx_to_lcd_ms"]), (20, 229))
        self.assertGreaterEqual(rows[-1]["lcd_frames"] - rows[0]["lcd_frames"], 10)

    def test_connecting_does_not_qualify_even_if_counters_advance(self):
        passed, rows = self.sample(lambda t: status(90 + 2*t, link=1), seconds=10)
        self.assertFalse(passed)
        self.assertTrue(rows)
        self.assertTrue(all(row["link"] == 1 for row in rows))

    def test_requested_duration_extends_the_old_five_second_window(self):
        passed, rows = self.sample(lambda t: status(90 + 2*t), seconds=15, duration=11)
        self.assertTrue(passed)
        self.assertGreaterEqual(rows[-1]["elapsed_s"] - rows[0]["elapsed_s"], 11)
        self.assertGreaterEqual(rows[-1]["lcd_frames"] - rows[0]["lcd_frames"], 22)

    def test_positive_reported_rate_cannot_hide_a_stalled_picture_counter(self):
        def reply(t):
            return status(100 + 5 * min(t, 3) + 5 * max(t - 9, 0))

        passed, _ = self.sample(reply, seconds=13, duration=8)
        self.assertFalse(passed)
        passed, _ = self.sample(reply, seconds=25, duration=8)
        self.assertTrue(passed)

    def test_gap_or_counter_rollback_restarts_continuous_window(self):
        for fault in ("no_response", "counter_rollback"):
            def reply(t):
                if t == 4 and fault == "no_response":
                    return "", False
                return status(2*(t - 4) if t >= 4 and fault == "counter_rollback" else 100 + 2*t)

            with self.subTest(fault=fault):
                passed, rows = self.sample(reply, seconds=11, duration=8)
                self.assertFalse(passed)
                if fault == "no_response":
                    self.assertTrue(any(row["capture"] == "no_response" for row in rows))
                else:
                    self.assertTrue(any(row.get("lcd_frames") == 0 for row in rows))
                passed, rows = self.sample(reply, seconds=16, duration=8)
                self.assertTrue(passed)
                restarted = 5 if fault == "no_response" else 4
                self.assertGreaterEqual(rows[-1]["elapsed_s"], restarted + 8)


if __name__ == "__main__":
    unittest.main()
