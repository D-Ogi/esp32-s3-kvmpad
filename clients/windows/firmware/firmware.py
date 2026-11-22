# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Install the offline 7B image or start the board's signed HTTPS update over UART."""
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import time
from urllib.parse import urlsplit

import serial

FLASH_SIZE = 16 * 1024 * 1024
# Only these images may enter the wired installation. NVS is device-specific.
LAYOUT = {
    "bootloader.bin": (0, 0x8000),
    "partition-table.bin": (0x8000, 0x1000),
    "ota_data_initial.bin": (0x19000, 0x2000),
    "kvmpad.bin": (0x20000, 0x400000),
    "ime_data.bin": (0x820000, 0x7B0000),
}


def external_tool(module, *args):
    result = subprocess.run([sys.executable, "-I", "-B", "-m", module, *map(str, args)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            encoding="utf-8", errors="replace", timeout=600,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if result.returncode:
        details = result.stdout.strip().splitlines()
        raise OSError(details[-1] if details else f"{module} failed ({result.returncode}).")
    return result.stdout


def load_bundle(directory, application):
    manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
    if manifest["board"] != "ESP32-S3-Touch-LCD-7B" or set(manifest["sha256"]) != set(LAYOUT):
        raise ValueError("This package does not contain the complete 7B image.")
    images = {}
    for name, (offset, limit) in LAYOUT.items():
        data = (application if name == "kvmpad.bin" else directory / name).read_bytes()
        if not 0 < len(data) <= limit or hashlib.sha256(data).hexdigest() != manifest["sha256"][name]:
            raise ValueError(f"Package verification failed for {name}. Repair the CLIENT installation.")
        images[offset] = data
    app = images[0x20000]
    if (app[:1] != b"\xe9" or app[12:14] != b"\x09\x00" or app[32:36] != b"2T\xcd\xab" or
            app[80:112].split(b"\0")[0] != b"kvmpad" or
            app[48:80].split(b"\0")[0].decode("ascii") != manifest["firmware"]):
        raise ValueError("Unexpected chip, application or firmware version.")
    external_tool("espsecure", "verify-signature", "--version", "2", "--keyfile",
                  directory / "ota-public.pem", application)
    return manifest, images


def install(port, directory, backup, local, report):
    from firmware_image import prepare
    application = prepare(directory, local, report)
    manifest, images = load_bundle(directory, application)
    report(f"Checking the board on {port} for firmware {manifest['firmware']}…")
    def flash(*args, after="no-reset"):
        return external_tool("esptool", "--chip", "esp32s3", "--port", port, "--baud", "460800",
                             "--after", after, *args)
    security = flash("get-security-info")
    if "Secure Boot: Disabled" not in security or "Flash Encryption: Disabled" not in security:
        raise ValueError("This wired installer requires an unencrypted board with hardware Secure Boot off.")
    identity = flash("flash-id")
    if not re.search(r"^Detected flash size: 16MB\s*$", identity, re.MULTILINE):
        raise ValueError("The 7B requires 16 MiB of detected flash.")
    revision = re.search(r"revision v(\d+)\.(\d+)", identity)
    address = re.search(r"^MAC:\s+([a-fA-F0-9:]+)", identity, re.MULTILINE)
    if not revision or not address:
        raise ValueError("Could not identify the board. Check the USB-UART connection.")
    chip_revision = int(revision[1]) * 100 + int(revision[2])
    for header in (images[0], images[0x20000]):
        minimum, maximum = struct.unpack_from("<HH", header, 15)
        if chip_revision < minimum or (maximum and chip_revision > maximum):
            raise ValueError("This firmware does not support the connected chip revision.")

    if backup.exists():
        raise FileExistsError("Choose a new filename to keep the existing backup.")
    with tempfile.TemporaryDirectory(dir=backup.parent) as temp:
        path = Path(temp) / "backup.bin"
        report("Reading the 16 MiB backup. Keep USB connected.")
        flash("read-flash", "--no-progress", "0", hex(FLASH_SIZE), path)
        snapshot = path.read_bytes()
        if len(snapshot) != FLASH_SIZE:
            raise OSError("The board returned an incomplete backup.")
        report("Checking the backup against flash…")
        device = flash("verify-flash", "0", path)
        if not re.search(r"^MAC:\s+" + re.escape(address[1]) + r"\s*$", device, re.MULTILINE):
            raise OSError("The board identity changed during the backup.")
        # Windows rename is atomic on this volume and fails if the name now exists.
        with path.open("r+b") as output:
            os.fsync(output.fileno())
        path.rename(backup)
        same_layout = snapshot[0x8000:0x8000 + len(images[0x8000])] == images[0x8000]
        erase = [] if same_layout or snapshot.count(0xff) == FLASH_SIZE else ["--erase-all"]
        segments = []
        for name, (offset, _) in LAYOUT.items():
            staged = Path(temp) / name
            staged.write_bytes(images[offset])
            segments.extend([hex(offset), staged])
        report("Backup verified. Writing firmware and language data…")
        flash("write-flash", "--no-progress", *erase, *segments)
        report("Verifying the installed files…")
        flash("verify-flash", *segments, after="hard-reset")
    return "Flash verification passed. The board has restarted. Check the touchscreen."


def validate_url(url):
    parts = urlsplit(url)
    if (not url.startswith("https://") or not parts.hostname or parts.username is not None or
            parts.fragment or not re.fullmatch(r"[A-Za-z0-9:/._~%?=&+@,-]+", url) or
            len("ota url " + url) >= 128):
        raise ValueError("Use a short HTTPS image URL without spaces, credentials or a fragment (119 characters maximum).")


def reply(link, line, pattern, seconds):
    link.reset_input_buffer()
    link.write((line + "\r\n").encode("ascii"))
    end = time.monotonic() + seconds
    raw = b""
    while time.monotonic() < end:
        raw = (raw + link.read(4096))[-32768:]
        text = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", raw.decode("ascii", "replace")).replace("\r", "")
        if re.search(pattern, text, re.MULTILINE):
            return text
    raise TimeoutError("No complete reply from KVMpad. Check the USB-UART port and board power.")


def status(link):
    text = reply(link, "ota status", r"^counters +attempts \d+ aborts \d+ rollbacks \d+", 3)
    def field(label):
        match = re.search("^" + label + r" +(.+)$", text, re.MULTILINE)
        if not match:
            raise ValueError("The board did not report " + label + ".")
        return match[1].strip()
    if not field("app").startswith("kvmpad ") or field("signed ota") != "yes" or field("rollback") != "enabled":
        raise ValueError("KVMpad must support signed updates and rollback.")
    return field("state"), field("running"), field("next slot").split()[0], field("last reason"), text


def update(port, url, report):
    validate_url(url)
    with serial.Serial(port=None, baudrate=115200, timeout=.15, write_timeout=3) as link:
        link.dtr = link.rts = False
        link.port = port
        link.open()
        state, running, target, _, _ = status(link)
        if state not in {"idle", "confirmed", "failed"} or " confirmed" not in running:
            raise ValueError("Wait for the board to confirm its running firmware.")
        answer = reply(link, "ota url " + url, r"^refused:|^update started;", 5)
        if "update started;" not in answer:
            raise ValueError("The board refused the update. Check its current state.")
        deadline = time.monotonic() + 600
        while time.monotonic() < deadline:
            time.sleep(1)
            try:
                state, running, _, reason, text = status(link)
            except TimeoutError:
                continue  # UART remains on the independent bridge during the ESP restart.
            progress = re.search(r"^progress +(\d+%)", text, re.MULTILINE)
            report(f"{state.capitalize()}  {progress[1] if progress else ''}")
            if state in {"failed", "rollback"} or (reason != "none" and state in {"idle", "confirmed"}):
                raise ValueError("The board reported " + reason + ". Check the board and the HTTPS image.")
            if running.startswith(target + " ") and " confirmed" in running:
                app = re.search(r"^app +kvmpad (\S+)", text, re.MULTILINE)
                return f"Board confirmed firmware {app[1]} in {target}."
    raise TimeoutError("The board did not confirm the new slot. Check OTA status before retrying.")
