# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Exercise the firmware queue, cancellation and staged RGB565 output on synthetic H.264 input."""
import argparse
import ctypes as ct
import json
from pathlib import Path
import subprocess
import sys
import zlib

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/live_view"))
from framing import encode


class Receiver(ct.Structure):
    _fields_ = [("id", ct.c_uint32), ("seq", ct.c_uint32), ("codec", ct.c_int), ("received", ct.c_bool)]


class Queue(ct.Structure):
    _fields_ = [("rx", Receiver), ("buf", ct.c_void_p * 2), ("cap", ct.c_size_t),
                ("length", ct.c_size_t * 2), ("waiting", ct.c_int), ("decoding", ct.c_int)]


CANCEL = ct.CFUNCTYPE(ct.c_bool, ct.c_void_p)


class Surface(ct.Structure):
    _fields_ = [("pixels", ct.c_void_p), ("width", ct.c_uint), ("height", ct.c_uint),
                ("stride", ct.c_uint), ("cancel", CANCEL), ("ctx", ct.c_void_p)]

class Yuv(ct.Structure):
    _fields_ = [("plane", ct.c_void_p * 3), ("stride", ct.c_int * 3), ("width", ct.c_uint),
                ("height", ct.c_uint), ("pts", ct.c_int64), ("duration", ct.c_int64),
                ("full", ct.c_bool), ("bt709", ct.c_bool)]


class Picture(ct.Structure):
    _fields_ = [("yuv", Yuv), ("reference", ct.c_void_p)]


def load(path):
    lib = ct.CDLL(str(path.resolve()))
    for name, args, result in (
        ("kvm_live_queue_init", [ct.POINTER(Queue), ct.c_void_p, ct.c_void_p, ct.c_size_t, ct.c_uint32,
                                 ct.c_int], ct.c_bool),
        ("kvm_live_queue_init_in_place", [ct.POINTER(Queue), ct.c_void_p, ct.c_size_t, ct.c_uint32,
                                          ct.c_int], ct.c_bool),
        ("kvm_live_queue_writable", [ct.POINTER(Queue)], ct.c_bool),
        ("kvm_live_queue_push", [ct.POINTER(Queue), ct.c_char_p, ct.c_size_t], ct.c_bool),
        ("kvm_live_queue_take", [ct.POINTER(Queue), ct.POINTER(ct.c_size_t)], ct.c_void_p),
        ("kvm_live_queue_release", [ct.POINTER(Queue)], None),
        ("kvm_live_queue_stop", [ct.POINTER(Queue)], None),
        ("kvm_media_decode_live", [ct.c_int, ct.c_void_p, ct.c_size_t, ct.POINTER(Surface)], ct.c_bool),
        ("kvm_media_decoder_open", [ct.c_int], ct.c_void_p),
        ("kvm_media_decoder_close", [ct.c_void_p], None),
        ("kvm_media_h264_picture", [ct.c_void_p, ct.POINTER(Picture), ct.c_char_p, ct.c_size_t,
                                    CANCEL, ct.c_void_p], ct.c_bool),
        ("kvm_media_h264_band", [ct.POINTER(Picture), ct.c_uint, ct.c_void_p], ct.c_bool),
        ("kvm_media_render_yuv", [ct.POINTER(Yuv), ct.c_bool, ct.POINTER(Surface)], ct.c_bool),
        ("kvm_media_h264_release", [ct.POINTER(Picture)], None),
    ):
        fn = getattr(lib, name)
        fn.argtypes, fn.restype = args, result
    return lib


