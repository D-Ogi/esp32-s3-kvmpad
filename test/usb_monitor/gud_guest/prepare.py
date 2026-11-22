# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Prepare a verified guest kernel, selected modules and C sysroot in the evidence directory."""

import argparse
import base64
import hashlib
import io
import json
import re
import shutil
import subprocess
import tarfile
import urllib.request
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("out", type=Path)
p.add_argument("--gpgv", default="gpgv")
a = p.parse_args()
o = a.out.resolve()


def put(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def debmembers(path):
    raw = path.read_bytes()
    assert raw.startswith(b"!<arch>\n")
    pos = 8
    while pos < len(raw):
        size = int(raw[pos + 48 : pos + 58])
        name = raw[pos : pos + 16].strip()
        pos += 60
        if name.startswith(b"data."):
            with tarfile.open(
                fileobj=io.BytesIO(raw[pos : pos + size]), mode="r|*"
            ) as ar:
                for member in ar:
                    if member.isfile():
                        yield member.name.removeprefix("./"), ar.extractfile(member)
            return
        pos += size + size % 2


pkgs = json.loads((o / "packages.json").read_text())
for name, entry in pkgs.items():
    package = o / Path(entry["Filename"]).name
    if hashlib.sha256(package.read_bytes()).hexdigest() != entry["SHA256"]:
        raise ValueError(f"SHA256 mismatch: {package.name}")
    if name.startswith("linux-image-"):
        for path, stream in debmembers(package):
            if path.startswith("boot/config-"):
                lines = stream.read().decode().splitlines()
                verdict = [
                    s
                    for s in lines
                    if any(
                        k in s
                        for k in (
                            "CONFIG_DRM_GUD",
                            "CONFIG_USB_DUMMY_HCD",
                            "CONFIG_USB_CONFIGFS_F_FS",
                        )
                    )
                ]
                (o / "debian-kernel-config.txt").write_text(
                    path + "\n" + "\n".join(verdict) + "\n"
                )
                break
        continue
    for path, stream in debmembers(package):
        if path == "bin/busybox":
            put(o / "busybox", stream.read())
        elif "/include/" in path or path.startswith("usr/lib/x86_64-linux-musl/"):
            put(o / "sysroot" / path, stream.read())
        elif path.endswith("/archlinux.gpg"):
            armored = stream.read().replace(b"\r", b"")
            blocks = re.findall(
                b"-----BEGIN PGP PUBLIC KEY BLOCK-----.*?\n\n(.*?)\n=", armored, re.S
            )
            binary = (
                b"".join(base64.b64decode(block) for block in blocks)
                if blocks
                else armored
            )
            put(o / "gnupg/archlinux.gpg", binary)

# Verify this exact package signer using the independently signed Debian keyring.
# A second mirror can deliver the same bytes without changing the trust decision.
name = "linux-lts-6.18.64-1-x86_64.pkg.tar.zst"
signer = "ADC8A1FCC15E01D45310419E94657AB20F2A092B"
base = "https://mirror.rackspace.com/archlinux/core/os/x86_64/"
archive = "https://archive.archlinux.org/packages/l/linux-lts/"
for filename in (name, name + ".sig"):
    dest = o / filename
    if not dest.exists():
        for url in (base + filename, archive + filename):
            try:
                with (
                    urllib.request.urlopen(url, timeout=60) as src,
                    dest.with_suffix(".part").open("wb") as out,
                ):
                    shutil.copyfileobj(src, out)
                dest.with_suffix(".part").replace(dest)
                break
            except OSError:
                if url.startswith(archive):
                    raise
verify = subprocess.run(
    [
        a.gpgv,
        "--homedir",
        str(o / "gnupg"),
        "--keyring",
        "archlinux.gpg",
        "--status-fd",
        "1",
        str(o / (name + ".sig")),
        str(o / name),
    ],
    capture_output=True,
    text=True,
)
(o / "kernel-signature.log").write_text(verify.stdout + verify.stderr)
if verify.returncode or "VALIDSIG " + signer + " " not in verify.stdout:
    raise RuntimeError(verify.stdout + verify.stderr)
digest = hashlib.sha256((o / name).read_bytes()).hexdigest()
if digest != "bb164f90c8aa3ad01249014d3ecadfad43e9e726e64e883fafbac0239ecc054f":
    raise ValueError(f"SHA256 mismatch: {name}")
(o / "kernel-sha256.txt").write_text(digest + "  " + name + "\n")
# Read module dependency names from the signed .ko metadata.
with (o / "kernel.tar").open("wb") as file:
    subprocess.run(["zstd", "-qdc", str(o / name)], stdout=file, check=True)
with tarfile.open(o / "kernel.tar") as ar:
    all_files = ar.getnames()
    root = next(
        p[: -len("vmlinuz")]
        for p in all_files
        if "/modules/" in p and p.endswith("/vmlinuz")
    )
    put(o / "vmlinuz", ar.extractfile(root + "vmlinuz").read())
    mods = [
        s[len(root) :]
        for s in all_files
        if s.startswith(root) and s.endswith(".ko.zst")
    ]
    by_name = {Path(s).name[:-7].replace("-", "_"): s for s in mods}
    done = set()
    order = []

    def add(mod):
        if mod in done:
            return
        done.add(mod)
        compressed = ar.extractfile(root + by_name[mod]).read()
        data = subprocess.check_output(["zstd", "-qdc"], input=compressed)
        match = re.search(rb"\x00depends=([^\x00]*)\x00", data)
        if match and match[1]:
            for d in match[1].decode().split(","):
                add(d.replace("-", "_"))
        put(o / "mods" / (mod + ".ko"), data)
        order.append(mod)

    for mod in ["dummy_hcd", "usb_f_fs", "gud"]:
        add(mod)
    (o / "load-modules").write_text(
        "\n".join("insmod /mods/" + mod + ".ko || exit 1" for mod in order) + "\n"
    )
    print("Verified kernel and module dependency closure", root, order)
