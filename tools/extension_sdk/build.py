# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Build an offline Extension Studio distribution for this Python interpreter and platform."""
import argparse
import base64
import hashlib
from importlib.metadata import distribution
from io import BytesIO
import json
from pathlib import Path
import sys
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

from packaging.requirements import Requirement
from packaging.utils import canonicalize_name

from start import python_target

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
STUDIO = REPO / "tools/kvmpack"
sys.path.insert(0, str(STUDIO))
from card import write_file


def runtime_files(files):
    versions = {}
    pending = [Requirement(name) for name in ("jsonschema>=4,<5", "Pillow", "cryptography")]
    while pending:
        requirement = pending.pop()
        name = canonicalize_name(requirement.name)
        dist = distribution(name)
        if dist.version not in requirement.specifier:
            raise ValueError(f"{requirement} is required, found {dist.version}")
        if name in versions:
            continue
        versions[name] = dist.version
        dependencies = map(Requirement, dist.requires or ())
        pending.extend(r for r in dependencies if r.marker is None or r.marker.evaluate({"extra": ""}))
        if not dist.files:
            raise ValueError(f"{name} needs an installed wheel with a RECORD")
        for entry in dist.files:
            if (entry.is_absolute() or ".." in entry.parts or "__pycache__" in entry.parts or
                    entry.suffix in {".pyc", ".pyo"}):
                continue
            if entry.parts[0].endswith(".dist-info") and entry.name in {
                "RECORD", "INSTALLER", "REQUESTED", "direct_url.json"
            }:
                continue
            if entry.hash is None:
                raise ValueError(f"{name}: missing wheel digest for {entry}")
            data = Path(dist.locate_file(entry)).read_bytes()
            digest = hashlib.new(entry.hash.mode, data).digest()
            if base64.urlsafe_b64encode(digest).rstrip(b"=").decode("ascii") != entry.hash.value:
                raise ValueError(f"{name}: installed file differs from its recorded digest: {entry}")
            dest = "lib/" + entry.as_posix()
            if dest in files and files[dest] != data:
                raise ValueError(f"Conflicting distributions provide {dest}")
            files[dest] = data
    return versions


def build(output):
    files = {}
    sources = [STUDIO / name for name in
               ("kvmpack.py", "extension_studio.py", "card.py", "paired_install.py", "preview.html", "autopilot_preview.js")]
    sources.extend(STUDIO.joinpath("schemas").glob("*.json"))
    sources.extend(p for p in STUDIO.joinpath("examples").rglob("*") if p.suffix.lower() in {".json", ".png"})
    sources.extend(REPO.joinpath("extensions", "usb-monitor-console").glob("*.json"))
    sources.extend(STUDIO.joinpath("samples", "rate-slider").glob("*.json"))
    sources.extend(p for p in REPO.joinpath("extensions", "pilot-panel").rglob("*") if p.suffix.lower() in {".json", ".png"})
    for source in sources:
        files[source.relative_to(REPO).as_posix()] = source.read_bytes()
    files["docs/EXTENSION-BINDINGS.md"] = (REPO / "docs/EXTENSION-BINDINGS.md").read_bytes()
    for source in (HERE / "start.py", HERE / "README.md", REPO / "LICENSE.md"):
        files[source.name] = source.read_bytes()
    runtime = {"target": python_target(), "packages": runtime_files(files)}
    files["runtime.json"] = (json.dumps(runtime, indent=2, sort_keys=True) + "\n").encode()
    checksums = [f"{hashlib.sha256(files[p]).hexdigest()}  {p}\n" for p in sorted(files)]
    files["SHA256SUMS"] = "".join(checksums).encode()
    archive = BytesIO()
    with ZipFile(archive, "w") as z:
        for name, data in sorted(files.items()):
            info = ZipInfo("KVMpad-Extension-SDK/" + name, (1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            z.writestr(info, data, compress_type=ZIP_DEFLATED, compresslevel=9)
    write_file(output, archive.getvalue())
    print(f"{output}: {len(files)} files, SHA-256 {hashlib.sha256(archive.getvalue()).hexdigest()}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="new ZIP path")
    build(parser.parse_args().output)
