# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Prepare a verified 7B installation directory from an ESP-IDF build."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile


def stage(build, ime, key, target):
    board = "ESP32-S3-Touch-LCD-7B"
    info = json.loads((build / "project_description.json").read_bytes())
    if info["project_name"] != "kvmpad" or info["target"] != "esp32s3":
        raise ValueError("Expected an ESP32-S3 KVMpad build.")
    config = set(Path(info["config_file"]).read_text(encoding="utf-8").splitlines())
    required = {'CONFIG_ESPTOOLPY_FLASHSIZE="16MB"', "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y",
                "CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y"}
    if required - config:
        raise ValueError("The 7B release requires 16 MiB flash, signed OTA and rollback.")
    if b"PRIVATE KEY" in key.read_bytes() or b"BEGIN PUBLIC KEY" not in key.read_bytes():
        raise ValueError("Supply the public OTA verification key.")
    segments = {"bootloader.bin": (0, 0x8000), "partition-table.bin": (0x8000, 0x1000),
                "ota_data_initial.bin": (0x19000, 0x2000), "kvmpad.bin": (0x20000, 0x400000)}
    sources = json.loads((build / "flasher_args.json").read_bytes())["flash_files"]
    if {int(address, 0) for address in sources} != {offset for offset, _ in segments.values()}:
        raise ValueError("Unexpected flash layout.")
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=target.parent) as temp:
        out = Path(temp) / "7b"
        out.mkdir()
        for address, relative in sources.items():
            name = Path(relative).name
            offset, limit = segments[name]
            path = build / relative
            if int(address, 0) != offset or not 0 < path.stat().st_size <= limit:
                raise ValueError(f"Invalid flash segment: {name}")
            shutil.copyfile(path, out / name)
        app = (out / "kvmpad.bin").read_bytes()
        version = app[48:80].split(b"\0")[0].decode("ascii")
        if version != info["project_version"] or app[80:112].split(b"\0")[0] != b"kvmpad":
            raise ValueError("The firmware identity differs from its build metadata.")
        subprocess.run([sys.executable, "-I", "-m", "espsecure", "verify-signature", "--version", "2",
                        "--keyfile", key, out / "kvmpad.bin"], check=True)
        data = ime.read_bytes()
        if data[:8] != b"KVMIMEDT" or len(data) > 0x7B0000:
            raise ValueError("Invalid language image.")
        schema, count = struct.unpack_from("<II", data, 8)
        if schema != 1 or not 1 <= count <= 8:
            raise ValueError("Invalid language directory.")
        for i in range(count):
            offset, length, digest = struct.unpack_from("<II32s", data, 48 + i * 64)
            if offset < 4096 or offset % 4096 or offset + length > len(data) or hashlib.sha256(data[offset:offset+length]).digest() != digest:
                raise ValueError("Language module verification failed.")
        (out / "ime_data.bin").write_bytes(data)
        shutil.copyfile(key, out / "ota-public.pem")
        hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(out.glob("*.bin"))}
        (out / "manifest.json").write_text(json.dumps({"board": board, "firmware": version, "sha256": hashes},
                                                     indent=2) + "\n", encoding="utf-8")
        out.rename(target)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("build", "ime", "public-key", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    stage(args.build, args.ime, args.public_key, args.out)
