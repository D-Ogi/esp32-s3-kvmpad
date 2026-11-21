# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Offscreen D3D11 hardware or WARP textures, real HOST encoders, KVS framing and independent FFmpeg decoding."""
import argparse
import asyncio
import os
from pathlib import Path
import struct
import zlib
import subprocess
import time
from unittest.mock import patch

from media import Capture, FORMATS, MODES, MAX_PAYLOAD, CROP_PREFIX
from framing import Receiver


def decode(ffmpeg, codec, payload):
    fmt = FORMATS[codec]
    demux = {"jpeg": ["-f", "mjpeg"], "h264-intra": ["-f", "h264"],
             "cinepak-intra": ["-f", "image2pipe", "-video_size", "960x540",
                               "-vcodec", "cinepak", "-frame_size", str(len(payload))]}
    result = subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", *demux[codec],
                             "-i", "pipe:0", "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "gray",
                             "pipe:1"], input=payload, capture_output=True, timeout=5, check=True)
    if len(result.stdout) != fmt.width * fmt.height:
        raise AssertionError(f"{codec} did not decode at the negotiated extent")
    return result.stdout


def verify_picture(pixels, codec, factor, monitor, index):
    fmt = FORMATS[codec]
    width, height, dpi = monitor
    cx = ((width * 96 // dpi // 2 + 16) * dpi + 48) // 96
    cy = ((height * 96 // dpi // 2 - 8) * dpi + 48) // 96
    if index == 1:
        cx, cy = width - 1, height - 1
    elif index == 2:
        cx, cy = 0, 0

    def origin(cursor, extent, crop):
        if extent < crop:
            return -((crop - extent) // 2)
        return max(0, min(extent - crop, cursor - crop // 2))

    left = origin(cx, width, fmt.width * factor // 2)
    top = origin(cy, height, fmt.height * factor // 2)
    errors = []
    for y in range(5, fmt.height, 19):
        for x in range(5, fmt.width, 19):
            sx, sy = left + x * factor // 2, top + y * factor // 2
            # Lossy transform blocks and antialiasing can straddle color/padding edges.
            if (min(sx % 64, 63 - sx % 64, sy % 64, 63 - sy % 64) < 12 or
                    min(abs(sx), abs(sy), abs(width - sx), abs(height - sy)) < 12):
                continue
            expected = 24 + 32 * ((sx // 64 + 3 * (sy // 64)) % 7) if (
                0 <= sx < width and 0 <= sy < height) else 0
            value = pixels[y * fmt.width + x]
            errors.append(abs(value - expected))
    errors.sort()
    assert len(errors) > 100
    # A Cinepak palette entry can average rare blocks that fall on a median-cut boundary.
    if sum(errors) / len(errors) > 3 or errors[len(errors) * 95 // 100] > 8:
        raise AssertionError(f"{codec} {monitor} cursor {index} has mean luma error "
                             f"{sum(errors) / len(errors):.2f}, p95 {errors[len(errors) * 95 // 100]}")
    return len(errors)


def device_results(check, codec, stream, dims, pairs):
    data = bytearray()
    for text, frame in pairs:
        data += struct.pack("<HI",len(text),len(frame)) + text + frame
    proc = subprocess.run([check,str(FORMATS[codec].kind),str(stream),*map(str,dims)],
                          input=data, capture_output=True,check=True,timeout=5)
    return proc.stdout.decode("ascii").splitlines()


def verify_crop(check, codec, factor, layout, monitor, index, image, record, stream=21):
    from json import dumps
    mw,mh,dpi=monitor
    ox,oy={"landscape":(-1440,-180),"portrait":(1920,-250),"small":(-399,350)}[layout]
    cx=((mw*96//dpi//2+16)*dpi+48)//96
    cy=((mh*96//dpi//2-8)*dpi+48)//96
    if index==1: cx,cy=mw-1,mh-1
    if index==2: cx,cy=0,0
    cw,ch=FORMATS[codec].width*factor//2,FORMATS[codec].height*factor//2
    w,h=min(mw,cw),min(mh,ch)
    left,top=max(0,min(mw-w,cx-cw//2)),max(0,min(mh-h,cy-ch//2))
    px,py=(cw-w)//2,(ch-h)//2
    edge=int(w<cw or h<ch or left!=cx-cw//2 or top!=cy-ch//2)
    expected=[ox+cx,oy+cy,ox+left,oy+top,w,h,px,py,edge]
    raw=bytes.fromhex(record)
    assert len(raw)==48 and CROP_PREFIX.unpack(raw[:32])==(b"KCM1",*expected,bytes(3))
    assert struct.unpack_from("<IIII",raw,32)==(stream,index,struct.unpack_from("<I",image,28)[0],zlib.crc32(raw[:44]))
    text=dumps({"type":"CROP1","data":record},separators=(",",":")).encode()
    result=device_results(check,codec,stream,(cw,ch),[(text,image)])
    assert result==[" ".join(map(str,expected))],(result,expected)


async def refused(awaitable):
    try:
        await awaitable
    except (PermissionError, EOFError, BrokenPipeError):
        return
    raise AssertionError("A revoked or unavailable source produced output")


async def run(surface, ffmpeg, driver, crop_check=None):
    if surface.name.lower() != "kvmpad-test-surface.exe" or not surface.is_file():
        raise ValueError("Supply the separately built kvmpad-test-surface.exe fixture")
    # The address-coded fixture never instantiates a Windows capture session or opens a display.
    # Its HARDWARE branch rejects software adapters. WARP remains available for headless CI.
    subprocess.run([str(surface), "--coordinate-proof", driver], check=True, timeout=90)
    layouts = {"landscape": (1440, 1080, 144), "portrait": (900, 1500, 120), "small": (399, 301, 96)}
    for codec, fmt in FORMATS.items():
        count = raw = landmarks = peak = 0
        elapsed = 0
        crop_count = 0
        for layout, monitor in layouts.items():
            for mode, factor in MODES.items():
                capture = Capture(surface, ffmpeg)
                receiver = Receiver(codec, 21)
                try:
                    with patch.dict(os.environ, KVMPAD_TEST_LAYOUT=layout):
                        label = await capture.open(codec, mode)
                    assert label == r"\\.\DISPLAY42"
                    for seq in range(3):
                        start = time.monotonic()
                        if crop_check:
                            message, record = await capture.frame_with_crop(21, seq)
                        else:
                            message = await capture.frame(21, seq)
                        elapsed += time.monotonic() - start
                        if crop_check:
                            verify_crop(crop_check, codec, factor, layout, monitor, seq, message, record)
                            crop_count += 1
                        payload = receiver.receive(message)
                        assert 0 < len(payload) <= MAX_PAYLOAD
                        peak = max(peak, len(payload))
                        landmarks += verify_picture(decode(ffmpeg, codec, payload), codec, factor, monitor, seq)
                        raw += fmt.width * fmt.height  # the native oracle checked every pre-encode pixel
                        count += 1
                    await refused(capture.frame_with_crop(21, 3) if crop_check else capture.frame(21, 3))  # cursor entered a different display
                finally:
                    await capture.close()
        print(f"{codec}: {count} frames, {raw} raw pixels, {landmarks} decoded samples, "
              f"{peak} peak payload bytes, {count / elapsed:.2f} HOST frames/s")

        if crop_count:
            print(f"{codec}: {crop_count} native picker-fixture metadata records matched the C device parser and image CRC.")

        for failure in ("denied", "unidentified", "resized", "revoked"):
            capture = Capture(surface, ffmpeg)
            try:
                with patch.dict(os.environ, KVMPAD_TEST_LAYOUT=failure):
                    if failure in ("denied", "unidentified"):
                        await refused(capture.open(codec, "PIXEL_1X"))
                        await refused(capture.frame_with_crop(21, 0) if crop_check else capture.frame(21, 0))
                    else:
                        await capture.open(codec, "PIXEL_1X")
                        await refused(capture.frame_with_crop(21, 0) if crop_check else capture.frame(21, 0))
                await capture.wait_closed()
                assert capture.process.returncode != 0
                assert capture.process.stdout.read() == b""
            finally:
                await capture.close()
        print(f"{codec}: denied, unidentified display, changed extent, post-encode grant check and monitor exit passed")

    capture = Capture(surface, ffmpeg)
    entered, finish = asyncio.Event(), asyncio.Event()
    original = capture.encode_h264

    async def revoke_after_encode(pixels):
        packet = await original(pixels)
        entered.set()
        await finish.wait()
        return packet

    try:
        with patch.dict(os.environ, KVMPAD_TEST_LAYOUT="landscape"):
            await capture.open("h264-intra", "PIXEL_1X")
        with patch.object(capture, "encode_h264", revoke_after_encode):
            pending = asyncio.create_task(capture.frame_with_crop(21, 0) if crop_check else capture.frame(21, 0))
            await asyncio.wait_for(entered.wait(), 5)
            capture.revoke()
            finish.set()
            await refused(pending)
        await refused(capture.frame(21, 1))
        await refused(capture.open("h264-intra", "PIXEL_1X"))
    finally:
        await capture.close()
    print("Revocation after FFmpeg encoded an IDR suppressed the message and expired the grant.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--surface", type=Path, required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--crop-check", type=Path, help="Native receiver built from test/live_view/crop_wire_check.c")
    parser.add_argument("--driver", choices=("warp", "hardware"), default="warp",
                        help="D3D11 render backend for the address oracle and encoder matrix")
    args = parser.parse_args()
    with patch.dict(os.environ, KVMPAD_TEST_GPU=args.driver):
        asyncio.run(run(args.surface.resolve(), args.ffmpeg, args.driver, args.crop_check))
