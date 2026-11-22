# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
import json
import tempfile
import unittest
from pathlib import Path

from verify_motion_scanouts import verify


def sample(frame, change=None):
    pixels = [[0x2345] * 64 for _ in range(37)]
    for y in range(6, 36):
        for x in range(12, 52):
            sx, sy = 16*x - 192, 16*y - 84
            if frame is None:
                white = (sx//13 + sy//11) % 2
            elif y < 10 and x < 48:
                white = (frame >> ((x - 12)//6)) & 1
            else:
                white = frame & 1
            pixels[y][x] = 0xffff * white
    if change:
        x, y = change
        pixels[y][x] ^= 1
    crc = 2166136261
    lines = ["SNAP_BEGIN div=16", "I (100) ui_platform: SNAP_SOURCE scanout"]
    for y, row in enumerate(pixels):
        for word in row:
            crc = ((crc * 16777619) & 0xffffffff) ^ word
        lines.append(f"SNAP_ROW {y} " + "".join(f"{word:04X}" for word in row))
    lines.append(f"SNAP_END w=64 h=37 crc={crc:08X} err=ESP_OK")
    return "\n".join(lines).encode(), f"{crc:08X}"


def make_run(work, frames, change=None):
    guest = ["GUD_MOTION_READY", "START", *(f"GUD_MOTION_FRAME {i}" for i in range(1, 21)),
             "GUD_MOTION_HELD", "ACK motion", "GUD_DRAW_COMPLETE"]
    (work / "guest.log").write_text("\n".join(guest))
    events = [json.dumps(dict(id=i, host_ns=500000000*i)) for i in range(1, 21)]
    (work / "motion-frames.jsonl").write_text("\n".join(events))
    records = []
    for i, frame in enumerate(frames):
        blob, crc = sample(frame, change if frame == 7 else None)
        (work / f"scan-{i:03d}.log").write_bytes(blob)
        records.append(dict(begin_ns=i*5+1, end_ns=i*5+3, frame=frame, crc=crc))
    (work / "scans.json").write_text(json.dumps(records))


class MotionScanouts(unittest.TestCase):
    def test_verified_checkerboards_and_repeated_ids(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            make_run(work, [None, None, 1, *range(1, 21)])
            (work / "baseline.log").write_bytes(sample(None)[0])
            result = verify(work)
            self.assertTrue(result["prestart_baseline_raw"])
            self.assertEqual((result["initial_checkerboards"], result["numbered_scans"]), (2, 21))

    def test_missed_frame_with_consistent_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            make_run(work, [None, *range(1, 9), *range(10, 21)])
            with self.assertRaisesRegex(ValueError, "Frame 10 after 8"):
                verify(work)

    def test_one_changed_sample_with_recomputed_crc_and_index(self):
        for point, message in [((31, 20), "source pixel"), ((0, 0), "outside the viewport")]:
            with self.subTest(point=point), tempfile.TemporaryDirectory() as tmp:
                work = Path(tmp)
                make_run(work, [None, *range(1, 21)], change=point)
                with self.assertRaisesRegex(ValueError, message):
                    verify(work)

    def test_unknown_or_late_checkerboard_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            make_run(work, [None, 1, None, *range(2, 21)])
            with self.assertRaisesRegex(ValueError, "Checkerboard returned"):
                verify(work)
            blob, _ = sample(None, (26, 18))
            (work / "scan-000.log").write_bytes(blob)
            with self.assertRaisesRegex(ValueError, "scan-000.log: (Unrecognized frame ID|Frame .* differs)"):
                verify(work)


if __name__ == "__main__":
    unittest.main()
