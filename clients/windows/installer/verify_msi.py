#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Validate a CLIENT MSI and compare its embedded files to the publication without installing it."""

import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile
import xml.etree.ElementTree as ET


def sha256(path):
    checksum = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.digest()


def verify(msi, payload, wix):
    subprocess.run([wix, "msi", "validate", str(msi)], check=True)
    with tempfile.TemporaryDirectory(prefix="kvmpad-verify-", dir=msi.parent) as work:
        temporary = Path(work)
        dump = temporary / "tables.wxs"
        extracted = temporary / "cabinet"
        subprocess.run([wix, "msi", "decompile", str(msi), "-out", str(dump), "-x", str(extracted)],
                       check=True)
        package = ET.parse(dump).getroot().find("{*}Package")
        install = package.find("{*}StandardDirectory[@Id='ProgramFiles64Folder']/{*}Directory[@Id='INSTALLFOLDER']")
        if install is None:
            raise ValueError("missing x64 Program Files installation directory")
        shortcuts = package.findall(".//{*}Shortcut")
        if len(shortcuts) != 1 or shortcuts[0].get("Directory") != "ProgramMenuFolder":
            raise ValueError("expected exactly one shortcut in the Start menu")
        media = package.findall("{*}Media")
        if not media or any(row.get("EmbedCab") != "yes" for row in media):
            raise ValueError("expected all cabinets to be embedded")

        contents = set()

        def visit(directory, relative):
            for child in directory.findall("{*}Directory"):
                subdir = relative / child.attrib["Name"]
                contents.add(subdir)
                visit(child, subdir)
            for file in directory.findall("{*}Component/{*}File"):
                path = relative / file.attrib["Name"]
                contents.add(path)
                if sha256(payload / path) != sha256(extracted / "File" / file.attrib["Id"]):
                    raise ValueError(f"embedded file differs: {path}")

        visit(install, Path())
        source = {path.relative_to(payload) for path in payload.rglob("*")}
        if contents != source:
            raise ValueError(f"missing: {source - contents}; unexpected: {contents - source}")
        print(f"Verified {sum((payload / p).is_file() for p in contents)} files in {msi.name}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--msi", required=True, type=Path)
    parser.add_argument("--payload", required=True, type=Path)
    parser.add_argument("--wix", default="wix")
    args = parser.parse_args()
    verify(args.msi.resolve(), args.payload.resolve(), args.wix)
