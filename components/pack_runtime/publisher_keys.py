# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Compile an explicitly provisioned bundle of up to eight public Ed25519 PEM keys."""
import argparse
from pathlib import Path
import re

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey


def generate(bundle, header):
    if bundle.stat().st_size > 4096:
        raise ValueError("publisher key bundle exceeds 4096 bytes")
    pem = bundle.read_bytes().strip()
    keys = []
    block = re.compile(rb"-----BEGIN PUBLIC KEY-----\s+[A-Za-z0-9+/=\r\n]+-----END PUBLIC KEY-----")
    while pem:
        match = block.match(pem)
        if not match:
            raise ValueError("expected a PEM SubjectPublicKeyInfo public key")
        key = serialization.load_pem_public_key(match.group())
        if not isinstance(key, Ed25519PublicKey):
            raise ValueError("publisher keys must be Ed25519")
        raw = key.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        if raw in keys or len(keys) == 8:
            raise ValueError("duplicate key or more than eight publisher keys")
        keys.append(raw)
        pem = pem[match.end():].lstrip()
    if not keys:
        raise ValueError("empty publisher key bundle")
    rows = ["    {" + ",".join(f"0x{b:02x}" for b in key) + "}," for key in keys]
    header.write_text("#include <stdint.h>\nstatic const uint8_t pack_publisher_keys[][32] = {\n"
                      + "\n".join(rows) + "\n};\n", encoding="ascii")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("header", type=Path)
    args = parser.parse_args()
    generate(args.bundle, args.header)
