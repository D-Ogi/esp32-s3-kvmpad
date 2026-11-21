# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Run the actual target H.264 and heap archives against a 640x360 card file in generic Xtensa QEMU."""
import argparse
from fractions import Fraction
import hashlib
import json
import random
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
MEDIA = HERE.parents[2] / "components/live_view"
sys.path.insert(0, str(HERE.parent))
from jpeg_rom.check import rom_segments


def build(args, work):
    ff = args.ffmpeg.resolve()
    compiler = args.compiler.resolve()
    product = args.build.resolve()
    idf = args.idf.resolve()
    pico = compiler.parent.parent / "picolibc/xtensa-esp-elf/lib/esp32s3"
    (work / "esp_heap_caps.h").write_text(
        "#include <stddef.h>\n#include <stdint.h>\n"
        "enum { MALLOC_CAP_8BIT=1, MALLOC_CAP_SPIRAM=2, MALLOC_CAP_INTERNAL=4, MALLOC_CAP_DMA=8 };\n"
        "void *heap_caps_malloc(size_t, unsigned);\nvoid heap_caps_free(void *);\n"
        "size_t heap_caps_get_largest_free_block(unsigned);\nsize_t heap_caps_get_free_size(unsigned);\n")
    (work / "esp_log.h").write_text("#define ESP_LOGI(...)\n#define ESP_LOGW(...)\n")
    if not args.fragmented_live:
        shutil.copyfile(args.clip, work / "clip.mp4")
    (work / "clip.S").write_text('.section .rodata.clip,"a"\n.global clip,clip_end\n'
                                 'clip: .incbin "clip.mp4"\nclip_end:\n')
    flags = ["-Os", "-mlongcalls", "-mtext-section-literals", "-ffunction-sections", "-fdata-sections",
             "-DKVM_TEST_EXTERNAL_HEAP", "-DKVM_HAVE_FFMPEG", "-D_GNU_SOURCE",
             "-specs=" + str(product / "specs/picolibc.specs")]
    if args.fragmented_live:
        flags.append("-DKVM_TEST_FRAGMENTED_LIVE")
    if args.preview:
        flags.append("-DKVM_TEST_SOURCE_PREVIEW")
        if args.clip.suffix.lower() in (".h264", ".264"):
            flags.append("-DKVM_TEST_ANNEXB_PREVIEW")
    for path in (work, idf / "components/heap/include", MEDIA / "include", ff, ff / "src"):
        flags += ["-I", path]
    sources = [HERE / "card.c", args.decoder.resolve(), MEDIA / "h264_heap.c", work / "clip.S", HERE / "entry.S"]
    objects = []
    for src in sources:
        obj = src.stem + ".o"
        subprocess.run([compiler, *flags, "-c", src, "-o", obj], cwd=work, check=True)
        objects.append(obj)
    # The exception handler uses a separate test stack and the non-windowed calling convention.
    subprocess.run([compiler, "-Os", "-ffunction-sections", "-mabi=call0", "-mtext-section-literals",
                    "-S", HERE / "high_mul.c", "-o", "high_mul.S"], cwd=work, check=True)
    subprocess.run([compiler, "-c", "high_mul.S", "-o", "high_mul.o"], cwd=work, check=True)
    heap = product / "esp-idf/heap"
    for name in ("tlsf.c", "multi_heap.c"):
        matches = [p for p in heap.rglob(name + ".*") if p.suffix in (".obj", ".o")]
        if len(matches) != 1:
            raise ValueError(f"Expected one product {name} object, found {matches}")
        objects.append(matches[0])
    libraries = [ff / ("lib" + n) / ("lib" + n + ".a") for n in ("avformat", "avcodec", "avutil")]
    # The product demuxer's unused OS backends can emit picolibc nosys warnings at link time.
    subprocess.run([compiler, "-nostdlib", "-Wl,--gc-sections,--wrap=fseek,--wrap=ftell,--wrap=fread,--wrap=ferror",
                    "-T", HERE / "image.ld", *objects, "high_mul.o", "-Wl,--start-group", *libraries,
                    pico / "libc.a", pico / "libm.a", "-lgcc", "-Wl,--end-group", "-o", "direct.elf"],
                   cwd=work, stdout=sys.stderr, check=True)
    symbols = subprocess.check_output([compiler.with_name(compiler.name.replace("gcc", "nm")), "-n", "direct.elf"],
                                      cwd=work, text=True)
    return int(re.search(r"^(\w+) B result$", symbols, re.M)[1], 16)


