# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Compare every reconstructed card frame to H.264 output and exercise rejected companions."""
import argparse
from pathlib import Path
import struct
import subprocess
import sys
import uuid
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/live_view"))
from prepare_card_video import UNKNOWN_PTS, decoder, read


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source", "cache", "reader", "check", "work"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()

    def run(source, cache, command="read", success=True):
        p = subprocess.run([args.check.resolve(), source, cache, command], check=False, capture_output=True, text=True)
        assert p.returncode == (0 if success else 1), (p.returncode, p.stderr)
        return p.stdout.splitlines()

    rows = run(args.source, args.cache)
    with decoder(args.reader.resolve(), args.source, "exact") as frames:
        w, h = struct.unpack("<II", read(frames, 8))
        next_pts = 0
        for i, row in enumerate(rows):
            duration, pts = struct.unpack("<Iq", read(frames, 12))
            crc = zlib.crc32(read(frames, 2 * w * h))
            if i == 0:
                origin = pts
            when = next_pts if UNKNOWN_PTS in (pts, origin) else pts - origin
            assert row == f"{i} {when} {crc:08x}", (row, i)
            next_pts = when + duration
        assert rows and not frames.read(1)
    run(args.source, args.cache, "cancel", False)
    temp = args.work / ("cache-check-" + uuid.uuid4().hex)
    raw = args.cache.read_bytes()
    try:
        temp.write_bytes(b"unrelated")
        run(temp, args.cache, success=False)
        with args.source.open("rb") as f:
            changed = bytearray(f.read())
        changed[-1] ^= 1
        temp.write_bytes(changed)
        run(temp, args.cache, success=False)
        for broken in (raw[:63], raw[:-1], raw + b"extra", raw[:80] + b"\xff\xff\x04\x00" + raw[84:],
                       raw[:76] + bytes([raw[76] ^ 1]) + raw[77:]):
            temp.write_bytes(broken)
            run(args.source, temp, success=False)
        # End exactly at a full reader window. An appended byte must be detected on the next read.
        width, height = 679, 12
        pixels = bytes((i % 251 for i in range(2 * width * height)))
        header = bytearray(raw[:60])
        struct.pack_into("<HHII", header, 4, width, height, 0, 1)
        fixture = header + struct.pack("<IQII", zlib.crc32(header), 0, 40000, zlib.crc32(pixels))
        for start in range(0, len(pixels), 2 * width * 8):
            band = pixels[start:start + 2 * width * 8]
            fixture.extend(struct.pack("<HBB", len(band), 0, 0) + band)
        assert len(fixture) == 16384
        temp.write_bytes(fixture)
        assert run(args.source, temp) == [f"0 0 {zlib.crc32(pixels):08x}"]
        temp.write_bytes(fixture + b"x")
        run(args.source, temp, success=False)
        # End a compressed XOR band on a half-word. Check both in-place reconstruction
        # and callers supplying RGB565 surfaces which are only two-byte aligned.
        width, height = 35, 11
        struct.pack_into("<HHII", header, 4, width, height, 0, 3)
        samples = [bytes((i // 8 + frame) % 251 for i in range(width * height * 2)) for frame in (0, 1)]
        samples.append(samples[1])
        fixture = header + struct.pack("<I", zlib.crc32(header))
        expected = bytearray()
        for i, picture in enumerate(samples):
            pts, crc = 40000 * i, zlib.crc32(picture)
            fixture += struct.pack("<QII", pts, 40000, crc)
            expected += struct.pack("<IQI", i, pts, crc) + picture
            for off in range(0, len(picture), 16 * width):
                band = picture[off:off + 16 * width]
                if i == 0:
                    coding = 1 if off else 0
                    payload = zlib.compress(band, wbits=-15) if coding else band
                elif i == 1:
                    coding = 2
                    delta = bytes(a ^ b for a, b in zip(band, samples[0][off:]))
                    payload = zlib.compress(delta, wbits=-15)
                else:
                    coding, payload = 3, b""
                fixture += struct.pack("<HBB", len(payload), coding, 0) + payload
        temp.write_bytes(fixture)
        for layout in ("pixels-one", "pixels-two", "pixels-one-unaligned", "pixels-two-unaligned"):
            pixels = subprocess.check_output([args.check.resolve(), args.source, temp, layout])
            assert pixels == expected, layout
    finally:
        temp.unlink(missing_ok=True)
    print(f"PASS: {len(rows)} ordered {w}x{h} frames and timestamps, cancellation, source binding and corruption checks.")


if __name__ == "__main__":
    main()
