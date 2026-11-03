# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Bench HTTPS server for the signed-OTA board plan (docs/OTA.md, ADR-031).

Serves the files of one directory over TLS with a declared Content-Length (the device refuses a chunked or
length-less response). The certificate and its key live outside the repository (the workspace `keys/`
directory); a development firmware pins the certificate through KVM_OTA_TEST_CERT (main/CMakeLists.txt).
`--rate` throttles the body so a reset can land at a chosen point of the transfer. Standard library only.

    python -I test/hil/ota_serve.py --dir <images> --cert <crt> --key <key> [--port 8443] [--rate 200000]
"""
from __future__ import annotations

import argparse
import http.server
import pathlib
import ssl
import sys
import time


def make_handler(root: pathlib.Path, rate: int):
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self) -> None:  # noqa: N802 (http.server naming)
            name = self.path.lstrip("/").split("?", 1)[0]
            f = (root / name).resolve()
            if f.parent != root or not f.is_file():
                self.send_error(404)
                return
            data = f.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Connection", "close")
            self.end_headers()
            sent = 0
            start = time.monotonic()
            try:
                while sent < len(data):
                    chunk = data[sent:sent + 16384]
                    self.wfile.write(chunk)
                    sent += len(chunk)
                    if rate > 0:
                        ahead = sent / rate - (time.monotonic() - start)
                        if ahead > 0:
                            time.sleep(ahead)
            except (ConnectionError, ssl.SSLError) as e:
                print(f"GET {name}: client gone after {sent}/{len(data)} B ({type(e).__name__})", flush=True)
                return
            print(f"GET {name}: {sent}/{len(data)} B in {time.monotonic() - start:.1f} s", flush=True)

        def log_message(self, fmt: str, *args) -> None:  # one line per request, no headers
            print(f"{self.address_string()} {fmt % args}", flush=True)

    return Handler


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", required=True)
    ap.add_argument("--cert", required=True)
    ap.add_argument("--key", required=True)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8443)
    ap.add_argument("--rate", type=int, default=0, help="bytes per second for the body, 0 = unthrottled")
    args = ap.parse_args()
    root = pathlib.Path(args.dir).resolve()
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(args.cert, args.key)
    srv = http.server.ThreadingHTTPServer((args.bind, args.port), make_handler(root, args.rate))
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    print(f"serving {root} on https://{args.bind}:{args.port} rate={args.rate or 'unthrottled'}", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