def execute(args, work, addr):
    loaders = rom_segments(args.rom, work)
    # Retain the real register window spill/fill vectors. The test ELF provides
    # its own level-one exception vector to implement the two missing MUL-high opcodes.
    rom = work / "rom-40000000.bin"
    rom.write_bytes(rom.read_bytes()[:0x280])
    with subprocess.Popen([args.qemu, "-M", "sim", "-cpu", "de233_fpu", "-m", "1030M", "-display", "none",
                           "-serial", "none", "-nic", "none", "-qmp", "stdio", "-S", *loaders,
                           "-device", "loader,file=direct.elf,cpu-num=0"],
                          cwd=work, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True) as proc:
        def query(op, **kwargs):
            proc.stdin.write(json.dumps({"execute": op, "arguments": kwargs}) + "\n")
            proc.stdin.flush()
            while line := proc.stdout.readline():
                reply = json.loads(line)
                if "return" in reply:
                    return reply["return"]
                if "error" in reply:
                    raise RuntimeError(reply)
            raise RuntimeError("QEMU exited")

        try:
            version = json.loads(proc.stdout.readline())["QMP"]["version"]["package"]
            query("qmp_capabilities")
            query("cont")
            end = time.monotonic() + 60
            while time.monotonic() < end:
                dump = query("human-monitor-command", **{"command-line": f"xp /17wx 0x{addr:x}"})
                words = [int(w, 16) for line in dump.splitlines()
                         for w in re.findall(r"0x[0-9a-f]+", line.partition(":")[2])]
                if words and words[0] == 0x4d454d31:
                    return version, words
                time.sleep(0.1)
            raise RuntimeError(query("human-monitor-command", **{"command-line": "info registers"}))
        finally:
            proc.kill()


