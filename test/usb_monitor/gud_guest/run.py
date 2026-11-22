# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Build the production receiver into a minimal guest, boot TCG and check serial output."""

import argparse
import gzip
import os
import stat
import subprocess
from pathlib import Path


def build(out, work, clang, qemu, target=None, virtual=True):
    here = Path(__file__).resolve().parent
    component = here.parents[2] / "components/usb_monitor"
    inc = out / "sysroot/usr/include"
    lib = out / "sysroot/usr/lib/x86_64-linux-musl"
    env = dict(os.environ, TMP=str(work), TEMP=str(work), TMPDIR=str(work))
    subprocess.run(
        [
            clang,
            "--target=x86_64-linux-musl",
            "-fuse-ld=lld",
            "-nostdinc",
            "-nostdlib",
            "-static",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-std=gnu11",
            "-isystem",
            inc / "x86_64-linux-musl",
            "-isystem",
            inc,
            "-isystem",
            inc / "x86_64-linux-gnu",
            "-I",
            component / "include",
            lib / "crt1.o",
            lib / "crti.o",
            here / "guest.c",
            *([component / "gud_receiver.c"] if virtual else ["-DGUD_CLIENT_ONLY"]),
            lib / "libc.a",
            lib / "crtn.o",
            "-o",
            work / "fixture",
        ],
        check=True,
        env=env,
    )

    entries = []


    def member(path, data=b"", kind=stat.S_IFREG, mode=0o755, dev=(0, 0)):
        name = path.encode() + b"\0"
        fields = (
            len(entries) + 1,
            kind | mode,
            0,
            0,
            1,
            0,
            len(data),
            0,
            0,
            *dev,
            len(name),
            0,
        )
        header = b"070701" + b"".join(f"{v:08x}".encode() for v in fields) + name
        entries.append(
            header + b"\0" * (-len(header) % 4) + data + b"\0" * (-len(data) % 4)
        )


    for d in ("bin", "dev", "mods"):
        member(d, kind=stat.S_IFDIR)
    member("dev/console", kind=stat.S_IFCHR, mode=0o600, dev=(5, 1))
    member("bin/sh", b"busybox", stat.S_IFLNK)
    member("bin/busybox", (out / "busybox").read_bytes())
    member("init", (here / "init").read_bytes().replace(b"\r\n", b"\n"))
    member("fixture", (work / "fixture").read_bytes())
    if virtual:
        member("virtual-device", (here / "virtual-device").read_bytes().replace(b"\r\n", b"\n"))
        member("load-modules", (out / "load-modules").read_bytes().replace(b"\r\n", b"\n"))
        modules = sorted((out / "mods").glob("*.ko"))
    else:
        modules = [out / "mods" / n for n in ("lz4_compress.ko", "gud.ko")]
    if target:
        member("target", target.encode("ascii"))
        member("physical-init", (here / "physical-init").read_bytes().replace(b"\r\n", b"\n"))
    for module in modules:
        member("mods/" + module.name, module.read_bytes())
    member("TRAILER!!!")
    initrd = work / "initrd.gz"
    initrd.write_bytes(gzip.compress(b"".join(entries), compresslevel=3, mtime=0))
    cmd = [
        qemu,
        "-machine",
        "pc,accel=tcg",
        "-cpu",
        "max",
        "-smp",
        "2",
        "-m",
        "480",
        "-nodefaults",
        "-no-reboot",
        "-display",
        "none",
        "-nic",
        "none",
        "-monitor",
        "none",
        "-serial",
        "stdio",
        "-kernel",
        str(out / "vmlinuz"),
        "-initrd",
        str(initrd),
        "-append",
        "console=ttyS0,115200 rdinit=/init panic=-1 loglevel=5",
    ]
    return cmd


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("out", type=Path)
    p.add_argument("--work", type=Path, help="separate directory for the guest, initramfs and log")
    p.add_argument("--clang", default="clang")
    p.add_argument("--qemu", default="qemu-system-x86_64")
    a = p.parse_args()
    out = a.out.resolve()
    work = a.work.resolve() if a.work else out
    work.mkdir(parents=True, exist_ok=True)
    cmd = build(out, work, a.clang, a.qemu)
    with (work / "guest.log").open("wb") as log:
        p = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, timeout=100)
    text = (work / "guest.log").read_bytes().decode(errors="replace")
    print(text[-4300:])
    required = (
        "Unbound sentinel ffff:ffff",
        "Guest-only match attached gud",
        "GUD_VT_MATCH phase=1",
        "GUD_VT_MATCH phase=2",
        "DRM driver=gud",
        "rect=0,0 640x480 mismatch=0,189",
        "rect=616,471 24x9 mismatch=189,0",
        "GUD_RECONNECT fresh enumeration, retained runtime match and verified pixels",
        "GUD_RELOAD fresh module leaves the sentinel unbound",
        "GUD_DRM_MODES count=1 vga=1 svga=0",
        "GUD_DRM_MODES count=2 vga=1 svga=1",
        "GUD_LEGACY verified VGA-only offer, full and edge",
        "mode=800x600 rect=0,0 800x384",
        "mode=800x600 rect=0,384 800x216",
        "mode=800x600 rect=776,591 24x9",
        *(f"GUD_MODE_MATCH {mode}-{phase} {geom} pixels={pixels}"
          for mode, geom, pixels in (("vga", "640x480", 307200), ("svga", "800x600", 480000),
                                     ("return", "640x480", 307200)) for phase in ("full", "edge")),
        "PASS real Linux DRM GUD",
    )
    if p.returncode or any(s not in text for s in required):
        raise SystemExit("Guest qualification failed. Inspect guest.log.")
