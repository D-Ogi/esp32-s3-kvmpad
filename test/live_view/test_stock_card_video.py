# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Test stock decoder admission, frame timing and offline card import; optionally compare every native frame."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import nullcontext
import hashlib
import io
from itertools import zip_longest
import random
import struct
import sys
import tempfile
import unittest
from fractions import Fraction
from pathlib import Path
import shutil
import subprocess
import uuid
from unittest.mock import patch
import zlib

TOOLS = Path(__file__).resolve().parents[2] / "tools/live_view"
sys.path.insert(0, str(TOOLS))
import stock_card_pixels as stock  # noqa: E402
import prepare_card_video as card  # noqa: E402
import card_video_import as importer  # noqa: E402
from card_video_import import PLAYBACK_MODES, import_video  # noqa: E402


def compare(source, reader):
    rgb_hash = hashlib.sha256()
    timing_hash = hashlib.sha256()
    count = 0
    with (
        card.frame_source(source, reader, "exact") as (nw, nh, native),
        stock.frames(source) as (sw, sh, decoded),
    ):
        if (nw, nh) != (sw, sh):
            raise AssertionError("Display size differs")
        for count, (expected, actual) in enumerate(zip_longest(native, decoded), 1):
            if expected is None or actual is None or expected[:2] != actual[:2]:
                raise AssertionError(f"Frame {count}: count, duration or PTS differs")
            if expected[2] != actual[2]:
                raise AssertionError(f"Frame {count}: RGB565 differs")
            rgb_hash.update(actual[2])
            timing_hash.update(struct.pack("<Iq", *actual[:2]))
    if not count:
        raise AssertionError("Empty comparison")
    print(
        f"{source.name}: {count} frames {sw}x{sh}; RGB565 SHA256 {rgb_hash.hexdigest()}; "
        f"duration/PTS SHA256 {timing_hash.hexdigest()}"
    )
    return count


def make_video(path, options=(), codec="libx264"):
    subprocess.run(
        [
            "ffmpeg",
            "-v",
            "error",
            "-nostdin",
            "-f",
            "lavfi",
            "-i",
            "testsrc2=s=34x22:r=24000/1001",
            "-frames:v",
            "4",
            "-c:v",
            codec,
            *options,
            str(path),
        ],
        check=True,
    )


class StockTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.gettempdir()) / ("stock-card-test-" + uuid.uuid4().hex)
        self.tmp.mkdir()
        self.addCleanup(shutil.rmtree, self.tmp)

    def test_raw_xor_copy_and_frame_budget(self):
        rng = random.Random(1729)
        block = 480
        first = rng.randbytes(block) + rng.randbytes(block) + b"\x12\x34" * 47
        delta = bytes(value ^ 1 for value in first[block:2 * block])
        second = first[:block] + delta + b"\xfa\x56" * 47
        for previous, frame, expected_codes in ((None, first, [0, 0, 1]), (first, second, [3, 2, 1])):
            with self.subTest(previous=previous is not None):
                output = io.BytesIO()
                card.write_frame_bands(output, frame, previous, block, None)
                with ThreadPoolExecutor(max_workers=2) as workers:
                    concurrent = io.BytesIO()
                    card.write_frame_bands(concurrent, frame, previous, block, None, workers)
                self.assertEqual(output.getvalue(), concurrent.getvalue())
                total_budget = 0
                output.seek(0)
                codes = []
                for offset in range(0, len(frame), block):
                    band = frame[offset:offset + block]
                    prior = previous[offset:offset + block] if previous else None
                    choices = [len(band), len(zlib.compress(band, 4, -15))]
                    if prior is not None:
                        difference = bytes(x ^ y for x, y in zip(band, prior, strict=True))
                        choices.append(0 if band == prior else len(zlib.compress(difference, 4, -15)))
                    total_budget += 4 + min(choices)
                    n, code, reserved = struct.unpack("<HBB", output.read(4))
                    payload = output.read(n)
                    codes.append(code)
                    self.assertEqual(reserved, 0)
                    self.assertLessEqual(n, len(band))
                    if code in (1, 2):
                        payload = zlib.decompress(payload, -15)
                    if code == 2:
                        payload = bytes(x ^ y for x, y in zip(payload, prior, strict=True))
                    if code == 3:
                        self.assertEqual(payload, b"")
                        payload = prior
                    self.assertEqual(payload, band)
                self.assertEqual(codes, expected_codes)
                self.assertLessEqual(output.tell(), total_budget)
                self.assertEqual(output.read(), b"")

    def test_size_savings_fund_fixed_huffman_in_another_band(self):
        block = 15360
        pattern = random.Random(0).randbytes(32)
        first = (pattern * (block // len(pattern)))
        rng = random.Random(817)
        tokens = [rng.randbytes(2) for _ in range(20)]
        second = bytearray()
        while len(second) < block:
            second.extend(rng.choice(tokens))
            second.extend(bytes(rng.randrange(7)))
        second = bytes(second[:block])
        old_sizes = [len(zlib.compress(band, 4, -15)) for band in (first, second)]
        output = io.BytesIO()
        card.write_frame_bands(output, first + second, None, block, None)
        output.seek(0)
        length, coding, reserved = struct.unpack("<HBB", output.read(4))
        payload = output.read(length)
        self.assertEqual((coding, reserved, (payload[0] >> 1) & 3), (1, 0, 1))
        self.assertGreater(length, old_sizes[0])
        self.assertEqual(zlib.decompress(payload, -15), first)
        length, coding, reserved = struct.unpack("<HBB", output.read(4))
        self.assertEqual((coding, reserved), (1, 0))
        self.assertEqual(zlib.decompress(output.read(length), -15), second)
        self.assertLessEqual(output.tell(), sum(old_sizes) + 8)

    def test_cancel_during_picture_removes_staging(self):
        source = self.tmp / "source.mp4"
        source.write_bytes(b"fixture")
        folder = self.tmp / "card"
        folder.mkdir()
        # Stop on the third eight-row band of the first picture.
        for width, height in ((8, 32), (960, 540)):
            with self.subTest(width=width):
                band_checks = []
                progress = []

                def frames():
                    band_checks.extend((False, False, True))
                    yield 40000, 0, bytes(width * height * 2)

                decoded = (width, height, frames())
                with patch.object(card, "frame_source", return_value=nullcontext(decoded)):
                    with self.assertRaises(InterruptedError):
                        import_video(source, folder, progress=progress.append,
                                     cancelled=lambda: band_checks.pop(0) if band_checks else False)
                self.assertEqual(progress, [])
                self.assertEqual(list(folder.iterdir()), [])

    def test_cancel_while_hashing_original(self):
        source = self.tmp / "source.h264"
        source.write_bytes(bytes(3 * 1024 * 1024))
        folder = self.tmp / "card"
        folder.mkdir()
        positions = []
        original_open = Path.open
        with source.open("rb") as stream:
            def cancelled():
                positions.append(stream.tell())
                return stream.tell() > 0

            def open_file(path, *args, **kwargs):
                return nullcontext(stream) if path == source else original_open(path, *args, **kwargs)

            with patch.object(Path, "open", open_file), self.assertRaises(InterruptedError):
                import_video(source, folder, cancelled=cancelled)
        self.assertGreater(positions[-1], 0)
        self.assertLess(positions[-1], source.stat().st_size)
        self.assertEqual(list(folder.iterdir()), [])

    def test_concurrent_publication_of_original(self):
        source = self.tmp / "source.mp4"
        make_video(source)
        for matches in (True, False):
            with self.subTest(matches=matches):
                folder = self.tmp / str(matches)
                folder.mkdir()
                original = folder / source.name
                content = source.read_bytes() if matches else b"independent original"

                def publish(tmp, target):
                    if target == original:
                        target.write_bytes(content)
                    card.publish_new(tmp, target)

                with patch.object(importer, "publish_new", side_effect=publish):
                    if matches:
                        self.assertEqual(import_video(source, folder), (4, 17, 11))
                    else:
                        with self.assertRaisesRegex(ValueError, "destination video differs"):
                            import_video(source, folder)
                self.assertEqual(original.read_bytes(), content)
                cache = folder / (source.name + ".kvc")
                self.assertEqual(set(folder.iterdir()), {original, cache} if matches else {original})

    def test_unsupported_frames(self):
        base = dict(width=34, height=22, pix_fmt="yuv420p")
        for invalid in (
            {"width": 1921},
            {"height": 1081},
            {"pix_fmt": "yuv420p10le"},
            {"pix_fmt": "nv12"},
            {"sample_aspect_ratio": "4:3"},
            {"interlaced_frame": "1"},
            {"color_space": "bt2020nc"},
            {"color_primaries": "bt2020"},
            {"color_transfer": "smpte2084"},
        ):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                stock.validate_picture({**base, **invalid})

    def test_rounding(self):
        self.assertEqual(stock.microseconds(1, Fraction(1, 2000000)), 1)
        self.assertEqual(stock.microseconds(-1, Fraction(1, 2000000)), -1)
        self.assertEqual(stock.microseconds(1001, Fraction(1, 24000)), 41708)
        self.assertEqual(
            stock.half_row(memoryview(bytes([0, 1, 255, 0, 1, 253, 4, 5, 10])), 3, 3, 0, 2),
            bytes([1, 254]),
        )
        self.assertEqual(
            stock.half_row(memoryview(bytes([0, 1, 255, 0, 1, 253, 4, 5, 10])), 3, 3, 1, 2),
            bytes([5, 10]),
        )
        for metadata in (
            {"side_data_type": "Display Matrix", "rotation": "0"},
            {"side_data_type": "Mastering display metadata"},
        ):
            with self.assertRaises(ValueError):
                stock.validate_side_data(metadata)

    def test_reject_real_inputs_before_publication(self):
        out = self.tmp / "out.kvc"
        for name, options, codec in (
            ("wide", ["-vf", "setsar=2/1"], "libx264"),
            ("hdr", ["-colorspace", "bt2020nc"], "libx264"),
            ("four", ["-pix_fmt", "yuv444p"], "libx264"),
            ("mpeg", [], "mpeg4"),
        ):
            with self.subTest(name=name):
                video = self.tmp / (name + ".mp4")
                make_video(video, options, codec)
                with self.assertRaises(ValueError):
                    card.prepare(video, out)
                self.assertFalse(out.exists())
                self.assertFalse(list(self.tmp.glob(".kvc-*.tmp")))

    def test_card_pair_reuse_collision_and_cancellation(self):
        source = self.tmp / "source.mp4"
        make_video(source)
        folder = self.tmp / "staged"
        folder.mkdir()
        with self.assertRaises(InterruptedError):
            import_video(source, folder, cancelled=lambda: True)
        self.assertEqual(list(folder.iterdir()), [])
        existing = folder / source.name
        existing.write_bytes(b"preserve")
        with self.assertRaises(FileExistsError):
            import_video(source, folder)
        self.assertEqual(existing.read_bytes(), b"preserve")
        existing.unlink()
        self.assertEqual(import_video(source, folder), (4, 17, 11))
        self.assertEqual(existing.read_bytes(), source.read_bytes())
        cache = folder / (source.name + ".kvc")
        self.assertEqual(cache.read_bytes()[24:56], card.digest(existing))
        with self.assertRaises(FileExistsError):
            import_video(source, folder)
        cache.unlink()
        self.assertEqual(import_video(existing, folder), (4, 17, 11))

    def test_quantized_reordered_card_clock(self):
        source = self.tmp / "quantized.mp4"
        subprocess.run([
            "ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc2=s=36x24:r=24",
            "-frames:v", "12", "-c:v", "libx264", "-threads", "1", "-bf", "2",
            "-g", "12", "-video_track_timescale", "1000", source,
        ], check=True)
        # 24 Hz in a 1 ms track. The decoder reorders B pictures; durations
        # follow the compressed sample, while the display timestamps increase.
        with stock.frames(source) as (_, _, frames):
            times = [(duration, pts) for duration, pts, _ in frames]
        self.assertEqual([pts for _, pts in times], [
            0, 42000, 83000, 125000, 167000, 208000, 250000, 292000, 333000, 375000, 417000, 458000,
        ])
        self.assertEqual(sorted(duration for duration, _ in times), [41000] * 4 + [42000] * 8)
        if READER:
            self.assertEqual(compare(source, READER), 12)
            folder = self.tmp / "staged"
            folder.mkdir()
            self.assertEqual(import_video(source, folder), (12, 18, 12))
            reference = self.tmp / "reference.kvc"
            card.prepare(source, reference, READER)
            self.assertEqual((folder / (source.name + ".kvc")).read_bytes(), reference.read_bytes())

    def test_native_color_and_timing_profiles(self):
        if not READER:
            self.skipTest("pass --reader for independent native RGB, range and PTS comparison")
        for level in ("tv", "pc"):
            for matrix in ("bt709", "smpte170m"):
                with self.subTest(level=level, matrix=matrix):
                    video = self.tmp / (matrix + "-" + level + ".mp4")
                    make_video(
                        video, ["-vf", "setpts=PTS+4", "-color_range", level, "-colorspace", matrix]
                    )
                    self.assertEqual(compare(video, READER), 4)
        variable = self.tmp / "variable.mp4"
        make_video(variable, ["-vf", r"setpts=(N+2*gte(N\,2))/FRAME_RATE/TB", "-fps_mode", "vfr"])
        self.assertEqual(compare(variable, READER), 4)
        variable = self.tmp / "variable-no-reorder.mp4"
        make_video(
            variable, ["-bf", "0", "-vf", r"setpts=(N+2*gte(N\,2))/FRAME_RATE/TB", "-fps_mode", "vfr"]
        )
        self.assertEqual(compare(variable, READER), 4)

    def test_import_annex_b_playback_modes(self):
        if not READER:
            self.skipTest("pass --reader for the spatial card modes")
        source = self.tmp / "source.h264"
        make_video(source, ["-vf", "pad=36:24"])
        folder = self.tmp / "card"
        folder.mkdir()
        for label in ("Reduced", "Large bank"):
            mode = PLAYBACK_MODES[label]
            self.assertEqual(import_video(source, folder, reader=READER, mode=mode), (4, 18, 12))
            companion = folder / (source.name + card.MODES[mode][1])
            self.assertEqual(struct.unpack_from("<I", companion.read_bytes(), 8)[0], card.MODES[mode][0])
        before = set(folder.iterdir())
        for mode in card.TEMPORAL_MODES:
            with self.assertRaisesRegex(ValueError, "constant-frame-rate"):
                import_video(source, folder, reader=READER, mode=mode)
        unaligned = self.tmp / "unaligned.h264"
        make_video(unaligned)
        with self.assertRaisesRegex(ValueError, "cropping aligned"):
            import_video(unaligned, folder, reader=READER, mode="reduced")
        self.assertEqual(set(folder.iterdir()), before)


READER = None
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reader", type=Path)
    parser.add_argument("--compare", type=Path, nargs="+")
    args = parser.parse_args()
    READER = args.reader.resolve() if args.reader else None
    if args.compare:
        if not READER:
            parser.error("--compare needs --reader")
        for video in args.compare:
            compare(video.resolve(), READER)
    else:
        unittest.main(argv=[sys.argv[0]], verbosity=2)
