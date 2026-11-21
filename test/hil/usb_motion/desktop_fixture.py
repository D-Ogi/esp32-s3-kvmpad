# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Generate the colored-window motion fixture using Pillow."""
from io import BytesIO
from pathlib import Path
from struct import pack
from PIL import Image, ImageDraw


def window_scene(width, height):
    scene = Image.new("RGB", (width, height), "#193657")
    draw = ImageDraw.Draw(scene)
    for y in range(0, height, 2):
        draw.rectangle((0, y, width, y + 1), fill=(25, 48 + y * 65 // height, 82 + y * 80 // height))
    draw.rectangle((width//6, 64, width-40, height-48), fill="#edf2f4")
    draw.rectangle((width//6, 64, width-40, 101), fill="#2b5266")
    for y in range(128, height-72, 22):
        draw.rectangle((width//4, y, width-80-y%99, y+3), fill="#577281")
    return scene


def textured_scene(width, height):
    # A repeatable background with few flat horizontal runs, kept within the wire limit.
    small = Image.new("RGB", (64, 40))
    small.putdata([((x*13+y*19)%256, (x*37+y*7)%256, (x*29+y*23)%256)
                   for y in range(small.height) for x in range(small.width)])
    return small.resize((width, height), Image.Resampling.BILINEAR)


def pair(width, height, second_y, background=window_scene):
    scene = background(width, height)
    draw = ImageDraw.Draw(scene)
    positions = [(32, 32), (480, second_y)]
    for x,y in [*positions,(112,112)]:
        draw.rectangle((x-16,y-16,x+31,y+31),fill="black")
    for frame, (x,y) in enumerate(positions, 1):
        marked = scene.copy()
        ImageDraw.Draw(marked).rectangle((x,y,x+15,y+15), fill="white")
        buf = BytesIO()
        marked.save(buf, format="JPEG", quality=86, subsampling=2, optimize=True)
        jpeg = buf.getvalue()
        assert len(jpeg) <= 96*1024
        yield pack("<HBBHHHHI",0,3,0,0,0,width,height,len(jpeg)<<10 | frame) + jpeg


if __name__ == "__main__":
    data = b"".join(record for second_y in (288, 32) for size in [(640,480),(1024,600)]
                    for record in pair(*size, second_y))
    data += b"".join(pair(1024, 600, 32, textured_scene))
    Path(__file__).with_name("desktop.bin").write_bytes(data)
