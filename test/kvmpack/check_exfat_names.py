# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Check exFAT file names and card replacement with the shipped FatFs sources."""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fatfs", type=Path, required=True, help="Generated kvmpad-components/fatfs/src")
    parser.add_argument("--sdkconfig", type=Path, required=True, help="7B config/sdkconfig.h")
    parser.add_argument("--cc", required=True, help="C compiler (32-bit MinGW on Windows)")
    parser.add_argument("--work", type=Path, required=True, help="Directory for synthetic test outputs")
    args = parser.parse_args()
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    source = args.fatfs.resolve()
    config = args.sdkconfig.read_text(encoding="utf-8")
    utf8 = "#define CONFIG_FATFS_API_ENCODING_UTF_8 1"
    if utf8 not in config:
        raise ValueError("The supplied firmware configuration must enable the UTF-8 API.")

    # The filesystem, code-page conversion and on-disk structures come from IDF.
    # The fixture supplies a single-threaded memory disk and heap.
    includes = work / "platform" / "freertos"
    includes.mkdir(parents=True, exist_ok=True)
    (includes / "FreeRTOS.h").write_text(
        "#include <stddef.h>\n#ifndef MIN\n#define MIN(a,b) ((a)<(b)?(a):(b))\n"
        "#define MAX(a,b) ((a)>(b)?(a):(b))\n#endif\n", encoding="ascii")
    (includes / "semphr.h").write_text("", encoding="ascii")
    test = Path(__file__).resolve().parent
    runtime = test.parents[1] / "components/pack_runtime"
    for name in ("esp_err.h", "board_hal.h", "board_pins.h", "driver/sdmmc_host.h", "esp_vfs_fat.h",
                 "sdmmc_cmd.h", "freertos/task.h", "diskio_sdmmc.h"):
        header = includes.parent / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "card_mount_platform.h"\n', encoding="ascii")
    binaries = {}
    configs = {"utf8": config, "oem": config.replace(utf8, "#define CONFIG_FATFS_API_ENCODING_ANSI_OEM 1")}
    for profile, contents in configs.items():
        directory = work / profile
        directory.mkdir(exist_ok=True)
        (directory / "sdkconfig.h").write_text(contents, encoding="utf-8")
        binary = directory / "names.exe"
        mount = []
        if profile == "utf8":
            mount = ["-DKVM_TEST_CARD_MOUNT", "-I", test / "host", "-I", runtime / "include",
                     runtime / "pack_card_io.c", test / "host/card_mount.c"]
        subprocess.run([args.cc, "-std=c11", "-O2", "-I" + str(source), "-I" + str(directory),
                        "-I" + str(includes.parent), str(source / "ff.c"), str(source / "ffunicode.c"),
                        test / "host/exfat_names.c", *mount, "-o", str(binary)], check=True)
        binaries[profile] = binary
    image = work / "fixture.img"
    subprocess.run([binaries["utf8"], "seed", image], check=True)
    for profile in ("oem", "utf8"):
        subprocess.run([binaries[profile], "check", image], check=True)
    subprocess.run([binaries["utf8"], "remount"], check=True)


if __name__ == "__main__":
    main()
