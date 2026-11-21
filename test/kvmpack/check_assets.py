# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Independent Pillow pixel oracle and signed and unsigned KVP asset corpus."""
import io
import json
from pathlib import Path
import struct
import subprocess
import zipfile
import zlib

from PIL import Image
import kvmpack


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def rgba_png(w, h, adam7):
    passes = [(0,0,8,8),(4,0,8,8),(0,4,4,8),(2,0,4,4),(0,2,2,4),(1,0,2,2),(0,1,1,2)] if adam7 else [(0,0,1,1)]
    raw = bytearray()
    for x0, y0, dx, dy in passes:
        if x0 >= w: continue
        for y in range(y0, h, dy):
            raw.append(0)
            for x in range(x0, w, dx):
                raw.extend((x % 256, y % 256, (x ^ y) % 256, (3*x + y) % 256))
    ihdr = struct.pack(">2I5B", w, h, 8, 6, 0, 0, adam7)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


def check_assets(directory, compiler, exe, default, trusted, files, run):
    check = directory / ("png-pixels" + exe.suffix)
    host = Path(__file__).parent / "host"
    subprocess.run([*[(host/"png_check.c" if str(c).endswith("device_check.c") else c) for c in compiler],
                    "-o", check], check=True)
    pngs = [rgba_png(13, 9, True), rgba_png(1, 9, True), rgba_png(11, 1, True), rgba_png(16, 10, False)]
    static = pngs[-1]
    frame = struct.pack(">5I2H2B", 0,16,10,0,0,1,1,0,0)
    pngs.append(static[:33] + chunk(b"acTL", struct.pack(">2I",1,0)) + chunk(b"fcTL", frame) + static[33:])
    for mode in ("1", "L", "LA", "P", "RGB", "RGBA"):
        image = Image.new(mode, (23, 11))
        for y in range(image.height):
            for x in range(image.width):
                v = (x*13 + y*7) % 256
                image.putpixel((x,y), (v % 8 if mode == "P" else v) if len(mode) == 1 else
                               tuple((v+j*35)%256 for j in range(len(mode))))
        if mode == "P":
            image.putpalette(bytes((i*7)%256 for i in range(768)))
            image.info["transparency"] = bytes(range(8))
        stream = io.BytesIO()
        image.save(stream, format="PNG", bits=4 if mode == "P" else None)
        pngs.append(stream.getvalue())
    raw16 = b"\0" + struct.pack(">6H", 0x1234, 0xabcd, 0xff00, 0x2120, 0x0304, 0x5566)
    pngs.append(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">2I5B",2,1,16,2,0,0,0)) +
                chunk(b"IDAT", zlib.compress(raw16)) + chunk(b"IEND", b""))
    stream = io.BytesIO()
    Image.new("RGBA", (1024,568), (101,61,250,66)).save(stream, format="PNG")
    pngs.append(stream.getvalue())
    for raw in pngs:
        source, output = directory/"sample.png", directory/"rgba.bin"
        source.write_bytes(raw)
        with Image.open(io.BytesIO(raw)) as image:
            expected = image.convert("RGBA").tobytes()
            subprocess.run([check, source, str(image.width), str(image.height), output], check=True)
        assert output.read_bytes() == expected
    assert subprocess.run([check, source, "1024", "568", output, "64"]).returncode == 1

    path = directory/"card/assets.kvmpack"
    names = ["assets/A/photo.png", "assets/sample.png", "assets/xyz/test.a-b_7.png"]
    ui = json.loads(files["ui.json"])
    buttons = [node for node in ui["root"]["children"] if node["type"] == "Button"]
    for button in buttons:
        button["props"]["icon"] = names[0]
    icons = {**files, "ui.json":json.dumps(ui).encode(), names[1]:static}
    for raw in [*pngs, rgba_png(95, 51, True), rgba_png(1, 568, True), rgba_png(1024, 1, False)]:
        path.write_bytes(kvmpack.build({**icons, names[0]:raw}))
        with Image.open(io.BytesIO(raw)) as image:
            w, h = image.size
            limit = max(32, w, h)
            tw, th = max(1,w*32//limit), max(1,h*32//limit)
            pixels = image.convert("RGBA")
            expected = bytearray()
            for y in range(th):
                for x in range(tw):
                    r, g, b, a = pixels.getpixel((x*w//tw, y*h//th))
                    expected.extend((b, g, r, a))
        subprocess.run([check, "icon", path, str(tw), str(th), output], check=True)
        assert output.read_bytes() == expected
    for invalid in ("assets/a/photo.png", "assets/missing.png", "../photo.png", 1, None):
        buttons[0]["props"]["icon"] = invalid
        path.write_bytes(kvmpack.build({**icons, "ui.json":json.dumps(ui).encode(), names[0]:static}))
        run(default, "tap", path, False)
    buttons[0]["props"]["icon"] = names[0]
    ui["root"]["children"][0]["props"]["icon"] = names[0]  # Text
    path.write_bytes(kvmpack.build({**icons, "ui.json":json.dumps(ui).encode(), names[0]:static}))
    run(default, "tap", path, False)
    del ui["root"]["children"][0]["props"]["icon"]
    long_path = "assets/" + "a"*149 + ".png"
    for button in buttons:
        button["props"]["icon"] = long_path
    escaped = json.dumps(ui).encode().replace(b'"assets/', b'"assets\\/')
    path.write_bytes(kvmpack.build({**files, "ui.json":escaped, long_path:static}))
    run(default, "tap", path, True)

    manifest = json.loads(files["manifest.json"])
    square = rgba_png(32, 32, False)
    def minimum(refs, reverse=False):
        controls = json.loads(files["ui.json"])
        for node, ref in zip((n for n in controls["root"]["children"] if n["type"] == "Button"), refs):
            node["props"]["icon"] = ref
        lo, hi = 16, 512
        while lo < hi:
            mid = (lo+hi)//2
            manifest["limits"]["max_memory_kib"] = mid
            raw = kvmpack.build({**files, "ui.json":json.dumps(controls).encode(),
                                 "manifest.json":json.dumps(manifest).encode(), names[0]:square,
                                 names[1]:square, names[2]:pngs[-1]})
            if reverse:
                with zipfile.ZipFile(io.BytesIO(raw)) as src, zipfile.ZipFile(path,"w") as dst:
                    for name in reversed(src.namelist()):
                        dst.writestr(name,src.read(name))
            else:
                path.write_bytes(raw)
            code = subprocess.run([default,"inspect",path], stdout=subprocess.DEVNULL).returncode
            assert code in (0,1)
            if code == 0: hi = mid
            else: lo = mid+1
        return lo
    plain, shared, distinct = minimum([]), minimum([names[0]]*2), minimum(names[:2])
    assert 4 <= shared-plain <= 5 and 4 <= distinct-shared <= 5
    assert minimum(names[:2],True) == distinct
    print(f"Icon PNG load thresholds on host: plain/shared/distinct={plain}/{shared}/{distinct} KiB; reversed ZIP order matched.")
    print(f"Button thumbnails: {len(pngs)+3} pixel-exact BGRA comparisons, two references share one buffer; invalid references rejected.")
    payload = {**files, **dict(zip(names, pngs))}
    path.write_bytes(kvmpack.build(payload, trusted))
    assert "publisher=" in run(exe, "tap", path, True)
    run(default, "tap", path, False)
    path.write_bytes(kvmpack.build(payload))
    assert "unsigned" in run(default, "tap", path, True)
    subprocess.run([check, "file", path], check=True)
    path.write_bytes(kvmpack.build({**files, names[0]:pngs[-1], names[1]:pngs[-1]}))
    run(default, "tap", path, True)

    def archive(payload, override=None):
        source = io.BytesIO(kvmpack.build(payload, trusted))
        with zipfile.ZipFile(source) as z:
            entries = {name:z.read(name) for name in reversed(z.namelist())}
        entries.update(override or {})
        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as out:
            for name, data in entries.items(): out.writestr(name, data)

    archive(payload)
    run(exe, "tap", path, True)
    for name in ("assets/../a.png", "assets//a.png", "assets/com1.png", "assets/LpT2/f.png", "assets/image.PNG",
                 "assets/a b.png", "assets/a\\b.png", "assets/" + "a"*152 + ".png", "assets/SAMPLE.png"):
        archive({**payload, name:pngs[0]})
        run(exe, "tap", path, False)
    raw = pngs[0]
    bad_crc = raw[:-1] + bytes([raw[-1]^1])
    for invalid in (bad_crc, raw[:33]+chunk(b"acTL",struct.pack(">II",2,0))+raw[33:], raw[:-12], raw+b"tail",
                    raw[:33]+chunk(b"BADX", b"")+raw[33:], raw[:33]+chunk(b"a1Cb", b"")+raw[33:],
                    raw[:33]+chunk(b"abcb", b"")+raw[33:], raw[:33]+chunk(b"IDAT", b"invalid")+raw[-12:]):
        archive({**files, names[0]:invalid})
        run(exe, "tap", path, False)
        run(exe, "unsupported-data", path, True)
    changed = {**payload, names[0]:pngs[-1]}
    archive(payload, {names[0]:pngs[-1]})
    run(exe, "tap", path, False)
    archive(changed, {kvmpack.SIGNATURE:trusted.sign(kvmpack.signature_input(payload))})
    run(exe, "tap", path, False)
    many = {**files, **{f"assets/a{i}.png":pngs[0] for i in range(59)}}
    archive(many)
    run(exe, "tap", path, True)
    archive(many, {"assets/excess.png":raw})
    run(exe, "tap", path, False)
    archive(payload, {names[0]:b"\0" * (2*1024*1024)})
    run(exe, "tap", path, False)
    many["assets/u.png"] = raw
    path.write_bytes(kvmpack.build(many))
    run(default, "tap", path, True)
    print(f"PNG firmware checks passed: {len(pngs)} independent RGBA comparisons, Adam7, integrity, signature ordering, paths and 64-member limits.")
