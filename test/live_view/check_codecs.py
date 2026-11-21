# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Run the host encode/framing/decode checks and export a synthetic board fixture."""

import argparse
import ctypes as ct
import json
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/live_view"))
from framing import FORMATS, Receiver, encode, negotiate

OPTIONS = {
    "jpeg": ["-c:v", "mjpeg", "-q:v", "5"],
    "h264-intra": ["-c:v", "libx264", "-profile:v", "baseline", "-pix_fmt", "yuv420p",
                   "-tune", "zerolatency", "-g", "1", "-bf", "0", "-x264-params", "repeat-headers=1"],
    "cinepak-intra": ["-c:v", "cinepak", "-g", "1", "-q:v", "80", "-max_extra_cb_iterations", "0",
                      "-min_strips", "1", "-max_strips", "1"],
}
SINK = ct.CFUNCTYPE(ct.c_bool, ct.c_void_p, ct.c_uint, ct.POINTER(ct.c_uint16))


class DeviceReceiver(ct.Structure):
    _fields_ = [("stream_id", ct.c_uint32), ("sequence", ct.c_uint32),
                ("codec", ct.c_int), ("received", ct.c_bool)]


def load_device_code(cc, out):
    component = ROOT / "components/live_view"
    lib = out / ("cvid.dll" if sys.platform == "win32" else "cvid.so")
    subprocess.run([cc, "-std=c99", "-O2", "-fPIC", "-shared", "-Wall", "-Wextra", "-Werror",
                    "-I", component / "include", component / "cinepak_tile.c",
                    component / "live_message.c", "-o", lib], check=True)
    device = ct.CDLL(str(lib))
    decoder = device.kvm_cvid_decode
    decoder.argtypes = [ct.c_void_p, ct.c_char_p, ct.c_size_t, SINK, ct.c_void_p]
    decoder.restype = ct.c_bool
    device.kvm_cvid_index.argtypes = [ct.c_void_p, ct.c_char_p, ct.c_size_t]
    device.kvm_cvid_index.restype = ct.c_bool
    device.kvm_cvid_index_band.argtypes = [ct.c_void_p, ct.c_uint, ct.c_void_p]
    device.kvm_cvid_index_band.restype = ct.c_bool
    device.kvm_live_start.argtypes = [ct.POINTER(DeviceReceiver), ct.c_uint32, ct.c_int]
    device.kvm_live_start.restype = ct.c_bool
    device.kvm_live_receive.argtypes = [ct.POINTER(DeviceReceiver), ct.c_char_p, ct.c_size_t]
    device.kvm_live_receive.restype = ct.c_void_p
    device.kvm_live_stop.argtypes = [ct.POINTER(DeviceReceiver)]
    device.kvm_live_stop.restype = None
    return device


def decode_cvid(decoder, data, cancel=False):
    pixels = bytearray()
    rows = []

    @SINK
    def sink(_, y, words):
        rows.append(y)
        pixels.extend(ct.string_at(words, 960 * 4 * 2))
        return not cancel

    workspace = (ct.c_uint16 * (2 * 256 * 4 + 4 * 960))()
    result = decoder(workspace, bytes(data), len(data), sink, None)
    return result, rows, pixels


def rgb565(rgb):
    triples = struct.iter_unpack("BBB", rgb)
    return b"".join(struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
                    for r, g, b in triples)


def indexed_pixels(device, packet):
    # Opaque caller-owned workspace large enough for both mixed and V1-only indices.
    index = ct.create_string_buffer(160 * 1024)
    if not device.kvm_cvid_index(index, bytes(packet), len(packet)):
        return None
    band = (ct.c_uint16 * (4 * 960))()
    pixels = bytearray(960 * 540 * 2)
    # LVGL can ask for nonadjacent bands when only part of a view needs repainting.
    for y in reversed(range(0, 540, 4)):
        assert device.kvm_cvid_index_band(index, y, band)
        pixels[y * 1920:(y + 4) * 1920] = bytes(band)
    return pixels


