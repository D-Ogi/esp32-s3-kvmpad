# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
import unittest

from card_playback_regression import check_picture_size, check_result, panel_scanout, prepared_rate_cases, scale_comparison_cases


def timed_result(frames, metadata=""):
    window = (frames - 1) * 40000 + 160000
    trace = (f"CARD_VIDEO frames={frames} lcd_verification=skipped total={frames}\n"
             f"refresh_frames={frames} window_us={window} p50_us=40000 p95_us=40000 p99_us=40000 vsync=2{metadata}\n"
             f"CARD_HIL_DONE file=x.mp4 mode=blend policy=timed source=companion epoch=1 "
             f"result=ok frames={frames} window_us={window}\n")
    return check_result(trace, "x.mp4", "timed", frames, "blend")["lcd_interval_us"]


class CardTimingTests(unittest.TestCase):
    def test_paired_display_sizes(self):
        cases = list(scale_comparison_cases())
        self.assertEqual(len(cases), 24)
        for i in range(0, len(cases), 2):
            a, b = cases[i:i + 2]
            self.assertEqual(a[:5], b[:5])
            self.assertEqual([a[5], b[5]], ["fit", "1:1"] if i % 4 == 0 else ["1:1", "fit"])
            sizes = dict((case[5], case[6]) for case in (a, b))
            self.assertEqual(sizes["fit"], (960, 540))
            self.assertEqual(sizes["1:1"], [(160, 90), (320, 180), (480, 270)][i // 8])

    def test_picture_size_requires_one_matching_lcd_result(self):
        text = "CARD_HIL_PICTURE width=960 height=540\r\n"
        check_picture_size(text, (960, 540))
        for trace in ("", text * 2, text.replace("960", "480")):
            with self.assertRaises(ValueError):
                check_picture_size(trace, (960, 540))

    def test_scanout_requires_complete_frames_and_product_affinity(self):
        line = "LCD[now] vsync=368 frames_done=368 missed=0 irq_mask_vsync=0x2 irq_mask_frame=0x2"
        self.assertEqual(panel_scanout(line)["frames_done"], 368)
        for wrong in (line.replace("368 missed=0", "321 missed=47"), line.replace("0x2", "0x1"),
                      line.replace("irq_mask_frame=0x2", "")):
            with self.subTest(line=wrong), self.assertRaises(ValueError):
                panel_scanout(wrong)

    def test_temporal_fixtures(self):
        spatial = set(prepared_rate_cases())
        temporal = set(prepared_rate_cases(False, True))
        self.assertEqual((len(spatial), len(temporal)), (39, 24))
        self.assertTrue(spatial.isdisjoint(temporal))
        self.assertEqual(set(prepared_rate_cases(True, True)), spatial | temporal)
        self.assertEqual({row[3] for row in temporal}, {"blend", "smart"})
        self.assertTrue(all(row[0].endswith(".mp4") for row in temporal))

    def test_ring_boundary_and_legacy(self):
        self.assertEqual(timed_result(301)["percentile_scope"], "unreported")
        for frames, scope in ((257, "full"), (258, "last"), (301, "last")):
            stats = timed_result(frames, f" interval_samples=256 interval_total={frames - 1} max_gap_us=200000")
            self.assertEqual((stats["percentile_scope"], stats["sample_count"], stats["total_intervals"],
                              stats["p99"], stats["full_max"]), (scope, 256, frames - 1, 40000, 200000))

    def test_invalid_interval_metadata(self):
        for fields in ("interval_total=300", "interval_samples=0 interval_total=300 max_gap_us=200000",
                       "interval_samples=256 interval_total=301 max_gap_us=200000",
                       "interval_samples=256 interval_total=300 max_gap_us=30000"):
            with self.subTest(fields=fields), self.assertRaises(ValueError):
                timed_result(301, " " + fields)


if __name__ == "__main__":
    unittest.main()
