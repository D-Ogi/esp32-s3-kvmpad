# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Package the Windows CLIENT, public firmware metadata and standalone tools for a test release."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import re
from urllib.parse import urljoin

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
PACKAGES = ("kvmpad-firmware-objects.zip", "kvmpad-firmware-libraries.zip")


def build(args):
    out = args.work.resolve()
    payload = out / "client"
    payload.mkdir(parents=True, exist_ok=False)
    subprocess.run([args.dotnet, "publish", ROOT / "clients/windows/src/KVMpad.Client.App",
                    "-c", "Release", "-r", "win-x64", "--self-contained", "true",
                    "--artifacts-path", out / "dotnet", "-o", payload,
                    "-p:Version=" + args.version, "-p:DebugType=None", "-p:DebugSymbols=false",
                    "-p:CardVideoReader=" + str(out / "external-reader")], check=True)
    shutil.copytree(args.runtime, payload / "runtime")
    for name in ("kvmpad_pairing.dll", "host/kvmpad-capture.exe", "layouts/kbd_layouts.json",
                 "firmware/firmware_ui.py", "studio/extension_studio.py", "runtime/python.exe"):
        if not (payload / name).is_file():
            raise ValueError("Missing release dependency: " + name)
    if (payload / "media/card-pixels.exe").exists():
        raise ValueError("The external card reader is not part of this installer.")
    firmware = payload / "firmware"
    board = firmware / "7b"
    board.mkdir()
    manifest = json.loads((args.board / "manifest.json").read_bytes())
    for name in ("bootloader.bin", "partition-table.bin", "ota_data_initial.bin", "ime_data.bin"):
        source = args.board / name
        if hashlib.sha256(source.read_bytes()).hexdigest() != manifest["sha256"][name]:
            raise ValueError("Invalid board image: " + name)
        shutil.copyfile(source, board / name)
    for name in ("ota-public.pem", "manifest.json"):
        shutil.copyfile(args.board / name, board / name)
    shutil.copyfile(ROOT / "tools/live_view/relink_firmware.py", firmware / "relink_firmware.py")
    files = {name: {"sha256": hashlib.sha256((args.packages / name).read_bytes()).hexdigest(),
                    "bytes": (args.packages / name).stat().st_size} for name in PACKAGES}
    (board / "delivery.json").write_text(json.dumps({
        "source": "https://github.com/D-Ogi/esp32-s3-kvmpad/releases/download/" + args.tag,
        "files": files}, indent=2) + "\n", encoding="utf-8")
    for name in ("LICENSE.md", "THIRD_PARTY_NOTICES.md"):
        shutil.copyfile(ROOT / name, payload / name)
    licenses = payload / "licenses"
    licenses.mkdir()
    shutil.copyfile(HERE / "PROGRAMMER-NOTICES.txt", licenses / "PROGRAMMER-NOTICES.txt")
    guide = (ROOT / "docs/INSTALL.md").read_text(encoding="utf-8")
    guide_url = f"https://github.com/D-Ogi/esp32-s3-kvmpad/blob/{args.tag}/docs/INSTALL.md"
    guide = re.sub(r"\]\(([^()\s]+)\)", lambda link: "](" + urljoin(guide_url, link[1]) + ")", guide)
    (payload / "INSTALL.md").write_text(guide, encoding="utf-8")
    # NuGet runtime packs include the notices that apply to the runtime redistributables.
    runtime_version = json.loads((payload / "KVMpad.Client.runtimeconfig.json").read_bytes())["runtimeOptions"]["includedFrameworks"]
    cache = Path(os.environ["NUGET_PACKAGES"])
    for framework in runtime_version:
        name = framework["name"].lower() + ".runtime.win-x64"
        package = cache / name / framework["version"]
        for path in package.iterdir():
            if path.is_file() and ("license" in path.name.lower() or "notice" in path.name.lower()):
                shutil.copyfile(path, licenses / (name + "-" + path.name))
    for source in args.notice:
        shutil.copyfile(source, licenses / source.name)
    msi = args.packages / "KVMpad-Client-x64.msi"
    subprocess.run([sys.executable, HERE / "build_msi.py", "--payload", payload, "--version", args.version,
                    "--output", msi, "--wix", args.wix], check=True)
    subprocess.run([sys.executable, HERE / "verify_msi.py", "--payload", payload, "--msi", msi,
                    "--wix", args.wix], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("board", "runtime", "packages", "work", "wix", "dotnet"):
        parser.add_argument("--" + name, type=lambda path: Path(path).resolve(), required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--notice", action="append", default=[], type=Path)
    build(parser.parse_args())