def run(args):
    work = args.work.resolve()
    work.mkdir(exist_ok=True, parents=True)
    if args.fragmented_live:
        packets = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-show_packets",
                             "-show_entries", "packet=pos,size", "-of", "json", args.clip]))["packets"]
        data = args.clip.read_bytes()
        units = [data[int(p["pos"]):int(p["pos"])+int(p["size"])] for p in packets]
        if len(units) != 3:
            raise ValueError("Expected the three independent 512x512 synthetic AVI pictures")
        raw = random.Random(20261119).randbytes(512 * 512 * 3 // 2)
        for qp in range(24, 52):
            dense = subprocess.check_output(["ffmpeg", "-v", "error", "-f", "rawvideo", "-pix_fmt", "yuv420p",
                    "-s", "512x512", "-i", "-", "-frames:v", "1", "-c:v", "libx264", "-threads", "1",
                    "-profile:v", "baseline", "-refs", "1", "-qp", str(qp), "-f", "h264", "-"], input=raw)
            if 81920 <= len(dense) <= 98272: break
        else:
            raise ValueError("Could not generate dense IDR")
        units.append(dense)
        golden = [subprocess.check_output(["ffmpeg", "-v", "error", "-f", "h264", "-i", "-", "-frames:v", "1",
                  "-pix_fmt", "yuv420p", "-f", "rawvideo", "-"], input=u) for u in units]
        oracle = []
        with (work / "clip.mp4").open("wb") as output:
            for i in range(8):
                u = units[i % 4]
                capacity = 98304 - len(u) - 8
                count = capacity - capacity // 256
                while count + count // 255 > capacity: count -= 1
                packet = b"\0\0\0\1\6\5" + b"\xff"*(count//255) + bytes([count % 255]) + b"X"*count + b"\x80" + u
                output.write(struct.pack("<I",len(packet)) + packet)
                oracle.append(golden[i % 4])
                if i: oracle.append(golden[(i - 1) % 4])
        expected = hashlib.sha256(b"".join(oracle) * 2)
        version, words = execute(args, work, build(args, work))
        digest = struct.pack("<8I", *words[4:12]).hex()
        if words[1:4] != [1, 30, 0] or digest != expected.hexdigest():
            raise AssertionError((words, digest, expected.hexdigest()))
        print(json.dumps({"qemu": version, "live_opens": 2, "file_opens_after_live": 2,
              "frames": 16, "compared_yuv_pictures": 30, "heap_and_wipe_errors": words[3],
              "dense_bytes": len(dense), "outer_free": 1985008, "largest": 1638400,
              "arena_bytes": 1835008, "minimum_free_lower_bound": words[12], "yuv_sha256": digest}, indent=2))
        return
    stream = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-select_streams", "v:0",
                        "-show_entries", "stream=width,height,nb_frames,avg_frame_rate", "-of", "json", args.clip]))["streams"][0]
    raw_preview = args.preview and args.clip.suffix.lower() in (".h264", ".264")
    if not raw_preview and (stream["width"], stream["height"]) != (640, 360):
        raise ValueError("Expected a 640x360 card clip")
    version, words = execute(args, work, build(args, work))
    preview = {}
    if args.preview:
        capacity = 524288 if raw_preview else 262144
        rate = 0 if raw_preview else int(1000 * Fraction(stream["avg_frame_rate"]) + Fraction(1, 2))
        if (words[13] != rate or not 0 < words[14] < capacity or
                words[15:17] != [stream["width"], stream["height"]]):
            raise AssertionError(("header preview", words[13:17]))
        preview = {"preview_fps_milli": words[13], "preview_arena_bytes": capacity,
                   "preview_minimum_free_bytes": words[14], "width": words[15], "height": words[16]}
    if raw_preview:
        if words[1:4] != [1, 0, 0]:
            raise AssertionError(("Annex B inspection", words))
        print(json.dumps({"qemu": version, "file": args.clip.name, "heap_errors": words[3], **preview}, indent=2))
        return
    frames = args.expected_prefix if args.expected_prefix is not None else int(stream["nb_frames"])
    raw = subprocess.check_output(["ffmpeg", "-v", "error", "-i", args.clip, "-map", "0:v:0", "-frames:v",
                                   str(frames), "-pix_fmt", "yuv420p", "-fps_mode", "passthrough",
                                   "-f", "rawvideo", "-"])
    expected = hashlib.sha256(raw).hexdigest()
    digest = struct.pack("<8I", *words[4:12]).hex()
    if words[1:4] != [2 if args.expected_prefix is not None else 1, frames, 0] or digest != expected:
        raise AssertionError((words, digest, expected))
    print(json.dumps({"qemu": version, "file": args.clip.name, "result": words[1], "frames": frames, **preview,
                      "heap_errors": words[3], "arena_bytes": 1802240, "minimum_free_bytes": words[12],
                      "native_yuv420p_sha256": digest}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("clip", "ffmpeg", "compiler", "build", "idf", "rom", "work"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--decoder", type=Path, default=MEDIA / "h264_decode.c")
    parser.add_argument("--expected-prefix", type=int, help="Expect OOM after these frames in a previous build")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--fragmented-live", action="store_true", help="Use unit 1's fragmented PSRAM and a live IDR AVI")
    mode.add_argument("--preview", action="store_true", help="Inspect bounded source metadata; MP4 also runs full playback")
    parser.add_argument("--qemu", default="qemu-system-xtensa")
    run(parser.parse_args())
