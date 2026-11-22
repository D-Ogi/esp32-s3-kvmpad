# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Link KVMpad from separate x64 Windows release inputs. No compiler, installation or board required."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

RELEASE_MANIFEST = "5f8d414c6e638d9aba20012e1dfb7f4d2be3b52437e542bcac2c8fd7670caa68"
VERSION = "esp-15.2.0_20251204"
TOOLS_URL = ("https://github.com/espressif/crosstool-NG/releases/download/" + VERSION
             + "/xtensa-esp-elf-15.2.0_20251204-x86_64-w64-mingw32.zip")
TOOLS_SHA = "efa1e337b5f64239674bf755b767969cbba7f4e66fe7c6ab8d19b650f56012a9"
SYS_LIB = "picolibc/xtensa-esp-elf/lib/esp32s3/no-rtti/"
TOOLS = {
    "bin/xtensa-esp-elf-ld.exe": "78b8759ab101bf426d6483f07a0e596acc41d92779e74572ef05c6de07b4e945",
    "lib/xtensa_esp32s3.so": "ef09e866a799f6a38ddcde18d7c0ff1a9d8ac3a9bf91554b0e656f0563f285a8",
    "picolibc/lib/gcc/xtensa-esp-elf/15.2.0/esp32s3/no-rtti/libgcc.a":
        "e263d21d76ef8790122fc3b790b50c09a80b29182d520820b4b038a65c2997c1",
    SYS_LIB + "libc.a": "f30cccb316b7daf5ad0f240874556f8cd53cef43c1fd6b9e7250298b0cd2a943",
    SYS_LIB + "libm.a": "64d11f883e2556321bd440749195e8445e312885958e447001723055b7604d98",
    SYS_LIB + "libstdc++.a": "4b207709c179bf1187c317dc2711a89f6a0febb2741ff36c4a8f614e0b3a29bd",
}


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verified_tools(root):
    return all((root / name).is_file() and digest(root / name) == checksum for name, checksum in TOOLS.items())


def linker_tools(offline):
    if offline:
        if not verified_tools(offline):
            raise ValueError("the offline Espressif linker directory is incomplete or differs from the release")
        return offline.resolve()
    root = Path(os.environ["LOCALAPPDATA"]) / "KVMpad" / "relink" / VERSION
    if not verified_tools(root):
        root.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=root.parent) as temporary:
            download = Path(temporary) / "espressif.zip"
            print("Downloading the verified Espressif Windows linker package (390 MiB).", flush=True)
            with urllib.request.urlopen(TOOLS_URL, timeout=120) as source, download.open("wb") as dest:
                shutil.copyfileobj(source, dest)
            if digest(download) != TOOLS_SHA:
                raise ValueError("Espressif tool archive checksum differs")
            with zipfile.ZipFile(download) as archive:
                for name, checksum in TOOLS.items():
                    content = archive.read("xtensa-esp-elf/" + name)
                    if hashlib.sha256(content).hexdigest() != checksum:
                        raise ValueError(f"incorrect Espressif file: {name}")
                    dest = root / name
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    staged = Path(temporary) / dest.name
                    staged.write_bytes(content)
                    staged.replace(dest)
    return root


def relink(args):
    output = args.output.resolve()
    if output in (args.objects.resolve(), args.libraries.resolve()):
        raise ValueError("select a separate output file")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="kvmpad-relink-", dir=output.parent) as temporary:
        work = Path(temporary)
        with zipfile.ZipFile(args.objects) as objects, zipfile.ZipFile(args.libraries) as libraries:
            if objects.getinfo("link.json").file_size > 256 * 1024:
                raise ValueError("oversized firmware manifest")
            raw = objects.read("link.json")
            if hashlib.sha256(raw).hexdigest() != RELEASE_MANIFEST:
                raise ValueError("firmware object manifest does not match this release")
            plan = json.loads(raw)
            total = 0
            for name, checksum in plan["files"].items():
                archive = libraries if name.startswith("libraries/") else objects
                total += archive.getinfo(name).file_size
                if total > 160 * 1024 * 1024:
                    raise ValueError("object archives exceed the release extraction size")
                path = work / name
                path.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(name) as source, path.open("wb") as dest:
                    shutil.copyfileobj(source, dest)
                if digest(path) != checksum:
                    raise ValueError(f"incorrect release file: {name}")
        if args.library_dir:
            if not args.unsigned:
                raise ValueError("select --unsigned to link replacement libraries")
            replacements = list(args.library_dir.glob("*.a"))
            if not replacements:
                raise ValueError("no replacement .a files in --library-dir")
            for replacement in replacements:
                dest = work / "libraries" / replacement.name
                if not dest.is_file():
                    raise ValueError(f"unknown library: {replacement.name}")
                shutil.copyfile(replacement, dest)

        tool = linker_tools(args.tools)
        for name in TOOLS:
            if name.endswith(".a"):
                target = work / "runtime" / Path(name).name
                target.parent.mkdir(exist_ok=True)
                shutil.copyfile(tool / name, target)
        print("Linking the ESP32-S3 application.", flush=True)
        response = work / "link.rsp"
        response.write_text("\n".join('"' + arg + '"' for arg in plan["arguments"]), encoding="utf-8")
        subprocess.run([tool / "bin/xtensa-esp-elf-ld.exe", "--dynconfig=" + str(tool / "lib/xtensa_esp32s3.so"),
                        "-o", "firmware.elf", "@link.rsp"], cwd=work, check=True)
        if not args.unsigned and digest(work / "firmware.elf") != plan["elf_sha256"]:
            raise ValueError("linked ELF differs from the release")
        subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32s3", "elf2image",
                        *plan["image_arguments"], "-o", "firmware.bin", "firmware.elf"], cwd=work, check=True)
        firmware = work / "firmware.bin"
        if not args.unsigned:
            if digest(firmware) != plan["unsigned_sha256"]:
                raise ValueError("linked image differs from the signed release")
            with firmware.open("ab") as signed:
                signed.write((work / "signature.bin").read_bytes())
            if digest(firmware) != plan["signed_sha256"]:
                raise ValueError("signed firmware checksum differs from the release")
        firmware.replace(output)
    print(f"{output}  SHA-256 {digest(output)}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--objects", type=Path, required=True)
    parser.add_argument("--libraries", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tools", type=Path, help="offline xtensa-esp-elf root with verified linker and runtime archives")
    parser.add_argument("--unsigned", action="store_true", help="write the locally linked unsigned image")
    parser.add_argument("--library-dir", type=Path, help="replacement .a files for a local unsigned build")
    try:
        relink(parser.parse_args())
    except (KeyError, OSError, ValueError, zipfile.BadZipFile, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"Relink failed: {exc}\n")
