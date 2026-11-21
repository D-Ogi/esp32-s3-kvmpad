# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Desktop reference for one binary media message in an already authorized stream."""

from dataclasses import dataclass
import struct
import zlib


@dataclass(frozen=True)
class Format:
    magic: bytes
    version: int
    kind: int
    width: int
    height: int


FORMATS = {
    "jpeg": Format(b"KVS1", 1, 1, 512, 512),
    "h264-intra": Format(b"KVS2", 2, 2, 512, 512),
    "cinepak-intra": Format(b"KVS2", 2, 3, 960, 540),
}
HEADER = struct.Struct("<4sBBBBIIIHHI")
CRC = struct.Struct("<I")
MAX_PAYLOAD = 96 * 1024


def negotiate(preferences, offered):
    """Control-channel selection. The caller supplies the receiver's measured capabilities."""
    return next((name for name in preferences if name in FORMATS and name in offered), None)


def encode(codec, stream, sequence, timestamp_ms, payload):
    fmt = FORMATS[codec]
    if not stream or not 0 < len(payload) <= MAX_PAYLOAD:
        raise ValueError("invalid stream or payload size")
    prefix = HEADER.pack(fmt.magic, fmt.version, fmt.kind, 1, 0, stream, sequence,
                         timestamp_ms, fmt.width, fmt.height, len(payload))
    return prefix + CRC.pack(zlib.crc32(payload, zlib.crc32(prefix))) + payload


class Receiver:
    """One negotiated stream. A stopped receiver is terminal, including after reconnect."""

    def __init__(self, codec, stream):
        if not 0 < stream <= 0xffffffff:
            raise ValueError("stream id out of range")
        self.format = FORMATS[codec]
        self.stream = stream
        self.sequence = None

    def stop(self):
        self.stream = None

    def receive(self, message):
        header_size = HEADER.size + CRC.size
        if not header_size < len(message) <= header_size + MAX_PAYLOAD:
            raise ValueError("message size")
        magic, version, kind, flags, reserved, stream, seq, _, w, h, size = HEADER.unpack_from(message)
        valid_flags = flags == 1 or (magic == b"KVS1" and flags == 0)
        if Format(magic, version, kind, w, h) != self.format or not valid_flags or reserved:
            raise ValueError("unexpected stream format")
        if stream != self.stream or size != len(message) - header_size:
            raise ValueError("stream id or payload length")
        if self.sequence is not None and not 0 < ((seq - self.sequence) & 0xffffffff) < 0x80000000:
            raise ValueError("stale sequence")
        data = memoryview(message)[header_size:]
        checksum, = CRC.unpack_from(message, HEADER.size)
        if zlib.crc32(data, zlib.crc32(message[:HEADER.size])) != checksum:
            raise ValueError("frame CRC")
        self.sequence = seq
        return data
