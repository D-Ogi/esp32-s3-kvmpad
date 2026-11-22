# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Download only Debian packages verified against the signed Bookworm release."""

import argparse
import base64
import hashlib
import json
import lzma
import re
import shutil
import subprocess
import urllib.request
from pathlib import Path

ROOT = "https://deb.debian.org/debian/"
STABLE_KEY = "4D64FEC119C2029067D6E791F8D2585B8783D481"
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("out", type=Path)
p.add_argument("--gpgv", default="gpgv")
p.add_argument(
    "--audit-debian-kernel",
    action="store_true",
    help="Also fetch the Debian generic kernel and record its GUD configuration",
)
a = p.parse_args()
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=True)


def download(url, dest):
    with urllib.request.urlopen(url, timeout=60) as f, dest.open("wb") as d:
        shutil.copyfileobj(f, d)


def checked(url, name, digest):
    dest = out / name
    if not dest.exists() or hashlib.sha256(dest.read_bytes()).hexdigest() != digest:
        download(url, dest)
    if hashlib.sha256(dest.read_bytes()).hexdigest() != digest:
        raise ValueError(f"SHA256 mismatch: {name}")
    return dest


home = out / "gnupg"
home.mkdir(exist_ok=True)
for filename in ("Release", "Release.gpg"):
    download(ROOT + "dists/bookworm/" + filename, out / filename)
# Load the three keys used by the Debian archive; require the named stable key.
keys = bytearray()
for k in ("release-12", "archive-key-12", "archive-key-13"):
    url = f"https://ftp-master.debian.org/keys/{k}.asc"
    armor = urllib.request.urlopen(url, timeout=20).read().decode().replace("\r", "")
    keys.extend(
        base64.b64decode("".join(armor.split("\n\n", 1)[1].split("\n=")[0].split()))
    )
(home / "release-keys.gpg").write_bytes(keys)
verify = subprocess.run(
    [
        a.gpgv,
        "--homedir",
        str(home),
        "--keyring",
        "release-keys.gpg",
        "--status-fd",
        "1",
        str(out / "Release.gpg"),
        str(out / "Release"),
    ],
    capture_output=True,
    text=True,
)
(out / "signature.log").write_text(verify.stdout + verify.stderr)
if f"[GNUPG:] VALIDSIG {STABLE_KEY} " not in verify.stdout or verify.returncode:
    raise RuntimeError((verify.stdout, verify.stderr))
release = (out / "Release").read_text().split("\nSHA256:\n", 1)[1].split("\n\n", 1)[0]
path = "main/binary-amd64/Packages.xz"
digest = re.search(r"^ ([a-f0-9]{64})\s+\d+ " + re.escape(path) + r"$", release, re.M)[
    1
]
index = lzma.decompress(
    checked(ROOT + "dists/bookworm/" + path, "Packages.xz", digest).read_bytes()
).decode()
packages = {}
for block in index.split("\n\n"):
    fields = dict(
        line.split(": ", 1)
        for line in block.splitlines()
        if line and line[0] != " " and ": " in line
    )
    if fields.get("Package"):
        packages[fields["Package"]] = fields
kernel = packages["linux-image-amd64"]["Depends"].split(" ")[0]
manifest = {}
needed = [
    "busybox-static",
    "musl-dev",
    "linux-libc-dev",
    "libdrm-dev",
    "archlinux-keyring",
]
if a.audit_debian_kernel:
    needed.append(kernel)
for name in needed:
    fields = packages[name]
    fname = Path(fields["Filename"]).name
    checked(ROOT + fields["Filename"], fname, fields["SHA256"])
    manifest[name] = {k: fields[k] for k in ("Filename", "Version", "SHA256", "Size")}
    print(name, fields["Version"], fields["Size"], "bytes verified", flush=True)
(out / "packages.json").write_text(json.dumps(manifest, indent=2) + "\n")
