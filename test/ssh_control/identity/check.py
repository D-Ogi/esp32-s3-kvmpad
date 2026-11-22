# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Cross-check in-memory provisioning against the OpenSSH public blob wire format."""
import base64
import hashlib
import os
import subprocess
import sys


def check():
    env = dict(os.environ)
    env["PATH"] = sys.argv[2] + os.pathsep + env["PATH"]
    result = subprocess.run([sys.argv[1]], env=env, capture_output=True, text=True, check=True)
    public, fingerprint, status = result.stdout.splitlines()
    assert public.startswith("PUBLIC ") and fingerprint.startswith("DIGEST ")
    blob = base64.b64decode(public[7:], validate=True)
    expected_prefix = bytes.fromhex("0000000b") + b"ssh-ed25519" + bytes.fromhex("00000020")
    assert blob.startswith(expected_prefix) and len(blob) == len(expected_prefix) + 32
    assert hashlib.sha256(blob).hexdigest() == fingerprint[7:]
    print(status)
    return blob


if __name__ == "__main__":
    first = check()
    second = check()
    assert first != second
    print("PASS independent SHA-256 and distinct identities for two fresh stores")
