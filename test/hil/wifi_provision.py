# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Put a bench unit on a test Wi-Fi network (TASK-080 HIL, ADR-015) without leaving the password anywhere.

The firmware must be built with CONFIG_KVM_NET_CONSOLE_JOIN=y (bench only, off by default). The SSID and the
password are read from the environment variables KVM_WIFI_SSID and KVM_WIFI_PASS (never from the command line,
which shells keep in their history). The console echoes the typed line, so this script never writes the exchange
to a file and prints only the result line `WIFI_JOIN err=...`. The firmware wipes the arguments and its console
history after the command.

  KVM_WIFI_SSID=... KVM_WIFI_PASS=... python -I test/hil/wifi_provision.py [--port COM3]
"""
from __future__ import annotations

import argparse
import os
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))  # -I does not add the script directory
from kvm_serial import PROMPT, open_port, read_until  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--timeout", type=float, default=10.0)
    args = ap.parse_args()
    ssid = os.environ.get("KVM_WIFI_SSID", "")
    password = os.environ.get("KVM_WIFI_PASS", "")
    if not ssid or any(c.isspace() or c == '"' for c in ssid + password):
        print("set KVM_WIFI_SSID (and KVM_WIFI_PASS); spaces and quotes are not supported", file=sys.stderr)
        return 2
    s = open_port(args.port)
    try:
        s.reset_input_buffer()
        line = f"wifi join {ssid} {password}" if password else f"wifi join {ssid}"
        s.write((line + "\r\n").encode())
        s.flush()
        line = ""
        out, ok = read_until(s, r"(?m)^" + re.escape(PROMPT), args.timeout)
    finally:
        s.close()
    m = re.search(r"WIFI_JOIN err=\S+", out)
    out = ""
    if not ok or m is None:
        print("WIFI_JOIN no answer (is the firmware built with CONFIG_KVM_NET_CONSOLE_JOIN?)")
        return 1
    print(m.group(0))
    return 0 if m.group(0).endswith("ESP_OK") else 1


if __name__ == "__main__":
    sys.exit(main())
