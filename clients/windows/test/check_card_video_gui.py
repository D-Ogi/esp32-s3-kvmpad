# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Verify the published Tk importer against temporal references and stream-copy edits at four frame rates."""
import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import tkinter as tk
from unittest.mock import patch
import uuid
import zlib


def decoded_frames(stream, width, height, rows, count):
    previous = b""
    for _ in range(count):
        start = stream.tell()
        frame_header = stream.read(16)
        _, _, crc = struct.unpack("<QII", frame_header)
        picture = bytearray()
        for y in range(0, height, rows):
            raw_length = width * min(rows, height - y) * 2
            n, coding, reserved = struct.unpack("<HBB", stream.read(4))
            assert reserved == 0 and n <= raw_length
            encoded = stream.read(n)
            assert len(encoded) == n
            if coding in (1, 2):
                band = zlib.decompress(encoded, -15)
            elif coding == 0:
                band = encoded
            else:
                assert coding == 3 and not encoded
            if coding in (2, 3):
                prior = previous[len(picture):len(picture) + raw_length]
                band = prior if coding == 3 else bytes(x ^ y for x, y in zip(band, prior, strict=True))
            assert len(band) == raw_length
            picture.extend(band)
        assert zlib.crc32(picture) == crc
        previous = bytes(picture)
        yield frame_header, previous, stream.tell() - start
    assert stream.read(1) == b""


def compare_companions(actual, reference):
    with actual.open("rb") as new, reference.open("rb") as old:
        header = new.read(64)
        assert header == old.read(64) and header[:4] == b"KVC1"
        assert zlib.crc32(header[:60]) == struct.unpack_from("<I", header, 60)[0]
        width, height, _, count = struct.unpack_from("<HHII", header, 4)
        rows, = struct.unpack_from("<I", header, 56)
        left = decoded_frames(new, width, height, rows, count)
        right = decoded_frames(old, width, height, rows, count)
        for index, (current, original) in enumerate(zip(left, right, strict=True)):
            assert current[:2] == original[:2], (actual, index, "pixels or timing")
            assert current[2] <= original[2], (actual, index, "frame size")


def check_appearance(window):
    import appearance

    palettes = [
        ("#f4f6f8", "#172332", "#ffffff", "#172332", "#15559a", "#ffffff", "#526174"),
        ("#16202b", "#ecf2f8", "#263442", "#ecf2f8", "#9bceff", "#12202d", "#b2c0cc"),
        ("#000000", "#ffffff", "#000000", "#ffffff", "#ffff00", "#000000", "#dddddd"),
    ]
    for palette in palettes:
        with patch.object(appearance, "system_palette", return_value=palette):
            window.appearance.refresh()
            window.dropdown_colors()
        assert window.appearance.image and window.root.cget("background") == palette[0]
        tkapp = window.root.tk
        listbox = str(tkapp.call("ttk::combobox::PopdownWindow", window.modes)) + ".f.l"
        values = tuple(str(tkapp.call(listbox, "cget", "-" + option))
                       for option in ("background", "foreground", "selectbackground", "selectforeground"))
        assert values == palette[2:6], (values, palette)
    window.appearance.refresh()


