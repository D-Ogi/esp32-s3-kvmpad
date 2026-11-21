# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Measure HOST demand over pinned loopback TLS using the native, clock-indexed WARP texture."""
import argparse
import asyncio
import ctypes
import json
from pathlib import Path
import shutil
import statistics
import struct
import time
import zlib

from test_loopback_pixels import closed, decode, loopback_host, require_fixture, send, silence
from media import CROP_PREFIX, FORMATS
from framing import Receiver


def picture(ffmpeg, codec, payload, left, top):
    pixels = decode(ffmpeg, codec, payload)
    width, height = FORMATS[codec].width, FORMATS[codec].height
    # Sample the interior of each known source block, away from lossy codec boundaries.
    errors = []
    for y in range(8, height, 17):
        for x in range(8, width, 17):
            sx, sy = left + x + 1440, top + y + 180
            if min(sx % 64, 63 - sx % 64, sy % 64, 63 - sy % 64) < 12:
                continue
            errors.append(abs(pixels[y * width + x] - 24 - 32 * ((sx // 64 + 3 * (sy // 64)) % 7)))
    assert len(errors) > 100 and statistics.mean(errors) < 3 and sorted(errors)[len(errors)*95//100] <= 8
    return len(errors)


async def run(args):
    surface = require_fixture(args.surface)
    ffmpeg = shutil.which(args.ffmpeg)
    if not ffmpeg:
        raise ValueError("FFmpeg is required")
    tick = ctypes.windll.kernel32.GetTickCount64
    tick.restype = ctypes.c_ulonglong
    period = 1440 * 1080
    report = {}
    async with loopback_host(surface, ffmpeg, "paced") as (socket, hello, consent, command):
        async def session(codec="jpeg", maximum=10):
            control, token = await hello()
            ack = await consent(control, codec, "PIXEL_1X", True, "ALLOW", maximum)
            assert ack["type"] == "STARTED"
            if maximum is None:
                assert "present_v" not in ack and "start_fps" not in ack
            else:
                assert (ack["present_v"], ack["max_fps"], ack["start_fps"]) == (1, maximum, min(5,maximum))
            data = await socket("/image")
            await send(data, type="BIND", session=token)
            return control, data, ack["stream"], Receiver(codec, ack["stream"])

        async def image(data, receiver, stream):
            record = json.loads(await asyncio.wait_for(data.recv(), 3))
            assert record["type"] == "CROP1"
            raw = bytes.fromhex(record["data"])
            msg = await asyncio.wait_for(data.recv(), 1)
            arrival = time.monotonic()
            source_now = tick() // 10
            payload = receiver.receive(msg)
            rid, seq, img_crc, checksum = struct.unpack_from("<IIII", raw, 32)
            assert (rid, seq, img_crc, checksum) == (stream, receiver.sequence, struct.unpack_from("<I",msg,28)[0],zlib.crc32(raw[:44]))
            _, x, y, left, top, *_ = CROP_PREFIX.unpack(raw[:32])
            capture_tick = x + 1440 + (y + 180)*1440
            return arrival, capture_tick, ((source_now-capture_tick)%period)*10, (payload,left,top)

        async def done(data, stream, seq, shown=True):
            await send(data, type="FRAME_DONE", stream=stream, frame=seq, shown=shown)

        ctrl, data, stream, rec = await session()
        arrivals, ages, samples = [], [], []
        for index in range(44):
            at, _, age, pic = await image(data, rec, stream)
            arrivals.append(at)
            ages.append(age)
            if index in (0, 20, 43):
                samples.append(pic)
            await done(data, stream, rec.sequence)
        start_fps = 5 / (arrivals[5] - arrivals[0])
        fast_fps = 8 / (arrivals[-1] - arrivals[-9])
        assert 4.4 < start_fps < 5.6 and 8.5 < fast_fps < 10.7, (start_fps, fast_fps)
        _, before, _, _ = await image(data, rec, stream)
        await silence(data, .36)
        await done(data, stream, rec.sequence)
        at1, after, _, _ = await image(data, rec, stream)
        source_jump = (after-before)%period * 10
        assert source_jump >= 340
        await done(data, stream, rec.sequence)
        at2, _, _, _ = await image(data, rec, stream)
        slow = at2 - at1
        assert .43 <= slow <= 1.25, slow
        await done(data, stream, rec.sequence, False)
        at3, _, _, _ = await image(data, rec, stream)
        assert .88 <= at3-at2 < 1.4, at3-at2
        frozen_seq = rec.sequence
        await send(ctrl, type="FREEZE", seq=2)
        assert json.loads(await ctrl.recv()) == dict(type="FROZEN",seq=2,last_frame=frozen_seq)
        await silence(data, .28)
        await send(ctrl, type="RESUME", seq=3)
        assert json.loads(await ctrl.recv()) == dict(type="RESUMED", seq=3)
        at4, *_ = await image(data, rec, stream)
        assert rec.sequence == frozen_seq+1
        await done(data, stream, frozen_seq)
        await silence(data,.11)
        await done(data, stream, rec.sequence)
        at5, *_ = await image(data, rec, stream)
        assert .17 <= at5-at4 <= .32
        await send(ctrl,type="STOP",seq=4)
        await asyncio.gather(closed(ctrl),closed(data))
        report["adaptive_jpeg"] = dict(initial_fps=start_fps, fast_fps=fast_fps,
            source_age_ms_p95=sorted(ages)[int(.95*(len(ages)-1))],
            source_jump_after_withheld_credit_ms=source_jump, backoff_period_s=slow,
            discarded_period_s=at3-at2, resume_period_s=at5-at4,
            sampled_lumas=sum(picture(ffmpeg,"jpeg",*p) for p in samples))

        ctrl, data, stream, rec = await session(maximum=None)
        arrivals = [(await image(data,rec,stream))[0] for _ in range(8)]
        legacy = 7/(arrivals[-1]-arrivals[0])
        assert 4.3 < legacy < 5.6
        await done(data,stream,rec.sequence)
        await asyncio.gather(closed(ctrl),closed(data))
        report["legacy_crop_only_fps"] = legacy

        for codec in ("h264-intra","cinepak-intra"):
            ctrl, data, stream, rec = await session(codec,3)
            frames = []
            ages = []
            times = []
            for _ in range(4):
                at, _, age, p = await image(data,rec,stream)
                times.append(at)
                ages.append(age)
                if not frames: frames.append(p)
                await done(data,stream,rec.sequence)
            await command("REVOKE")
            await asyncio.gather(closed(ctrl),closed(data))
            report[codec] = dict(fps=3/(times[-1]-times[0]), source_age_ms=ages,
                                 sampled_lumas=picture(ffmpeg,codec,*frames[0]))

            ctrl, data, stream, rec = await session(codec, 10)
            arrivals, ages = [], []
            for _ in range(40):
                at, _, age, _ = await image(data, rec, stream)
                arrivals.append(at)
                ages.append(age)
                await done(data, stream, rec.sequence)
            await send(ctrl, type="STOP", seq=2)
            await asyncio.gather(closed(ctrl), closed(data))
            measured = 8 / (arrivals[-1] - arrivals[-9])
            assert 1 <= measured <= 11
            report[codec].update(adaptive_tail_fps=measured, adaptive_frames=len(arrivals),
                                 adaptive_source_age_ms_p95=sorted(ages)[int(.95*(len(ages)-1))])

        ctrl, data, stream, rec = await session()
        await image(data,rec,stream)
        start = time.monotonic()
        await asyncio.wait_for(data.wait_closed(),3.8)
        await asyncio.gather(closed(ctrl),closed(data))
        elapsed = time.monotonic()-start
        assert 2.75 < elapsed < 3.65
        report["no_credit_revocation_s"] = elapsed
    print(json.dumps(report,indent=2))
    if args.report:
        args.report.write_text(json.dumps(report,indent=2)+"\n","utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--surface",type=Path,required=True)
    parser.add_argument("--ffmpeg",default="ffmpeg")
    parser.add_argument("--report",type=Path)
    asyncio.run(run(parser.parse_args()))
