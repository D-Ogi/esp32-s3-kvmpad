# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Compile the native hash driver, measured allocator and optional LVGL presenter check."""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
TEST = ROOT / "test/live_view"


def build(args):
    out = args.out.resolve()
    out.mkdir(exist_ok=True, parents=True)
    ff = args.ffmpeg.resolve()
    src = ROOT / "components/live_view"
    flags = ["-std=gnu11", "-O2", "-DKVM_HAVE_FFMPEG", "-I", src / "include", "-I", ff, "-I", ff / "src"]
    sources = [src / n for n in ("h264_decode.c", "media_decode.c", "cinepak_tile.c", "live_queue.c", "live_message.c")]
    libs = [ff / ("lib" + l) / ("lib" + l + ".a") for l in ("avformat", "avcodec", "avutil")]
    libs += ["-lbcrypt", "-lsecur32", "-lws2_32"] if os.name == "nt" else ["-lm", "-lpthread"]
    exe = ".exe" if os.name == "nt" else ""
    shared = ".dll" if os.name == "nt" else ".so"

    def cc(*command):
        subprocess.run([args.cc, *command], check=True)

    if args.motion_test:
        # This test includes the pinned decoder translation unit to reach its private fallback.
        program = out / ("second-mv-list" + exe)
        cc(*flags, "-DHAVE_AV_CONFIG_H", "-DBUILDING_avcodec", TEST / "second_mv_list.c",
           *libs, "-o", program)
        subprocess.run([program], check=True)

    # Override libavutil's allocator for both the frame driver and live decoder.
    # All other objects use the very same archive as the production decoder.
    cfg = (ff / "config.h").read_text()
    for option in ("HAVE_POSIX_MEMALIGN", "HAVE_MEMALIGN", "HAVE_ALIGNED_MALLOC"):
        cfg = cfg.replace("#define " + option + " 1", "#define " + option + " 0")
    (out / "config.h").write_text(cfg + "\n#define MALLOC_PREFIX kvm_probe_\n", encoding="utf8")
    mem = out / "probe_mem.o"
    cc("-fPIC", "-std=gnu11", "-O2", "-I", out, "-I", ff / "src", "-I", ff,
       "-c", ff / "src/libavutil/mem.c", "-o", mem)
    probe = [TEST / "allocation_probe.c", mem]
    cc(*flags, "-DKVM_MEASURE_ALLOCATIONS", TEST / "h264_frames.c", *sources, *probe, *libs,
       "-o", out / ("h264_frames" + exe))
    cc(*flags, "-fPIC", "-shared", *sources, *probe, *libs, "-o", out / ("native-codecs" + shared))
    if args.tlsf:
        tlsf = args.tlsf.resolve()
        bounded = ["-I", tlsf / "include", *sources, TEST / "retained_heap.c", tlsf / "tlsf.c", mem, *libs]
        cc(*flags, "-fPIC", "-shared", *bounded, "-o", out / ("bounded-codecs" + shared))
        cc(*flags, "-DKVM_TEST_BOUNDED_HEAP", TEST / "h264_frames.c", *bounded,
           "-o", out / ("bounded-frames" + exe))

    paged = []
    if args.paged_image:
        directory = ROOT / "tools/live_view/paged_h264"
        zlib = ["-idirafter", args.zlib_include.resolve()] if args.zlib_include else []
        zlib += [args.zlib.resolve() if args.zlib else "-lz"]
        extra = ["-I", directory, directory / "paged_video.c", *zlib]
        cc(*flags, TEST / "paged_frames.c", *extra, libs[2], *libs[3:], "-o", out / ("paged_frames" + exe))
        if args.lvgl:
            # Uses the same start/end link symbols as IDF target_add_binary_data.
            blob = out / "guest.S"
            section = '.section .rdata,"dr"' if os.name == "nt" else '.section .rodata'
            path = json.dumps(args.paged_image.resolve().as_posix(), ensure_ascii=False)
            blob.write_text(section + '\n.global _binary_kvm_paged_decoder_start\n'
                            '.global _binary_kvm_paged_decoder_end\n_binary_kvm_paged_decoder_start:\n'
                            + f'.incbin {path}\n_binary_kvm_paged_decoder_end:\n', encoding="utf8")
            paged = ["-DKVM_MEDIA_PAGED_GUEST", blob, *extra]
    if args.lvgl:
        # Only the firmware OS/board adapters are replaced. Use the simulator's
        # actual LVGL binary/configuration and the unmodified panel source.
        for header in ("board_lcd.h", "esp_log.h", "esp_heap_caps.h", "esp_timer.h", "esp_rom_crc.h", "multi_heap.h",
                       "freertos/FreeRTOS.h", "freertos/semphr.h", "freertos/task.h", "freertos/idf_additions.h"):
            shim = out / "platform" / header
            shim.parent.mkdir(parents=True, exist_ok=True)
            shim.write_text('#include "panel_test_platform.h"\n', encoding="ascii")
        hid = []
        if args.hid_load:
            components = ROOT / "components"
            core = components / "hid_core"
            usb = components / "usb_hid_device"
            includes = ["-I", core / "include", "-I", usb / "include", "-I", ROOT / "test/host_unit"]
            shims = out / "hid-platform"
            for header in ("esp_err.h", "esp_timer.h", "esp_check.h", "esp_log.h", "freertos/FreeRTOS.h",
                           "freertos/queue.h", "freertos/semphr.h"):
                shim = shims / header
                shim.parent.mkdir(parents=True, exist_ok=True)
                shim.write_text('#include "hid_test_platform.h"\n', encoding="ascii")
            objects = []
            for source in (core / "src/hid_dispatch.c", TEST / "panel_hid_load.c"):
                obj = out / (source.stem + ".o")
                cc(*flags, "-DKVM_TEST_MEDIA_HID", "-fsanitize=address", "-fno-omit-frame-pointer",
                   "-DLV_CONF_INCLUDE_SIMPLE", "-DLV_KCONFIG_IGNORE", "-I", shims, "-I", TEST, *includes,
                   "-I", ROOT / "main", "-I", ROOT / "managed_components/lvgl__lvgl",
                   "-I", ROOT / "simulator/lvgl_sdl", "-c", source, "-o", obj)
                objects.append(obj)
            hid = ["-DKVM_TEST_MEDIA_HID", "-Wl,--wrap,kvm_media_h264_picture", *objects, *includes,
                   *(core / "src" / f for f in ("hid_core.c", "hid_keymap.c", "kbd_fsm.c")),
                   usb / "src/usb_hid_reports.c"]
        cc(*flags, *paged, *hid, "-fsanitize=address", "-fno-omit-frame-pointer", "-DKVM_MEDIA_SYNTHETIC_DIAG",
           "-DKVM_TEST_EXTERNAL_HEAP", "-DLV_CONF_INCLUDE_SIMPLE",
           "-DLV_KCONFIG_IGNORE", "-I", out / "platform", "-I", TEST, "-I", ROOT / "main",
           "-I", ROOT / "components/kvmbridge/include", "-I", ROOT / "components/ui_theme/include",
           ROOT / "components/ui_theme/src/ui_theme.c", ROOT / "components/ui_theme/src/kvm_control_icons.c", "-I", ROOT / "managed_components/lvgl__lvgl",
           "-I", ROOT / "simulator/lvgl_sdl", TEST / "panel_test.c", ROOT / "main/media_panel.c",
           ROOT / "test/ui_components/host_lv_mem.c", ROOT / "simulator/lvgl_sdl/sim_image.c",
           ROOT / "managed_components/lvgl__lvgl/src/libs/lodepng/lodepng.c",
           src / "h264_heap.c", *sources, args.lvgl.resolve(), *libs,
           "-lpthread", "-o", out / ("panel_test" + exe))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg", required=True, type=Path, help="host output of build_ffmpeg.py")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--motion-test", action="store_true", help="Check the patched decoder's second-list detachment")
    parser.add_argument("--tlsf", type=Path, help="ESP-IDF components/heap/tlsf directory for bounded live admission")
    parser.add_argument("--lvgl", type=Path, help="ASan-enabled liblvgl_host.a from the KVMpad simulator")
    parser.add_argument("--hid-load", action="store_true", help="Include the mixed HID/media presenter regression (requires --lvgl)")
    parser.add_argument("--paged-image", type=Path, help="Build paged frame driver and optional guarded panel test")
    parser.add_argument("--zlib", type=Path, help="Host zlib archive when unavailable via -lz")
    parser.add_argument("--zlib-include", type=Path, help="Host zlib headers outside compiler search path")
    build(parser.parse_args())