def check_cinepak_vectors(device):
    def chunk(kind, contents):
        return bytes([kind]) + (4 + len(contents)).to_bytes(3, "big") + contents

    def packet(samples):
        # One grayscale V1 codebook entry, one index for each 4x4 block.
        books = chunk(0x26, bytes([0, 255, 255, 0]))
        strip = chunk(0x10, struct.pack(">4H", 0, 0, 540, 960) + books + chunk(0x32, samples))
        return chunk(0, struct.pack(">3H", 960, 540, 1) + strip)

    indices = bytes(960 * 540 // 16)
    ok, rows, pixels = decode_cvid(device.kvm_cvid_decode, packet(indices))
    assert ok and rows[-1] == 536
    assert pixels[:8] == struct.pack("<4H", 0, 0, 0xffff, 0xffff)
    assert indexed_pixels(device, packet(indices)) == pixels
    for invalid in (indices[:-1], indices + b"\0", indices[:-1] + b"\1"):
        assert decode_cvid(device.kvm_cvid_decode, packet(invalid))[:2] == (False, [])
        assert indexed_pixels(device, packet(invalid)) is None


def expect_reject(receiver, message):
    try:
        receiver.receive(message)
    except ValueError:
        return
    raise AssertionError("receiver accepted an invalid message")


def exercise(name, out, device):
    fmt = FORMATS[name]
    dims = f"{fmt.width}x{fmt.height}"
    avi = out / f"{name}.avi"
    ffmpeg = ["ffmpeg", "-nostdin", "-v", "error"]
    subprocess.run([*ffmpeg, "-f", "lavfi", "-i", f"testsrc2=s={dims}:r=5", "-frames:v", "3",
                    *OPTIONS[name], "-threads", "1", "-y", str(avi)], check=True)
    metadata = subprocess.check_output(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_packets",
                                        "-show_entries", "packet=pos,size,flags", "-of", "json", avi])
    packets = json.loads(metadata)["packets"]
    expected = subprocess.check_output([*ffmpeg, "-i", avi, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"])
    frame_size = fmt.width * fmt.height * 3
    assert len(packets) == 3 and len(expected) == 3 * frame_size
    assert negotiate([name], FORMATS) == name
    assert negotiate([name], []) is None
    receiver = Receiver(name, 17)
    native = DeviceReceiver()
    assert device.kvm_live_start(ct.byref(native), 17, fmt.kind)
    decoder = device.kvm_cvid_decode

    def receive_native(message):
        return device.kvm_live_receive(ct.byref(native), message, len(message))

    def reject(message):
        expect_reject(receiver, message)
        assert not receive_native(message)

    sizes = []
    with avi.open("rb") as stream:
        for i, meta in enumerate(packets):
            assert "K" in meta["flags"]
            stream.seek(int(meta["pos"]))
            payload = stream.read(int(meta["size"]))
            # Include a skipped sequence and a 32-bit wrap, with an independent picture after each.
            seq = [0xfffffffd, 0xffffffff, 0][i]
            message = encode(name, 17, seq, 200 * i, payload)
            reject(message[:-1])
            reject(message[:-1] + bytes([message[-1] ^ 1]))
            reject(encode(name, 18, seq, 200 * i, payload))
            for pos in (0, 4, 5, 6, 7, 20, 22, 24):
                damaged = bytearray(message)
                damaged[pos] ^= 0x80
                struct.pack_into("<I", damaged, 28, zlib.crc32(damaged[32:], zlib.crc32(damaged[:28])))
                reject(bytes(damaged))
            received = receiver.receive(message)
            pointer = receive_native(message)
            assert pointer and ct.string_at(pointer, len(payload)) == received
            reject(message)
            reference = expected[i * frame_size:(i + 1) * frame_size]
            if name == "cinepak-intra":
                ok, rows, pixels = decode_cvid(decoder, received)
                assert ok and rows == list(range(0, 540, 4)) and pixels == rgb565(reference)
                assert indexed_pixels(device, received) == pixels
                assert decode_cvid(decoder, received, cancel=True)[:2] == (False, [0])
                for pos in (1, 4, 8, 10, 11, 20, 22, 23):
                    bad = bytearray(received)
                    bad[pos] ^= 128
                    assert decode_cvid(decoder, bad)[:2] == (False, [])
                if i == 1:
                    (out / "cinepak-message-2.bin").write_bytes(message)
                if i == 0:
                    (out / "cinepak-frame.bin").write_bytes(received)
                    (out / "cinepak-message.bin").write_bytes(message)
                    checksum = f"0x{zlib.crc32(pixels):08x}"
                    (out / "cinepak-crc.txt").write_text(checksum, encoding="ascii")
                    print(f"Cinepak board fixture: {checksum} RGB565 CRC32, {len(received)} input bytes")
            else:
                demuxer = "h264" if name == "h264-intra" else "mjpeg"
                actual = subprocess.check_output([*ffmpeg, "-f", demuxer, "-i", "-", "-f", "rawvideo",
                                                  "-pix_fmt", "rgb24", "-"], input=bytes(received))
                assert actual == reference
            sizes.append(len(received))
        receiver.stop()
        device.kvm_live_stop(ct.byref(native))
        reject(encode(name, 17, 1, 600, payload))
    print(f"{name}: three independently decoded {dims} pictures, payload bytes {sizes}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True, help="directory for synthetic fixtures and build products")
    parser.add_argument("--cc", default="cc", help="host C compiler")
    args = parser.parse_args()
    if not __debug__:
        sys.exit("run with assertions enabled")
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    device = load_device_code(args.cc, args.out)
    check_cinepak_vectors(device)
    for codec in OPTIONS:
        exercise(codec, args.out, device)
