# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Prepare the signed application on the operator PC from independent linker inputs."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from urllib.request import urlopen

INPUTS = ("kvmpad-firmware-objects.zip", "kvmpad-firmware-libraries.zip")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def obtain(directory, name, package, source, local, report):
    path = (local or directory) / name
    if path.exists() and path.stat().st_size == package["bytes"] and digest(path) == package["sha256"]:
        return path
    if local:
        raise ValueError(f"Missing or incorrect {name} in the selected folder.")
    report(f"Downloading {name} ({package['bytes'] // 1048576} MiB)…")
    with tempfile.TemporaryDirectory(dir=directory) as temp:
        partial = Path(temp) / name
        with urlopen(source + "/" + name, timeout=45) as response, partial.open("xb") as output:
            if not response.geturl().startswith("https://"):
                raise ValueError("The download requires HTTPS.")
            while block := response.read(262144):
                output.write(block)
                if output.tell() > package["bytes"]:
                    raise ValueError(f"{name} exceeds its published size.")
        if partial.stat().st_size != package["bytes"] or digest(partial) != package["sha256"]:
            raise ValueError(f"Download verification failed for {name}.")
        partial.replace(path)
    return path


def prepare(directory, local, report):
    manifest = json.loads((directory / "manifest.json").read_bytes())
    expected = manifest["sha256"]["kvmpad.bin"]
    cache = Path(os.environ["LOCALAPPDATA"]) / "KVMpad" / "firmware" / expected
    for path in (directory / "kvmpad.bin", cache / "kvmpad.bin"):
        if path.is_file() and digest(path) == expected:
            return path
    delivery = json.loads((directory / "delivery.json").read_bytes())
    cache.mkdir(parents=True, exist_ok=True)
    objects, libraries = (obtain(cache, name, delivery["files"][name], delivery["source"], local, report)
                          for name in INPUTS)
    report("Linking the firmware on this PC. The first run also prepares the Espressif tools…")
    with tempfile.TemporaryDirectory(dir=cache) as temp:
        image = Path(temp) / "kvmpad.bin"
        command = [sys.executable, "-E", "-s", "-B", str(directory.parent / "relink_firmware.py"),
                   "--objects", str(objects), "--libraries", str(libraries), "--output", str(image)]
        if tools := os.environ.get("KVMPAD_RELINK_TOOLS"):
            command += ["--tools", tools]
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        if result.returncode:
            errors = (result.stderr or result.stdout).strip().splitlines()
            raise ValueError(errors[-1] if errors else "The linker failed.")
        if digest(image) != expected:
            raise ValueError("The linked image differs from the release image.")
        image.replace(cache / "kvmpad.bin")
    return cache / "kvmpad.bin"
