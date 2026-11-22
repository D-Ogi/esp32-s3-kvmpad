#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Build an x64 MSI from a complete Windows CLIENT publish directory using WiX 4.0.6."""

import argparse
import hashlib
from pathlib import Path
import re
import stat
import subprocess
import tempfile
import uuid
import xml.etree.ElementTree as ET

WIX_NAMESPACE = "http://wixtoolset.org/schemas/v4/wxs"
ENTRY_POINT = "KVMpad.Client.exe"


def msi_version(value):
    if not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", value):
        raise argparse.ArgumentTypeError("use a three-part MSI version, e.g. 1.2.3")
    if any(int(part) > limit for part, limit in zip(value.split("."), (255, 255, 65535))):
        raise argparse.ArgumentTypeError("MSI version must be within 255.255.65535")
    return value


def stable_id(prefix, relative):
    identity = relative.as_posix().casefold().encode("utf-8")
    return prefix + hashlib.sha256(identity).hexdigest()[:32]


def harvest(payload, target):
    root = ET.Element("Wix", xmlns=WIX_NAMESPACE)
    fragment = ET.SubElement(root, "Fragment")
    directory = ET.SubElement(fragment, "DirectoryRef", Id="INSTALLFOLDER")
    group = ET.SubElement(fragment, "ComponentGroup", Id="PublishedPayload")

    def component(parent, relative, guid="*"):
        identifier = stable_id("C", relative)
        ET.SubElement(group, "ComponentRef", Id=identifier)
        return ET.SubElement(parent, "Component", Id=identifier, Guid=guid, Bitness="always64")

    def visit(path, parent):
        entries = sorted(path.iterdir(), key=lambda item: item.name.casefold())
        if not entries:
            # Empty directories have no file key path from which WiX can derive a GUID.
            relative = path.relative_to(payload)
            guid = uuid.uuid5(uuid.NAMESPACE_URL, "kvmpad-client:folder:" + relative.as_posix().casefold())
            ET.SubElement(component(parent, relative, str(guid)), "CreateFolder")

        for entry in entries:
            if entry.lstat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT:
                raise ValueError(f"publish physical files and directories instead of reparse points: {entry}")
            relative = entry.relative_to(payload)
            if entry.is_dir():
                child = ET.SubElement(parent, "Directory", Id=stable_id("D", relative), Name=entry.name)
                visit(entry, child)
            elif entry != payload / ENTRY_POINT:
                if entry.name.casefold() == "kvmpad_pairing_test.dll":
                    raise ValueError(f"the pairing test library cannot ship: {entry}")
                ET.SubElement(component(parent, relative), "File", Id=stable_id("F", relative),
                              Source="!(bindpath.payload)\\" + str(relative), KeyPath="yes")

    visit(payload, directory)
    ET.indent(root)
    ET.ElementTree(root).write(target, encoding="utf-8", xml_declaration=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--payload", required=True, type=Path, help="complete self-contained win-x64 publication")
    parser.add_argument("--version", required=True, type=msi_version, help="increase for each new release")
    parser.add_argument("--output", required=True, type=Path, help="destination .msi outside the publish tree")
    parser.add_argument("--wix", default="wix", help="WiX 4 wix.exe (default: wix on PATH)")
    args = parser.parse_args()
    payload = args.payload.resolve()
    output = args.output.resolve()
    if output.suffix.lower() != ".msi" or output.is_relative_to(payload):
        parser.error("output must be an .msi outside the publish directory")
    if not (payload / ENTRY_POINT).is_file():
        parser.error(f"missing published application: {payload / ENTRY_POINT}")
    output.parent.mkdir(parents=True, exist_ok=True)
    # A repeat build of one release identifies the same product, even when built elsewhere.
    product = uuid.uuid5(uuid.NAMESPACE_URL, "kvmpad-client:win-x64:" + args.version)
    with tempfile.TemporaryDirectory(prefix="kvmpad-msi-", dir=output.parent) as stage:
        temporary = Path(stage)
        files = temporary / "Payload.wxs"
        try:
            harvest(payload, files)
        except ValueError as exc:
            parser.error(str(exc))
        package = temporary / output.name
        subprocess.run([args.wix, "build", "-arch", "x64", "-d", "Version=" + args.version,
                        "-d", "ProductCode=" + str(product), "-bindpath", "payload=" + str(payload),
                        "-intermediateFolder", str(temporary / "obj"), "-pdbtype", "none",
                        str(Path(__file__).with_name("Package.wxs")), str(files), "-out", str(package)],
                       check=True)
        package.replace(output)
    print(output)


if __name__ == "__main__":
    main()
