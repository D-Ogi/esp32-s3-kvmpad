# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""LAN probe of the DEVICE remote transport (TASK-100, ADR-032) from the test PC.

Needs no pairing, no token and no physical presence: it checks only what the DEVICE shows to any peer on
the LAN. Nothing it sends can reach the HOST - every request is refused before a WebSocket exists.

  mdns                      `_kvmpad._tcp.local` PTR / SRV / TXT / A (FR-723, SEC-103 `pair=` flag)
  tls   --host H            TLS version, cipher, certificate subject and its SHA-256 pin (ADR-028 §3)
  admit --host H            HTTP Upgrade answers before the handshake (ADR-032 §2, §2a)
  plain --host H            no plain-text listener (TECH-040: no ws:// in a release build)

Every result is one `KEY value` line; raw output can be saved with `--save DIR` (file name + SHA-256 printed).
No token, code or payload is ever printed: none is used.
"""
from __future__ import annotations

import argparse
import base64
import datetime as _dt
import hashlib
import io
import os
import socket
import ssl
import struct
import sys
import time

MDNS_ADDR = ("224.0.0.251", 5353)
SERVICE = "_kvmpad._tcp.local"
DEFAULT_PORT = 8443


# ---- mDNS ---------------------------------------------------------------------------------------
def _qname(name: str) -> bytes:
    out = b""
    for part in name.rstrip(".").split("."):
        out += bytes([len(part)]) + part.encode()
    return out + b"\x00"


def _read_name(buf: bytes, off: int) -> tuple[str, int]:
    labels = []
    jumped = False
    end = off
    guard = 0
    while True:
        guard += 1
        if guard > 64:
            raise ValueError("name loop")
        n = buf[off]
        if n == 0:
            off += 1
            break
        if n & 0xC0 == 0xC0:
            ptr = ((n & 0x3F) << 8) | buf[off + 1]
            if not jumped:
                end = off + 2
            jumped = True
            off = ptr
            continue
        labels.append(buf[off + 1 : off + 1 + n].decode(errors="replace"))
        off += 1 + n
    return ".".join(labels), (end if jumped else off)


def _parse(buf: bytes) -> list[dict]:
    _id, _flags, qd, an, ns, ar = struct.unpack("!HHHHHH", buf[:12])
    off = 12
    for _ in range(qd):
        _, off = _read_name(buf, off)
        off += 4
    recs = []
    for _ in range(an + ns + ar):
        name, off = _read_name(buf, off)
        rtype, _rclass, ttl, rdlen = struct.unpack("!HHIH", buf[off : off + 10])
        off += 10
        rdata = buf[off : off + rdlen]
        rec = {"name": name, "type": rtype, "ttl": ttl}
        if rtype == 12:  # PTR
            rec["ptr"], _ = _read_name(buf, off)
        elif rtype == 33:  # SRV
            _prio, _w, port = struct.unpack("!HHH", rdata[:6])
            rec["port"] = port
            rec["target"], _ = _read_name(buf, off + 6)
        elif rtype == 16:  # TXT
            items, i = [], 0
            while i < len(rdata):
                ln = rdata[i]
                items.append(rdata[i + 1 : i + 1 + ln].decode(errors="replace"))
                i += 1 + ln
            rec["txt"] = items
        elif rtype == 1 and rdlen == 4:  # A
            rec["a"] = socket.inet_ntoa(rdata)
        recs.append(rec)
        off += rdlen
    return recs


def cmd_mdns(args, out) -> int:
    # QU (unicast reply) by default; --multicast asks for a multicast answer and listens on 5353 in the group,
    # for hosts whose firewall drops an unsolicited unicast datagram to an ephemeral port.
    qclass = 0x0001 if args.multicast else 0x8001
    q = struct.pack("!HHHHHH", 0, 0, 1, 0, 0, 0) + _qname(SERVICE) + struct.pack("!HH", 12, qclass)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
    if args.iface:
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(args.iface))
    if args.multicast:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("", 5353))
        mreq = socket.inet_aton(MDNS_ADDR[0]) + socket.inet_aton(args.iface or "0.0.0.0")
        s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
    else:
        s.bind(("", 0))
    s.settimeout(0.5)
    found: dict[str, dict] = {}
    deadline = time.monotonic() + args.timeout
    sent = 0
    while time.monotonic() < deadline:
        if sent < 3:
            s.sendto(q, MDNS_ADDR)
            sent += 1
        try:
            buf, src = s.recvfrom(4096)
        except socket.timeout:
            continue
        try:
            recs = _parse(buf)
        except (ValueError, IndexError, struct.error):
            continue
        for r in recs:
            r["src"] = src[0]
            key = f"{r['type']}:{r['name']}"
            found[key] = r
    ptrs = [r for r in found.values() if r["type"] == 12 and r["name"].lower() == SERVICE.lower()]
    print(f"MDNS queries={sent} records={len(found)} instances={len(ptrs)}", file=out)
    for r in found.values():
        if r["type"] == 12:
            print(f"MDNS PTR {r['name']} -> {r['ptr']} ttl={r['ttl']} src={r['src']}", file=out)
        elif r["type"] == 33:
            print(f"MDNS SRV {r['name']} port={r['port']} target={r['target']} ttl={r['ttl']}", file=out)
        elif r["type"] == 16:
            print(f"MDNS TXT {r['name']} {' '.join(r['txt'])}", file=out)
        elif r["type"] == 1 and "a" in r:
            print(f"MDNS A {r['name']} {r['a']}", file=out)
    return 0 if ptrs else 1


# ---- TLS ----------------------------------------------------------------------------------------
def _ctx() -> ssl.SSLContext:
    # The pin is checked by the caller against the console value; there is no CA for a self-signed device.
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    return ctx


def cmd_tls(args, out) -> int:
    t0 = time.monotonic()
    with socket.create_connection((args.host, args.port), timeout=args.timeout) as raw:
        with _ctx().wrap_socket(raw, server_hostname=None) as tls:
            dt = (time.monotonic() - t0) * 1000.0
            der = tls.getpeercert(binary_form=True)
            pin = hashlib.sha256(der).hexdigest()
            print(f"TLS version={tls.version()} cipher={tls.cipher()[0]} handshake_ms={dt:.0f}", file=out)
            try:
                from cryptography import x509

                c = x509.load_der_x509_certificate(der)
                pk = c.public_key()
                curve = getattr(getattr(pk, "curve", None), "name", type(pk).__name__)
                print(
                    f"TLS subject={c.subject.rfc4514_string()} issuer={c.issuer.rfc4514_string()} key={curve} "
                    f"not_before={c.not_valid_before_utc:%Y-%m-%d} not_after={c.not_valid_after_utc:%Y-%m-%d}",
                    file=out,
                )
            except ImportError:
                pass
            print(f"TLS cert_len={len(der)} cert_sha256={pin}", file=out)
    if args.expect_sha256:
        ok = pin == args.expect_sha256.lower()
        print(f"TLS pin_match={'yes' if ok else 'NO'}", file=out)
        return 0 if ok else 1
    return 0


def _upgrade(host: str, port: int, path: str, proto: str | None, auth: str | None, timeout: float) -> str:
    key = base64.b64encode(os.urandom(16)).decode()
    lines = [
        f"GET {path} HTTP/1.1",
        f"Host: {host}:{port}",
        "Upgrade: websocket",
        "Connection: Upgrade",
        f"Sec-WebSocket-Key: {key}",
        "Sec-WebSocket-Version: 13",
    ]
    if proto:
        lines.append(f"Sec-WebSocket-Protocol: {proto}")
    if auth:
        lines.append(f"Authorization: {auth}")
    req = ("\r\n".join(lines) + "\r\n\r\n").encode()
    with socket.create_connection((host, port), timeout=timeout) as raw:
        with _ctx().wrap_socket(raw, server_hostname=None) as tls:
            tls.sendall(req)
            data = b""
            try:
                while b"\r\n\r\n" not in data and len(data) < 4096:
                    chunk = tls.recv(1024)
                    if not chunk:
                        break
                    data += chunk
            except (socket.timeout, ssl.SSLError, ConnectionError):
                pass
    head = data.split(b"\r\n", 1)[0].decode(errors="replace")
    return head or "<no answer>"


def cmd_admit(args, out) -> int:
    # Only refusals are expected here; a bogus bearer is a fixed 43-char string, never a real token.
    bogus = "Bearer " + "A" * 43
    cases = [
        ("ws_no_auth", "/v1/ws", "kvmpad.v1", None),
        ("ws_bogus_token", "/v1/ws", "kvmpad.v1", bogus),
        ("ws_no_subprotocol", "/v1/ws", None, None),
        ("pair_no_window", "/v1/pair", "kvmpad.pair.v1", None),
        ("pair_no_subprotocol", "/v1/pair", None, None),
        ("unknown_uri", "/v1/nothing", "kvmpad.v1", None),
    ]
    upgraded = 0
    for name, path, proto, auth in cases:
        head = _upgrade(args.host, args.port, path, proto, auth, args.timeout)
        if " 101 " in head + " ":
            upgraded += 1
        print(f"ADMIT {name} path={path} subprotocol={proto or '-'} -> {head}", file=out)
    print(f"ADMIT upgraded={upgraded}", file=out)
    return 0 if upgraded == 0 else 1


def cmd_plain(args, out) -> int:
    bad = 0
    for port in (80, 81, 8080, 8000):
        try:
            with socket.create_connection((args.host, port), timeout=2.0):
                print(f"PLAIN tcp/{port} open", file=out)
                bad += 1
        except OSError as e:
            print(f"PLAIN tcp/{port} closed ({type(e).__name__})", file=out)
    # Plain HTTP on the TLS port must not be answered as HTTP.
    try:
        with socket.create_connection((args.host, args.port), timeout=args.timeout) as s:
            s.sendall(b"GET /v1/ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n")
            s.settimeout(3.0)
            try:
                data = s.recv(256)
            except (socket.timeout, ConnectionError):
                data = b""
        http = data.startswith(b"HTTP/")
        print(f"PLAIN tls_port_plain_http answered_http={'yes' if http else 'no'} bytes={len(data)}", file=out)
        bad += 1 if http else 0
    except OSError as e:
        print(f"PLAIN tls_port connect failed ({type(e).__name__})", file=out)
    return 0 if bad == 0 else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--timeout", type=float, default=6.0)
    ap.add_argument("--save", help="directory for the raw output (outside the repository)")
    ap.add_argument("--name", default="remote_probe")
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("mdns")
    m.add_argument("--iface", help="local IPv4 address to send the query from")
    m.add_argument("--multicast", action="store_true", help="ask for a multicast answer, listen on 5353")
    for n in ("tls", "admit", "plain"):
        p = sub.add_parser(n)
        p.add_argument("--host", required=True)
        p.add_argument("--port", type=int, default=DEFAULT_PORT)
        if n == "tls":
            p.add_argument("--expect-sha256")
    args = ap.parse_args()
    buf = io.StringIO()
    fn = {"mdns": cmd_mdns, "tls": cmd_tls, "admit": cmd_admit, "plain": cmd_plain}[args.cmd]
    try:
        rc = fn(args, buf)
    except OSError as e:
        print(f"ERROR {type(e).__name__}: {e}", file=buf)
        rc = 2
    text = buf.getvalue()
    sys.stdout.write(text)
    if args.save:
        os.makedirs(args.save, exist_ok=True)
        stamp = _dt.datetime.now().strftime("%Y%m%d_%H%M%S")
        path = os.path.join(args.save, f"{stamp}_{args.name}_{args.cmd}.log")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print(f"SAVED {path} sha256={hashlib.sha256(text.encode()).hexdigest()}")
    print(f"RESULT rc={rc}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
