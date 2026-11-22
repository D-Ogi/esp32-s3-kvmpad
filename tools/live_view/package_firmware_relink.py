# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Prepare separate relocatable application and LGPL library inputs from a Windows IDF release build."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import zipfile

from relink_firmware import TOOLS

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
LIBRARIES = {"libavformat.a", "libavcodec.a", "libavutil.a", "libdavid-cermak__libssh.a"}


def capture_args(args):
    for arg in args:
        if arg.startswith("@"):
            yield from capture_args(shlex.split(Path(arg[1:]).read_text()))
        else:
            yield arg


def link_command(build, compiler, work):
    ninja = (build / "build.ninja").read_text(encoding="utf-8").splitlines()
    start = next(i for i, line in enumerate(ninja) if line.startswith("build kvmpad.elf: "))
    direct = ninja[start].split(" ", 3)[3].split(" |", 1)[0]
    props = {}
    for line in ninja[start + 1:]:
        if not line.startswith("  "):
            break
        key, value = line[2:].split(" = ", 1)
        props[key] = value
    rsp = work / "driver.rsp"
    rsp.write_text(direct + " " + props["LINK_PATH"] + " " + props["LINK_LIBRARIES"])
    record = work / "driver.json"
    wrapper = ",".join([sys.executable, str(Path(__file__).resolve()), "--capture", str(record)])
    # Ask the compiler driver to expand its specs and response files. The wrapper records
    # collect2's arguments before it runs, without modifying the signed build directory.
    subprocess.run([compiler, "-wrapper", wrapper, *shlex.split(props["FLAGS"] + " " + props["LINK_FLAGS"]),
                    "@" + str(rsp), "-o", str(work / "capture.elf")], cwd=build, check=True)
    image_line = next(line for line in ninja if " elf2image " in line and "kvmpad-unsigned.bin" in line)
    image_args = shlex.split(image_line.split(" elf2image ", 1)[1].split(" -o ", 1)[0])
    return json.loads(record.read_text())[1:], image_args


def portable_args(build, command, files):
    search = [Path(arg[2:]) for arg in command if arg.startswith("-L")]
    result = []

    def add_file(name, data):
        if name in files and files[name] != data:
            raise ValueError(f"conflicting inputs for {name}")
        files[name] = data
        return name

    arguments = iter(command)
    for arg in arguments:
        if arg == "-o":
            next(arguments)
        elif arg.startswith(("-L", "--sysroot=", "--dynconfig=", "--Map=")) or arg in ("--cref", "-fno-lto"):
            continue
        elif arg == "-T":
            name = next(arguments)
            path = next(folder / name for folder in search if (folder / name).is_file())
            # Generated IDF script comments contain local paths. They are not linker input.
            script = re.sub(rb"/\*.*?\*/", b" ", path.read_bytes(), flags=re.DOTALL)
            result.extend(["-T", add_file("scripts/" + name, script)])
        elif arg.startswith("-l"):
            name = "lib" + arg[2:] + ".a"
            path = next(folder / name for folder in search if (folder / name).is_file())
            match = [checksum for key, checksum in TOOLS.items() if key.endswith("/" + name)]
            if match != [hashlib.sha256(path.read_bytes()).hexdigest()]:
                raise ValueError(f"unexpected compiler runtime archive: {name}")
            result.append("runtime/" + name)
        elif arg.endswith((".a", ".obj", ".o")):
            path = (build / arg).resolve(strict=True)
            prefix = "libraries/" if path.name in LIBRARIES else "objects/"
            relative = path.relative_to(build).as_posix() if prefix == "objects/" and path.is_relative_to(build) else path.name
            result.append(add_file(prefix + relative, path.read_bytes()))
        elif any(c in arg for c in ":/\\@\n\r\""):
            raise ValueError(f"review new linker argument: {arg}")
        else:
            result.append(arg)
    return result


def write_zip(path, files):
    with zipfile.ZipFile(path, "w") as archive:
        for name, data in sorted(files.items()):
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            archive.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=6)


