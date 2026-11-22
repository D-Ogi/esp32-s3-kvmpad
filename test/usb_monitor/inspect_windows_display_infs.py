# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Read DriverStore INF text for indirect display extensions and possible USB child routes."""

import argparse
import hashlib
import json
import os
import re
from pathlib import Path


def values(lines, key):
    return [value.strip() for left, sep, value in (line.partition("=") for line in lines)
            if sep and left.strip().lower() == key]


def inspect(store):
    clients, includes, children = [], [], []
    paths = sorted(store.glob("*/*.inf"))
    if not paths:
        raise ValueError(f"No driver package INFs under {store}")
    for path in paths:
        data = path.read_bytes()
        if data.startswith((b"\xff\xfe", b"\xfe\xff")):
            source = data.decode("utf-16")
        else:
            try:
                source = data.decode("utf-8-sig")
            except UnicodeDecodeError:
                source = data.decode("latin-1")
        lines = [line.split(";", 1)[0].strip() for line in source.splitlines()]
        extensions = values(lines, "umdfextensions")
        if any(re.search(r"\bIddCx\w*", value, re.I) for value in extensions):
            models = [line for line in lines if line.startswith("%") and re.search(
                r"=\s*[^,]+,\s*(?:USB\\|WFD\\|ROOT\\|RdpIdd_IndirectDisplay\b)", line, re.I)]
            clients.append(dict(inf=path.name, sha256=hashlib.sha256(data).hexdigest(),
                                extensions=extensions, matching_lines=models))
        for value in values(lines, "include"):
            includes.extend((path.name, name.strip().strip('"').lower()) for name in value.split(","))
        if values(lines, "addcomponent") and any(re.search(r"\b(?:USB|HID)\\", line, re.I) for line in lines):
            children.append(dict(inf=path.name, component_ids=values(lines, "componentids")))
    names = {client["inf"].lower() for client in clients}
    return dict(inf_files=len(paths), iddcx_clients=clients,
                includes_iddcx_client=sorted({src for src, dest in includes if dest in names}),
                usb_or_hid_addcomponent=children)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("store", type=Path, nargs="?", default=Path(
        os.environ.get("SystemRoot", "C:/Windows")) / "System32/DriverStore/FileRepository")
    print(json.dumps(inspect(parser.parse_args().store), indent=2))
