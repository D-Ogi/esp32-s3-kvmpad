# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Prepare lossless LCD frames for offline playback of a local H.264 source on the 7B."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager, ExitStack
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[2]
MODES = {"exact": (0, ".kvc"), "reduced": (1, ".kvr"), "bank": (2, ".kvb"),
         "rr_blend": (3, ".kvf"), "rr_smart_loose": (4, ".kvs")}
TEMPORAL_MODES = {"rr_blend", "rr_smart_loose"}
ROWS = 8
UNKNOWN_PTS = -(1 << 63)


def build_reader(cc, ff, output):
    src = ROOT / "components/live_view"
    libs = [ff / f"lib{name}/lib{name}.a" for name in ("avformat", "avcodec", "avutil")]
    system = ["-lbcrypt", "-lsecur32", "-lws2_32", "-lshell32"] if os.name == "nt" else ["-lm", "-lpthread"]
    subprocess.run([cc, "-std=gnu11", "-O2", "-DKVM_HAVE_FFMPEG", "-I", src / "include",
                    "-I", ff, "-I", ff / "src", Path(__file__).with_name("card_pixels.c"),
                    src / "h264_decode.c", src / "media_decode.c", src / "cinepak_tile.c",
                    src / "live_message.c", *libs, *system, "-o", output], check=True)


def read(stream, count):
    b = stream.read(count)
    if len(b) != count:
        raise ValueError("The decoder returned an incomplete picture")
    return b


def digest(path, check_cancel=None):
    with path.open("rb") as f:
        if check_cancel is None:
            return hashlib.file_digest(f, "sha256").digest()
        sha = hashlib.sha256()
        while True:
            check_cancel()
            chunk = f.read(1024 * 1024)
            if not chunk:
                return sha.digest()
            sha.update(chunk)


def xor_band(band, previous):
    return (int.from_bytes(band, "little") ^ int.from_bytes(previous, "little")).to_bytes(len(band), "little")


def encode_band(band, previous):
    if band == previous:
        return 3, b""
    choices = [(0, band), (1, zlib.compress(band, 4, -15))]
    if previous is not None:
        choices.append((2, zlib.compress(xor_band(band, previous), 4, -15)))
    return min(choices, key=lambda item: len(item[1]))


def write_frame_bands(out, rgb, previous, block, check_cancel, pool=None):
    def compress(offset):
        band = rgb[offset:offset + block]
        prior = previous[offset:offset + block] if previous is not None else None
        coding, encoded = encode_band(band, prior)
        budget = len(encoded)
        if coding not in (1, 2):
            return coding, encoded, budget, None
        raw = band if coding == 1 else xor_band(band, prior)
        encoded = min(encoded, zlib.compress(raw, 9, -15), key=len)
        compressor = zlib.compressobj(9, zlib.DEFLATED, -15, 8, zlib.Z_FIXED)
        fixed = compressor.compress(raw) + compressor.flush()
        if len(fixed) <= len(encoded):
            return coding, fixed, budget, None
        return coding, encoded, budget, fixed if len(fixed) <= len(raw) else None

    bands = []
    alternatives = []
    spare = 0
    offsets = range(0, len(rgb), block)
    results = pool.map(compress, offsets) if pool else map(compress, offsets)
    # Consume results in row order. Only this thread checks cancellation, chooses the
    # picture budget and writes to the destination, including on a removable drive.
    for coding, encoded, budget, fixed in results:
        if check_cancel:
            check_cancel()
        if fixed is not None:
            alternatives.append((len(fixed) - len(encoded), len(bands), fixed))
        spare += budget - len(encoded)
        bands.append((coding, encoded))

    # Fixed Huffman blocks avoid building a dynamic table on the device. Spend at most
    # the bytes saved on this picture, choosing the least expensive replacements first.
    for extra, index, fixed in sorted(alternatives):
        if extra > spare:
            break
        bands[index] = (bands[index][0], fixed)
        spare -= extra
    for coding, encoded in bands:
        out.write(struct.pack("<HBB", len(encoded), coding, 0))
        out.write(encoded)


@contextmanager
def decoder(reader, source, mode):
    kind = "mp4" if source.suffix.lower() == ".mp4" else "annexb"
    with subprocess.Popen([reader, kind, mode, source], stdout=subprocess.PIPE) as p:
        try:
            yield p.stdout
            if p.wait():
                raise ValueError("H.264 decoding did not finish")
        finally:
            if p.poll() is None:
                p.kill()


