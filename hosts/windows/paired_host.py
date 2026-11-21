# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Read the authenticated CLIENT's public board identity and create one HOST listener identity."""
import base64
import datetime
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import secrets
import socket
import ssl
import tempfile

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID


def read_board(stream):
    line = stream.readline(2049)
    if len(line) > 2048 or not line.endswith(b"\n"):
        raise ValueError("Invalid CLIENT bootstrap")

    def unique(pairs):
        msg = dict(pairs)
        if len(msg) != len(pairs):
            raise ValueError("Duplicate bootstrap field")
        return msg

    msg = json.loads(line.decode("utf-8-sig"), object_pairs_hook=unique)
    if type(msg) is not dict or set(msg) != {"board_der", "board_address"} or any(
            type(v) is not str for v in msg.values()):
        raise ValueError("Invalid CLIENT bootstrap")
    der = base64.b64decode(msg["board_der"], validate=True)
    if not 0 < len(der) <= 1024 or base64.b64encode(der).decode("ascii") != msg["board_der"]:
        raise ValueError("Invalid board certificate encoding")
    cert = x509.load_der_x509_certificate(der)
    if (cert.public_bytes(serialization.Encoding.DER) != der or cert.subject != cert.issuer
            or cert.extensions.get_extension_for_class(x509.BasicConstraints).value.ca
            or not isinstance(cert.public_key(), ec.EllipticCurvePublicKey)):
        raise ValueError("Unexpected board certificate")
    address = ipaddress.IPv4Address(msg["board_address"])
    if address.packed[0] == 0 or address.packed[0] >= 224:
        raise ValueError("Invalid board address")
    return der, address


def route_address(board):
    # UDP connect selects an interface using the Windows route table without sending a datagram.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as route:
        route.connect((str(board), 443))
        local = ipaddress.IPv4Address(route.getsockname()[0])
    if local.is_unspecified or local.is_loopback != board.is_loopback:
        raise ValueError("No HOST route to the paired board")
    return local


def host_identity(address, password):
    key = ec.generate_private_key(ec.SECP256R1())
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "KVMpad HOST")])
    now = datetime.datetime.now(datetime.timezone.utc)
    certificate = (x509.CertificateBuilder().subject_name(subject).issuer_name(subject)
        .public_key(key.public_key()).serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=2))
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .add_extension(x509.SubjectAlternativeName([x509.IPAddress(address)]), critical=False)
        .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
        .add_extension(x509.KeyUsage(True, False, False, False, False, False, False, None, None), critical=True)
        .sign(key, hashes.SHA256()))
    private = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                serialization.BestAvailableEncryption(password.encode("utf-8")))
    return certificate, private


def board_context(board_der):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.verify_mode = ssl.CERT_REQUIRED
    ctx.verify_flags |= ssl.VERIFY_X509_PARTIAL_CHAIN
    ctx.load_verify_locations(cadata=ssl.DER_cert_to_PEM_cert(board_der))
    return hashlib.sha256(board_der).digest(), ctx


def listener_identity(board_der, local):
    pin, context = board_context(board_der)
    password = secrets.token_hex(32)
    certificate, encrypted = host_identity(local, password)
    # CPython's SSLContext accepts certificate chains only by filename. The temporary
    # PKCS#8 is encrypted to an independent in-memory secret and removed before bind.
    fd, name = tempfile.mkstemp(prefix="kvmpad-host-", suffix=".pem")
    try:
        with os.fdopen(fd, "wb") as pem:
            pem.write(certificate.public_bytes(serialization.Encoding.PEM))
            pem.write(encrypted)
        context.load_cert_chain(name, password=password)
    finally:
        Path(name).unlink(missing_ok=True)
    return pin, context, certificate.public_bytes(serialization.Encoding.DER)