def check_trimmed_card_clock(window, run, dest):
    from prepare_card_video import frame_source

    # A stream-copy edit inside a long GOP keeps 90 earlier packets for decoding only.
    modes = (("Exact", "exact", ".kvc"), ("Reduced", "reduced", ".kvr"),
             ("Large bank", "bank", ".kvb"), ("Temporal blend", "rr_blend", ".kvf"),
             ("Temporal smart loose", "rr_smart_loose", ".kvs"))
    ffmpeg = ["ffmpeg", "-v", "error", "-nostdin", "-y"]
    for fps in (5, 10, 15, 25):
        uncut = dest / "uncut.mp4"
        source = dest / f"trim-{fps}.mp4"
        # Fixed B-picture placement makes the first visible picture a reference for all five modes.
        subprocess.run([*ffmpeg, "-f", "lavfi", "-i", f"testsrc2=size=64x48:rate={fps}",
                        "-frames:v", "130", "-c:v", "libx264", "-threads", "1", "-g", "250",
                        "-sc_threshold", "0", "-bf", "2", "-x264-params", "b-adapt=0", uncut], check=True)
        subprocess.run([*ffmpeg, "-ss", str(90 / fps), "-i", uncut, "-c", "copy", source], check=True)
        window.source.set(str(source))
        for mode_id, (label, mode, suffix) in enumerate(modes):
            window.mode.set(label)
            message = run()
            name = source.name + suffix
            assert f"{name} contains 40 frames" in message, message
            button = ("Play", "Reduced", "Large bank", "Temporal blend", "Smart loose")[mode_id]
            assert "root of the microSD card" in message and "Extensions → Video files" in message, message
            assert message.endswith(f"select {source.name} and tap {button}."), message
            with frame_source(uncut, window.reader, mode) as (_, _, original):
                expected_pixels = [pixels for index, (_, _, pixels) in enumerate(original) if index >= 90]
            with (dest / name).open("rb") as companion:
                header = companion.read(64)
                width, height, saved_mode, count = struct.unpack_from("<HHII", header, 4)
                assert (width, height, saved_mode, count) == (32, 24, mode_id, 40)
                rows, = struct.unpack_from("<I", header, 56)
                records = decoded_frames(companion, width, height, rows, count)
                for index, ((record, pixels, _), expected) in enumerate(zip(records, expected_pixels, strict=True)):
                    pts, duration, _ = struct.unpack("<QII", record)
                    start = round(index * 1000000 / fps)
                    end = round((index + 1) * 1000000 / fps)
                    expected_duration = end - start if mode_id >= 3 else round(1000000 / fps)
                    assert (pts, duration) == (start, expected_duration), (name, index, pts, duration)
                    assert pixels == expected, (name, index)
                assert pts + duration == round(count * 1000000 / fps)
            print(f"{name}: 40 matching RGB565 frames, PTS and durations, end {pts + duration} us", flush=True)


def check(args):
    sys.path.insert(0, str(args.media.resolve()))
    from card_video_import import ImportWindow
    from prepare_card_video import digest

    dest = args.work / uuid.uuid4().hex
    dest.mkdir(parents=True)
    localized = dest / "Zażółć 日本語"
    root = tk.Tk()
    root.withdraw()
    window = ImportWindow(root)
    assert window.reader.is_file()
    window.folder.set(str(dest))

    def run():
        window.start.invoke()
        until = time.monotonic() + 120
        while window.task is not None:
            if time.monotonic() > until:
                raise TimeoutError(window.status.get())
            root.update()
            time.sleep(0.025)
        return window.status.get()

    try:
        if sys.platform == "win32":
            check_appearance(window)
        raw = dest / "library-check.avc"
        subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=64x48:rate=5",
                        "-frames:v", "3", "-c:v", "libx264", "-f", "h264", raw], check=True)
        window.source.set(str(raw))
        for label in window.modes.cget("values"):
            window.mode.set(label)
            assert ".mp4, .h264 or .264" in run()
        assert list(dest.iterdir()) == [raw]
        for label, extension, suffix in (("Reduced", ".H264", ".kvr"), ("Large bank", ".264", ".kvb")):
            source = raw.with_suffix(extension)
            shutil.copyfile(raw, source)
            window.source.set(str(source))
            window.mode.set(label)
            message = run()
            assert f"{source.name}{suffix} contains 3 frames" in message, message
            assert message.endswith(f"select {source.name} and tap {label}."), message
        for clip, count in (("bear", 82), ("flower", 150), ("joy", 171), ("pan-cut-regenerated", 96)):
            source = args.samples / (clip + ".mp4")
            if clip == "bear":
                # A file selected in a localized Windows directory must reach the native reader intact.
                localized.mkdir()
                source = Path(shutil.copyfile(source, localized / source.name))
            window.source.set(str(source))
            for label, suffix in (("Temporal blend", ".kvf"), ("Temporal smart loose", ".kvs")):
                window.mode.set(label)
                message = run()
                name = source.name + suffix
                assert name in message and f"{count} frames" in message, message
                compare_companions(dest / name, args.reference / name)
                assert digest(dest / source.name) == digest(source)
                assert "already exists" in run()
                print(f"{name}: {count} frames, byte-identical RGB565, timing and header", flush=True)
        window.source.set(str(args.samples / "untimed.h264"))
        assert "constant-frame-rate" in run()
        check_trimmed_card_clock(window, run, dest)
    finally:
        root.destroy()
        for path in dest.iterdir():
            if path.is_file():
                path.unlink()
        (localized / "bear.mp4").unlink(missing_ok=True)
        if localized.exists():
            localized.rmdir()
        dest.rmdir()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for field in ("media", "samples", "reference", "work"):
        parser.add_argument(field, type=Path)
    check(parser.parse_args())
