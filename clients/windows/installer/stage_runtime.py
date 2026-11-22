# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Stage a private CPython runtime for the Windows CLIENT tools."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile


def stage(destination):
    if sys.platform != "win32" or sys.version_info[:2] != (3, 12) or sys.maxsize < 2**32:
        raise SystemExit("Run with 64-bit CPython 3.12 for Windows.")
    base = Path(sys.base_prefix)
    destination.mkdir(parents=True, exist_ok=True)
    for name in ("python.exe", "python312.dll", "python3.dll", "vcruntime140.dll", "vcruntime140_1.dll", "LICENSE.txt"):
        shutil.copyfile(base / name, destination / name)
    dlls = destination / "DLLs"
    dlls.mkdir(exist_ok=True)
    for file in (base / "DLLs").iterdir():
        if file.suffix.lower() in {".dll", ".pyd", ".txt"} and "test" not in file.name:
            shutil.copyfile(file, dlls / file.name)
    for name in ("tcl8", "tcl8.6", "tk8.6"):
        shutil.copytree(base / "tcl" / name, destination / "tcl" / name, dirs_exist_ok=True)
    with zipfile.ZipFile(destination / "python312.zip", "w", zipfile.ZIP_DEFLATED) as archive:
        for file in sorted((base / "Lib").rglob("*")):
            relative = file.relative_to(base / "Lib")
            if (file.is_file() and file.suffix != ".pyc" and
                    not set(relative.parts) & {"site-packages", "test", "tests", "__pycache__", "idlelib", "ensurepip"}):
                archive.write(file, relative.as_posix())
    # Keep a landmark for Python's stdlib discovery after relocation.
    (destination / "Lib").mkdir(exist_ok=True)
    shutil.copyfile(base / "Lib/os.py", destination / "Lib/os.py")
    packages = destination / "Lib/site-packages"
    subprocess.run([sys.executable, "-I", "-m", "pip", "install", "--disable-pip-version-check",
                    "--no-compile", "--upgrade", "--target", str(packages), "-r",
                    str(Path(__file__).with_name("runtime-requirements.txt"))], check=True)
    # Wheel launchers can contain paths to the build interpreter. Use module entry points.
    if (packages / "bin").is_dir():
        shutil.rmtree(packages / "bin")
    for origin in packages.glob("*.dist-info/direct_url.json"):
        origin.unlink()
    subprocess.run([destination / "python.exe", "-I", "-B", "-c",
                    "import tkinter, serial, jsonschema, PIL, cryptography, websockets; "
                    "assert tkinter.Tcl().eval('info patchlevel').startswith('8.6')"], check=True)
    subprocess.run([destination / "python.exe", "-I", "-B", "-m", "esptool", "version"], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    stage(parser.parse_args().destination.resolve())
