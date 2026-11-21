# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Observe optimized target teardown calls immediately before free in offline Xtensa QEMU."""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
JPEG = HERE.parent / "jpeg_rom"
sys.path.insert(0, str(HERE.parent))
from jpeg_rom.check import rom_segments


def run(args):
    work = args.work.resolve()
    build = args.build.resolve()
    work.mkdir(parents=True, exist_ok=True)
    rom = rom_segments(args.rom, work)
    database = json.loads((build / "compile_commands.json").read_text())
    obj = []
    for source, variant in (("main/media_panel.c", "PANEL"), ("components/kvmbridge/bridge.c", "BRIDGE"),
                            ("components/live_view/h264_heap.c", "HEAP"),
                            ("components/live_view/media_decode.c", "DRIVER"),
                            ("components/live_view/media_decode.c", None)):
        entry = next(e for e in database if Path(e["file"]).as_posix().endswith("/" + source))
        cmd = entry["command"].rsplit(" -o ", 1)[0]
        output = work / ((variant or "decoder") + ".o")
        extra = ["-O" + args.opt, "-mtext-section-literals", "-fno-stack-protector",
                 "-Wno-unused-variable", "-Wno-unused-function", "-Wno-unused-value",
                 "-I", str(Path(entry["file"]).parent)]
        if variant:
            extra += ["-DTEST_" + variant]
        extra += ["-o", str(output), "-c", str(HERE / "release.c") if variant else entry["file"]]
        subprocess.run(cmd + " " + subprocess.list2cmdline(extra) if os.name == "nt" else shlex.split(cmd) + extra,
                       cwd=entry["directory"], check=True)
        obj.append(output)
    gcc = Path(re.match(r'^"([^"]+)"|(\S+)', cmd).group().strip('"'))
    prefix = str(gcc.parent / gcc.name.split("gcc")[0])
    crypto = build / "esp-idf/mbedtls/mbedtls/tf-psa-crypto/core/libtfpsacrypto.a"
    libc = gcc.parent.parent / "picolibc/xtensa-esp-elf/lib/esp32s3/libc.a"
    (work / "image.ld").write_text("""
ENTRY(_start)
PHDRS { code PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
 . = 0x40100000; .text : { *(.literal*) *(.text.start) *(.text*) } :code
 .rodata : { *(.rodata*) } :code
 . = 0x40210000; .data : { *(.data*) } :data
 .bss : { *(.bss*) *(COMMON) } :data
}
""")
    subprocess.run([gcc, "-nostdlib", "-Wl,--gc-sections,--wrap=free,--wrap=heap_caps_free",
                    "-T", "image.ld", JPEG / "start.S", *obj, crypto, libc, "-lgcc", "-o", "wipe.elf"],
                   cwd=work, check=True)
    symbols = subprocess.check_output([prefix + "nm", "-n", "wipe.elf"], cwd=work, text=True)
    addr = int(re.search(r"^(\w+) [Bb] result$", symbols, re.M)[1], 16)
    for name in ("explicit_bzero", "mbedtls_platform_zeroize"):
        assert re.search(r" [Tt] " + name + "$", symbols, re.M), name
    with subprocess.Popen([args.qemu, "-M", "sim", "-cpu", "de233_fpu", "-m", "1030M",
                           "-display", "none", "-serial", "none", "-nic", "none", "-S", "-qmp", "stdio",
                           *rom, "-device", "loader,file=wipe.elf,cpu-num=0"],
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE, cwd=work, text=True) as proc:
        def query(name, **kwargs):
            proc.stdin.write(json.dumps({"execute": name, "arguments": kwargs}) + "\n")
            proc.stdin.flush()
            while line := proc.stdout.readline():
                r = json.loads(line)
                if "error" in r:
                    raise RuntimeError(r)
                if "return" in r:
                    return r["return"]
            raise RuntimeError("QEMU exited")

        try:
            version = json.loads(proc.stdout.readline())["QMP"]["version"]
            query("qmp_capabilities")
            query("cont")
            end = time.monotonic() + 10
            while time.monotonic() < end:
                dump = query("human-monitor-command", **{"command-line": f"xp /16wx 0x{addr:x}"})
                words = [int(w, 16) for line in dump.splitlines()
                         for w in re.findall(r"0x[0-9a-f]+", line.partition(":")[2])]
                if words and words[0] == 0x57495045:
                    break
                time.sleep(0.05)
            else:
                raise RuntimeError((words, query("human-monitor-command", **{"command-line": "info registers"})))
        finally:
            proc.kill()
    assert words[1:5] == [0x7ff, 0x400, 0, 0], words
    assert all(words[5:]), words
    print(json.dumps({"qemu": version, "opt": args.opt, "verified_at_release": dict(zip(
        ["rgb0", "rgb1", "input0", "input1", "band", "jpeg", "arena", "wire", "worker", "paced_wire"],
        words[5:15], strict=True)), "dirty_positive_control": words[15], "guard_errors": words[3],
        "invalid_releases": words[4]}, indent=2))


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    for option in ("build", "rom", "work"):
        p.add_argument("--" + option, required=True, type=Path)
    p.add_argument("--opt", choices=("s", "3"), default="s")
    p.add_argument("--qemu", default="qemu-system-xtensa")
    run(p.parse_args())