def check_display_conversion(lib):
    # Independent array arithmetic over tightly cropped samples. Input and output
    # rows have distinct padding so a neighboring row cannot stand in for an edge.
    rng = np.random.default_rng(20261119)
    cases = 0
    for w in range(1, 10):
        for h in range(1, 10):
            planes = []
            for pw, ph, pitch in ((w, h, 5), ((w + 1)//2, (h + 1)//2, 3), ((w + 1)//2, (h + 1)//2, 7)):
                storage = rng.integers(0, 256, (ph, pw + pitch), dtype=np.uint8)
                planes.append(storage[:, 1:pw + 1])
            # Exercise transitions between neutral and colored blocks, including an unpaired edge pixel.
            for chroma in planes[1:]:
                chroma[:, ::2] = 128
            for half in (False, True):
                ow, oh = (w//2, h//2) if half else (w, h)
                if not ow or not oh:
                    continue
                sampled = []
                for plane in planes:
                    a = plane.astype(np.int32)
                    if half:
                        a = np.pad(a, ((0, a.shape[0] % 2), (0, a.shape[1] % 2)), mode="edge")
                        a = (a[::2, ::2] + a[::2, 1::2] + a[1::2, ::2] + a[1::2, 1::2] + 2) // 4
                    sampled.append(a)
                luma = sampled[0][:oh, :ow]
                u, v = [p.repeat(2, 0).repeat(2, 1)[:oh, :ow] - 128 for p in sampled[1:]]
                for full, bt709, rv, gu, gv, bu in (
                    (False, False, 409, 100, 208, 516), (False, True, 459, 55, 136, 541),
                    (True, False, 359, 88, 183, 454), (True, True, 403, 48, 120, 475),
                ):
                    yy = 256 * luma + 128 if full else 298 * (luma - 16) + 128
                    r, g, b = [np.clip(p // 256, 0, 255) for p in (yy + rv*v, yy - gu*u - gv*v, yy + bu*u)]
                    expected = np.full((oh + 2, ow + 6), 0xa55a, dtype=np.uint16)
                    expected[1:-1, 3:-3] = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3
                    output = np.full_like(expected, 0xa55a)
                    frame = Yuv((ct.c_void_p * 3)(*(p.ctypes.data for p in planes)),
                                (ct.c_int * 3)(*(p.strides[0] for p in planes)), w, h, 0, 0, full, bt709)
                    target = Surface(output[1:-1, 3:-3].ctypes.data, ow, oh, output.shape[1], CANCEL(), None)
                    assert lib.kvm_media_render_yuv(frame, half, target)
                    assert np.array_equal(output, expected), (w, h, half, full, bt709)
                    cases += 1
                    if half and w == h == 8:
                        output.fill(0xa55a)
                        target.cancel = CANCEL(lambda _: bool(np.any(output[2, 3:-3] != 0xa55a)))
                        assert not lib.kvm_media_render_yuv(frame, half, target)
                        assert np.array_equal(output[:3], expected[:3]) and np.all(output[3:] == 0xa55a)
    print(f"{cases} RGB565 matrix, range, scale and odd crop cases matched, including output guards")


def expected(payload):
    raw = subprocess.check_output(["ffmpeg", "-v", "error", "-f", "h264", "-i", "-", "-frames:v", "1",
                                   "-pix_fmt", "yuv420p", "-f", "rawvideo", "-"], input=payload)
    data = np.frombuffer(raw, np.uint8).astype(np.int32)
    y = (data[:512**2].reshape(512, 512) - 16) * 298 + 128
    u, v = [(c.reshape(256, 256) - 128).repeat(2, 0).repeat(2, 1)
            for c in np.split(data[512**2:], 2)]
    r, g, b = [np.clip(c // 256, 0, 255) for c in (y + 409*v, y - 100*u - 208*v, y + 516*u)]
    return ((r >> 3) << 11 | (g >> 2) << 5 | b >> 3).astype(np.uint16)

def dense_idr():
    rng = np.random.default_rng(20261119)
    raw = rng.integers(0, 256, 512 * 512 * 3 // 2, dtype=np.uint8).tobytes()
    for qp in range(24, 52):
        command = ["ffmpeg", "-v", "error", "-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "512x512",
                   "-i", "-", "-frames:v", "1", "-c:v", "libx264", "-threads", "1", "-profile:v", "baseline",
                   "-refs", "1", "-qp", str(qp), "-f", "h264", "-"]
        unit = subprocess.check_output(command, input=raw)
        if 80 * 1024 <= len(unit) <= 96 * 1024 - 32:
            return unit
    raise AssertionError("could not generate a dense baseline IDR near the wire bound")


def check_retained(lib, units):
    session = lib.kvm_media_decoder_open(2)
    assert session
    units = [*units, dense_idr()]
    slots = [Picture(), Picture()]
    callback = CANCEL(lambda _: False)
    pixels = np.zeros((512, 512), dtype=np.uint16)
    gold = [expected(unit) for unit in units]
    for i in range(18):
        current = i % 2
        source = i % len(units)
        unit = units[source]
        # One valid unregistered-user-data SEI brings the access unit to the
        # maximum wire size (the size itself is encoded as repeated 0xff).
        capacity = 96 * 1024 - len(unit) - 8
        count = capacity - capacity // 256
        while count + count // 255 > capacity:
            count -= 1
        extra = b"\x00\x00\x00\x01\x06\x05" + b"\xff" * (count // 255) + bytes([count % 255])
        unit = extra + b"X" * count + b"\x80" + unit
        assert 96 * 1024 - 1 <= len(unit) <= 96 * 1024
        assert lib.kvm_media_h264_picture(session, slots[current], unit, len(unit), callback, None)
        for slot, compare in [(current, source), (1 - current, (i - 1) % len(units))]:
            if not slots[slot].reference:
                continue
            for y in range(0, 512, 4):
                assert lib.kvm_media_h264_band(slots[slot], y, pixels[y].ctypes.data)
            assert np.array_equal(pixels, gold[compare])
    for slot in slots:
        lib.kvm_media_h264_release(slot)
        assert not slot.reference
    lib.kvm_media_decoder_close(session)
    print(f"18 near-96-KiB IDRs retained across slot swaps including {len(units[-1])}-byte dense IDR; RGB565 matched")


def check_wire_queue(lib, frames):
    q, size = Queue(), ct.c_size_t()
    capacity = 96 * 1024
    wire = ct.create_string_buffer(32 + capacity)
    assert lib.kvm_live_queue_init_in_place(q, wire, capacity, 42, 2)
    for frame in frames:
        assert lib.kvm_live_queue_writable(q)
        assert not lib.kvm_live_queue_push(q, frame, len(frame))  # Not the transport's allocation.
        ct.memmove(wire, frame, len(frame))
        assert lib.kvm_live_queue_push(q, wire, len(frame)) and not lib.kvm_live_queue_writable(q)
        assert not lib.kvm_live_queue_push(q, wire, len(frame))
        payload = lib.kvm_live_queue_take(q, size)
        assert payload == ct.addressof(wire) + 32 and ct.string_at(payload, size.value) == frame[32:]
        assert not lib.kvm_live_queue_writable(q) and not lib.kvm_live_queue_push(q, wire, len(frame))
        lib.kvm_live_queue_release(q)
        assert not any(wire.raw[32:]) and lib.kvm_live_queue_writable(q)
    ct.memmove(wire, frames[-1], len(frames[-1]))
    lib.kvm_live_queue_stop(q)  # Socket may still be filling the packet.
    assert not lib.kvm_live_queue_writable(q) and not lib.kvm_live_queue_push(q, wire, len(frames[-1]))
    assert wire.raw[:len(frames[-1])] == frames[-1]
    print("Paced wire ownership, sequence rollover, invalidation and producer/decoder exclusion passed")


def run(lib, avi, out=None):
    packets = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-show_packets", "-select_streams",
                        "v:0", "-show_entries", "packet=pos,size", "-of", "json", avi]))["packets"]
    data = avi.read_bytes()
    units = [data[int(p["pos"]):int(p["pos"]) + int(p["size"])] for p in packets]
    assert len(units) == 3
    if out:
        out.mkdir(parents=True,exist_ok=True)
        (out/"idr-message.bin").write_bytes(encode("h264-intra",17,0,0,units[0]))
        (out/"idr-crc.txt").write_text(hex(zlib.crc32(expected(units[0]).astype("<u2").tobytes())),encoding="ascii")
    q = Queue()
    a, b = (ct.create_string_buffer(96*1024) for _ in range(2))
    assert lib.kvm_live_queue_init(q, a, b, len(a), 42, 2)
    frames = [encode("h264-intra", 42, seq, i*200, payload)
              for i, (seq, payload) in enumerate(zip([0xfffffffe, 0xffffffff, 1], units))]
    check_wire_queue(lib, frames)
    size = ct.c_size_t()
    def push(i):
        return lib.kvm_live_queue_push(q, frames[i], len(frames[i]))
    assert push(0)
    first = lib.kvm_live_queue_take(q, size)
    assert first and size.value == len(units[0])
    assert push(1) and push(2) and not push(1)
    assert not lib.kvm_live_queue_take(q, size)
    assert ct.string_at(first, size.value) == units[0]
    cancelled = False
    callback = CANCEL(lambda _: cancelled)
    pixels = np.full((512, 520), 0xcccc, dtype=np.uint16)
    surface = Surface(pixels.ctypes.data, 512, 512, 520, callback, None)
    for i, ptr in [(0, first), (1, units[1]), (2, units[2])]:
        assert lib.kvm_media_decode_live(2, ptr, len(units[i]), surface)
        assert np.array_equal(pixels[:, :512], expected(units[i]))
        assert (pixels[:, 512:] == 0xcccc).all()
    assert not lib.kvm_media_decode_live(2, units[0] + units[1], len(units[0])+len(units[1]), surface)
    wrong_size = subprocess.check_output([
        "ffmpeg", "-v", "error", "-f", "lavfi", "-i", "color=size=496x512",
        "-frames:v", "1", "-c:v", "libx264", "-profile:v", "baseline", "-refs", "1",
        "-pix_fmt", "yuv420p", "-f", "h264", "-"])
    assert not lib.kvm_media_decode_live(2, wrong_size, len(wrong_size), surface)
    lib.kvm_live_queue_release(q)
    newest = lib.kvm_live_queue_take(q, size)
    assert newest and ct.string_at(newest, size.value) == units[2]
    lib.kvm_live_queue_stop(q)
    assert not push(2)
    cancelled = True
    assert not lib.kvm_media_decode_live(2, newest, size.value, surface)
    lib.kvm_live_queue_release(q)
    assert not any(a.raw) and not any(b.raw)
    check_retained(lib, units)
    print("3 independent IDR units matched 786432 RGB565 pixels; queue replacement, Stop, cancel and padding passed")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("library", type=Path)
    p.add_argument("fixture", type=Path, help="synthetic h264-intra.avi from check_codecs.py")
    p.add_argument("--out",type=Path,help="write a synthetic IDR message and its independent RGB565 CRC")
    p.add_argument("--arena-kib", type=int, help="run against a fixed-size bounded-codecs TLSF pool")
    args = p.parse_args()
    lib=load(args.library)
    if args.arena_kib:
        assert lib.kvm_test_pool_open(args.arena_kib * 1024)
    check_display_conversion(lib)
    run(lib, args.fixture,args.out)
    if args.arena_kib:
        assert lib.kvm_test_pool_close()
    if hasattr(lib,"kvm_probe_report"): lib.kvm_probe_report()