def package(build, compiler, output):
    output.mkdir(parents=True, exist_ok=True)
    description = json.loads((build / "project_description.json").read_bytes())
    if description["target"] != "esp32s3":
        raise ValueError("expected an ESP32-S3 build")
    files = {}
    with tempfile.TemporaryDirectory(dir=output) as temp:
        work = Path(temp)
        command, image_args = link_command(build, compiler, work)
        arguments = portable_args(build, command, files)
        signed = (build / "kvmpad.bin").read_bytes()
        unsigned = (build / "kvmpad-unsigned.bin").read_bytes()
        if not signed.startswith(unsigned) or len(signed) - len(unsigned) != 4096:
            raise ValueError("unexpected original signature sector")
        files["signature.bin"] = signed[len(unsigned):]
        plan = {"version": description["project_version"], "arguments": arguments,
                "image_arguments": image_args, "signed_sha256": hashlib.sha256(signed).hexdigest(),
                "unsigned_sha256": hashlib.sha256(unsigned).hexdigest(),
                "elf_sha256": hashlib.sha256((build / "kvmpad.elf").read_bytes()).hexdigest(),
                "files": {name: hashlib.sha256(data).hexdigest() for name, data in sorted(files.items())}}
        manifest = json.dumps(plan, sort_keys=True, indent=2).encode()
        objects = {k: v for k, v in files.items() if not k.startswith("libraries/")}
        libraries = {k: v for k, v in files.items() if k.startswith("libraries/")}
        if {Path(k).name for k in libraries} != LIBRARIES:
            raise ValueError("expected all four independent LGPL archives")
        objects["link.json"] = manifest
        objects["LICENSE.md"] = (REPO / "LICENSE.md").read_bytes()
        objects["THIRD_PARTY_NOTICES.md"] = (REPO / "THIRD_PARTY_NOTICES.md").read_bytes()
        idf = Path(description["idf_path"])
        for name in ("LICENSE", "components/esp_phy/lib/LICENSE", "components/esp_wifi/lib/LICENSE",
                     "components/esp_coex/lib/LICENSE", "components/lwip/lwip/COPYING",
                     "components/freertos/FreeRTOS-Kernel/LICENSE.md", "components/wpa_supplicant/COPYING",
                     "components/mbedtls/mbedtls/LICENSE", "components/mbedtls/mbedtls/tf-psa-crypto/LICENSE"):
            objects["licenses/esp-idf/" + name] = (idf / name).read_bytes()
        for name in ("components/pack_runtime/third_party/aws_lc/LICENSE",
                     "components/pack_runtime/third_party/aws_lc/LICENSE.fiat",
                     "components/ime_engine/openwnn/NOTICE", "components/ime_engine/pinyinime/NOTICE",
                     "components/ui_theme/fonts/OFL-NotoSans.txt", "components/ui_theme/fonts/OFL-NotoSansKR.txt"):
            objects["licenses/KVMpad/" + name] = (REPO / name).read_bytes()
        for component in description["build_component_info"].values():
            directory = Path(component["dir"])
            if directory.parent.name == "managed_components" and directory.name != "david-cermak__libssh":
                for license_file in directory.iterdir():
                    if license_file.is_file() and license_file.stem.lower() in ("license", "licence", "copying", "notice"):
                        objects[f"licenses/{directory.name}/{license_file.name}"] = license_file.read_bytes()
        for component, name in (("espressif__freetype", "freetype/docs/FTL.TXT"),
                                ("espressif__esp_tinyusb", "tinyusb/LICENSE"),
                                ("espressif__libpng", "libpng/LICENSE"), ("espressif__qoi", "qoi/LICENSE")):
            directory = Path(description["build_component_info"][component]["dir"])
            objects[f"licenses/{component}/{name}"] = (directory / name).read_bytes()
        ssh = Path(description["build_component_info"]["david-cermak__libssh"]["dir"])
        libraries["licenses/LGPL-2.1.txt"] = (ssh / "libssh-mirror/COPYING").read_bytes()
        libraries["licenses/libssh-BSD.txt"] = (ssh / "libssh-mirror/BSD").read_bytes()
        libraries["README.md"] = (HERE / "DECODER-SOURCE.md").read_bytes()
        for name, contents in (("objects", objects), ("libraries", libraries)):
            asset = work / f"kvmpad-firmware-{name}.zip"
            write_zip(asset, contents)
            asset.replace(output / asset.name)
        print("Trusted link manifest SHA-256:", hashlib.sha256(manifest).hexdigest())


if __name__ == "__main__":
    if sys.argv[1:2] == ["--capture"]:
        Path(sys.argv[2]).write_text(json.dumps(list(capture_args(sys.argv[3:]))))
    else:
        parser = argparse.ArgumentParser(description=__doc__)
        parser.add_argument("--build", required=True, type=Path)
        parser.add_argument("--compiler", required=True, type=Path, help="release Xtensa g++ driver")
        parser.add_argument("--output", required=True, type=Path)
        args = parser.parse_args()
        package(args.build.resolve(), args.compiler.resolve(), args.output.resolve())