@contextmanager
def frame_source(source, reader, mode):
    if reader is None:
        if mode != "exact":
            raise ValueError("This reconstruction mode requires the native reader.")
        from stock_card_pixels import frames

        with frames(source) as decoded:
            yield decoded
        return
    with decoder(reader, source, mode) as pixels:
        w, h = struct.unpack("<II", read(pixels, 8))
        if mode != "exact" and (w % 2 or h % 2):
            raise ValueError("This mode requires H.264 cropping aligned to four source pixels. Choose Exact for this video.")

        def records():
            while first := pixels.read(1):
                duration, pts = struct.unpack("<Iq", first + read(pixels, 11))
                yield duration, pts, read(pixels, w * h * 2)

        yield w, h, records()


def publish_new(tmp, target):
    """Publish a finished file on the same volume without replacing a previous import."""
    if os.name == "nt":
        os.rename(tmp, target)
    else:
        with target.open("xb"):
            pass
        try:
            os.replace(tmp, target)
        except BaseException:
            target.unlink(missing_ok=True)
            raise


def prepare(source, target, reader=None, mode="exact", progress=None, check_cancel=None):
    if mode not in MODES:
        raise ValueError("Choose a supported playback mode.")
    if mode in TEMPORAL_MODES and source.suffix.lower() != ".mp4":
        raise ValueError("Temporal modes require a timed CFR MP4 source")
    if target.exists():
        raise FileExistsError(target)
    size = source.stat().st_size
    if not 0 < size <= 0x7fffffff:
        raise ValueError("The source must fit the board's signed 32-bit file length.")
    source_hash = digest(source, check_cancel)
    tmp = target.parent / (".kvc-" + uuid.uuid4().hex + ".tmp")
    handle = tmp.open("x+b")
    try:
        with handle as out, frame_source(source, reader, mode) as (w, h, frames), ExitStack() as workers:
            if not (1 <= w <= 960 and 1 <= h <= 540):
                raise ValueError("Unsupported displayed picture dimensions")
            pool = None
            concurrency = min(4, os.cpu_count() or 1)
            if w * h >= 320 * 180 and concurrency > 1:
                pool = workers.enter_context(ThreadPoolExecutor(max_workers=concurrency))
            out.write(bytes(64))
            previous = None
            count = 0
            for duration, pts, rgb in frames:
                if not 1 <= duration <= 1000000 or count >= 2073600:
                    raise ValueError("Unsupported frame timing or duration")
                if count == 0:
                    origin = pts
                    next_pts = 0
                timestamp = pts - origin if UNKNOWN_PTS not in (pts, origin) else next_pts
                if timestamp < 0 or timestamp > 86400000000 or (count and timestamp < last_pts):
                    raise ValueError("Discontinuous timestamps")
                out.write(struct.pack("<QII", timestamp, duration, zlib.crc32(rgb)))
                write_frame_bands(out, rgb, previous, w * ROWS * 2, check_cancel, pool)
                previous, last_pts, next_pts = rgb, timestamp, timestamp + duration
                count += 1
                if out.tell() > 0xffffffff:
                    raise ValueError("The companion exceeds the FAT32 single-file limit.")
                if progress:
                    progress(count)
            if not count or digest(source, check_cancel) != source_hash:
                raise ValueError("Empty video or source changed during preparation")
            header = struct.pack("<4sHHIIQ32sI", b"KVC1", w, h, MODES[mode][0], count, size, source_hash, ROWS)
            out.seek(0)
            out.write(header + struct.pack("<I", zlib.crc32(header)))
            out.flush()
            os.fsync(out.fileno())
        if check_cancel:
            check_cancel()
        publish_new(tmp, target)
        return count, w, h
    finally:
        tmp.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--mode", choices=MODES, default="exact")
    parser.add_argument("--output", type=Path)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--reader", type=Path, help="Use a native reader instead of the stock Exact decoder")
    group.add_argument("--ffmpeg-build", type=Path, help="Native build from build_ffmpeg.py")
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    target = args.output or Path(str(args.source) + MODES[args.mode][1])
    if args.mode != "exact" and not (args.reader or args.ffmpeg_build):
        parser.error("this reconstruction mode requires --reader or --ffmpeg-build")
    reader = args.reader.resolve() if args.reader else None
    try:
        if args.ffmpeg_build:
            reader = args.ffmpeg_build.resolve() / (
                ".card-pixels-" + uuid.uuid4().hex + (".exe" if os.name == "nt" else ""))
            build_reader(args.cc, args.ffmpeg_build.resolve(), reader)
        count, w, h = prepare(args.source.resolve(), target, reader, args.mode)
    finally:
        if args.ffmpeg_build and reader:
            reader.unlink(missing_ok=True)
    print(f"{target}: {count} frames, {w}x{h}, {target.stat().st_size} bytes")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"Card video preparation failed: {error}")
