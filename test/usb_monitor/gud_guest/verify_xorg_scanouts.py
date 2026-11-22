# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Replay the three Xinerama desktop observations against independent board scanouts."""

import argparse
import hashlib
import json
import struct
from pathlib import Path

from ack_scanout import read_scanout


POSITIONS = {"primary": (20, 32), "first": (877, 101), "second": (1184, 273)}


def verify_root(header, size, digest, x, y):
    fields = struct.unpack(">25I", bytes.fromhex(header))
    if (fields[1:8] != (7, 2, 24, 1440, 600, 0, 0) or fields[11:13] != (32, 5760)
            or fields[14:17] != (0xff0000, 0x00ff00, 0x0000ff)
            or int(size) != fields[0] + 12 * fields[19] + 600 * 5760):
        raise ValueError("Unexpected root window dump format")
    desktop = bytearray(600 * 5760)
    for row in range(y, y + 120):
        start = row * 5760 + x * 4
        desktop[start:start + 640] = bytes.fromhex("ffffff00") * 160
    if hashlib.sha256(desktop).hexdigest() != digest:
        raise ValueError("The unified desktop did not contain the expected window")


def verify(work):
    lines = [line.strip() for line in (work / "guest.log").read_text().splitlines()]
    heads = [line for line in lines if line.startswith("head #")]
    if heads != ["head #0: 800x600 @ 0,0", "head #1: 640x480 @ 800,0"]:
        raise ValueError("Expected distinct adjacent Xinerama heads")
    records = []
    events = iter(line for line in lines if line.startswith(
        ("GUD_X_WINDOW ", "GUD_X_ROOT ", "GUD_X_READY ", "GUD_X_COMPLETE")))
    xid = None
    for phase, (x, y) in POSITIONS.items():
        window = next(events, "").split()
        if len(window) != 5 or window[0] != "GUD_X_WINDOW" or window[2:] != [phase, str(x), str(y)]:
            raise ValueError(f"Window did not move to the expected position in {phase}")
        if not window[1].isdigit() or (xid is not None and window[1] != xid):
            raise ValueError("Expected the same X window throughout the run")
        xid = window[1]
        root = next(events, "").split()
        if len(root) != 5 or root[:2] != ["GUD_X_ROOT", phase]:
            raise ValueError(f"Missing root window observation for {phase}")
        verify_root(*root[2:], x, y)
        if next(events, "") != f"GUD_X_READY {phase}":
            raise ValueError(f"Missing stable desktop for {phase}")
        raw = (work / f"{phase}.log").read_bytes()
        rows, crc = read_scanout(raw, 1, 1024, 600)
        for row, pixels in enumerate(rows[84:564]):
            for col, pixel in enumerate(pixels[192:832]):
                white = x <= 800 + col < x + 160 and y <= row < y + 120
                if pixel != 65535 * white:
                    raise ValueError(f"{phase}: LCD pixel at ({col},{row}) differs")
        records.append(dict(phase=phase, xid=xid, compared=307200, crc=crc,
                            root_sha256=root[4], capture_sha256=hashlib.sha256(raw).hexdigest()))
    if list(events) != ["GUD_X_COMPLETE"]:
        raise ValueError("Expected completion after all three observations")
    return records


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("work", type=Path)
    print(json.dumps(verify(parser.parse_args().work), indent=2))
