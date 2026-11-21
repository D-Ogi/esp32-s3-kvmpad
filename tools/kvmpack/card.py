# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Two-slot storage for validated packs on a mounted card. Grants live on the device."""

from contextlib import contextmanager
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import re
import struct
import tempfile
import zlib


PREFIX = struct.Struct("<4sQ32s")
CRC = struct.Struct("<I")
# KPH1 records include a pending digest, boot ID and two failure times between
# the quarantined digest and the count/disabled flags (pack_store.c).
HEALTH = struct.Struct("<4sQ32s44xBB2sI")
MAX_ARCHIVE = 256 * 1024


def write_file(path, data, replace=False):
    """Publish flushed bytes. Installer publication never overwrites a previous file."""
    path = Path(path)
    stream = tempfile.NamedTemporaryFile(dir=path.parent, prefix=".kvmpad-", suffix=".tmp", delete=False)
    temporary = Path(stream.name)
    try:
        with stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        if replace:
            os.replace(temporary, path)
        elif os.name == "nt":
            os.rename(temporary, path)
        else:
            # FAT cannot hard-link. Reserve the destination exclusively before atomic publication.
            with path.open("xb"):
                pass
            try:
                os.replace(temporary, path)
            except BaseException:
                path.unlink(missing_ok=True)
                raise
    finally:
        temporary.unlink(missing_ok=True)


def stage_installer(root, manifest, data):
    """Place a validated archive in the device picker, without writing a device index or grant."""
    root = Path(root)
    if not root.is_dir():
        raise FileNotFoundError("Card mount is unavailable.")
    profile, version = manifest["id"], manifest["version"]
    if (not re.fullmatch(r"[a-z][a-z0-9-]*(\.[a-z][a-z0-9-]*){2,}", profile) or
            not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version) or not 0 < len(data) <= MAX_ARCHIVE):
        raise ValueError("Invalid installer identity or size.")
    identity = f"{profile}-{version}"
    name = "pack-" + identity + ".kvmpack"
    if len(name) > 100:
        suffix = hashlib.sha256(identity.encode("ascii")).hexdigest()[:16]
        name = f"pack-{profile[:48]}-{version[:16]}-{suffix}.kvmpack"
    path = root / name
    write_file(path, data)
    with path.open("rb") as stream:
        if stream.read(len(data)+1) != data:
            raise OSError("Card readback differed from the validated installer.")
    return path


@dataclass(frozen=True)
class Snapshot:
    generation: int
    digest: bytes
    slot: str
    payload: bytes


def card_path(root, profile, create=False):
    directory = Path(root)
    if not directory.is_dir():
        raise FileNotFoundError("card mount is unavailable")
    # Short FAT names also work with the firmware's default 8.3 filename configuration.
    name = hashlib.sha256(profile.encode("ascii")).hexdigest()
    for part in ("KVPACKS", *(name[i:i + 8] for i in range(0, len(name), 8))):
        directory /= part
        if directory.is_symlink() or getattr(directory, "is_junction", lambda: False)():
            raise ValueError("link in card path")
        if create:
            directory.mkdir(exist_ok=True)
    return directory


def member(directory, name):
    path = directory / name
    if path.is_symlink() or (path.exists() and not path.is_file()):
        raise ValueError("invalid card member")
    return path


def read_index(directory, slot):
    try:
        with member(directory, f"{slot}.IDX").open("rb") as f:
            record = f.read(PREFIX.size + CRC.size + 1)
    except FileNotFoundError:
        return None
    if len(record) != PREFIX.size + CRC.size:
        return None
    magic, generation, digest = PREFIX.unpack_from(record)
    checksum, = CRC.unpack_from(record, PREFIX.size)
    if magic != b"KVP1" or not generation or zlib.crc32(record[:PREFIX.size]) != checksum:
        return None
    return generation, digest


def read_slot(directory, slot, index=None):
    index = index or read_index(directory, slot)
    if index is None:
        return None
    generation, digest = index
    try:
        with member(directory, f"{slot}.KVP").open("rb") as f:
            data = f.read(MAX_ARCHIVE + 1)
    except FileNotFoundError:
        return None
    if not 0 < len(data) <= MAX_ARCHIVE or hashlib.sha256(data).digest() != digest:
        return None
    return Snapshot(generation, digest, slot, data)


def quarantined_digests(directory):
    blocked = set()
    for slot in ("A", "B"):
        newest, digest = 0, None
        for copy in (0, 1):
            try:
                with member(directory, f"{slot}.H{copy}").open("rb") as f:
                    record = f.read(HEALTH.size + 1)
            except FileNotFoundError:
                continue
            if len(record) != HEALTH.size:
                continue
            magic, sequence, failed, count, disabled, reserved, checksum = HEALTH.unpack(record)
            if (magic == b"KPH1" and sequence > newest and sequence % 2 == copy and count <= 2 and
                    disabled <= 1 and reserved == b"\0\0" and zlib.crc32(record[:-CRC.size]) == checksum):
                newest = sequence
                digest = failed if disabled else None
        if digest is not None:
            blocked.add(digest)
    return blocked


def latest(directory, incoming=None):
    if member(directory, "OFF.IDX").exists():
        return None
    blocked = quarantined_digests(directory)
    if incoming in blocked:
        raise ValueError("card archive is quarantined")
    first, second = read_index(directory, "A"), read_index(directory, "B")
    if first and second and first[0] == second[0]:
        raise ValueError("ambiguous card generation")
    indexes = [(index, slot) for index, slot in ((first, "A"), (second, "B")) if index is not None]
    for index, slot in sorted(indexes, reverse=True):
        if index[1] in blocked:
            continue
        snapshot = read_slot(directory, slot, index)
        if snapshot is not None:
            return snapshot
    return None


def recover(root, profile):
    """Return a storage candidate for revalidation. None selects the firmware Home screen."""
    try:
        return latest(card_path(root, profile))
    except (OSError, ValueError):
        return None


@contextmanager
def writer(directory):
    # File locks are released by the OS on exit, including an interrupted writer.
    with member(directory, "WRITER.LCK").open("a+b") as f:
        if os.name == "nt":
            import msvcrt
            f.seek(0)
            msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield


def write_synced(path, data):
    with path.open("wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())


def stage(root, profile, data):
    """Store the validated payload in the inactive slot, then publish its index."""
    if not 0 < len(data) <= MAX_ARCHIVE:
        raise ValueError("card payload length")
    directory = card_path(root, profile, create=True)
    digest = hashlib.sha256(data).digest()
    with writer(directory):
        old = latest(directory, incoming=digest)
        if old and old.digest == digest:
            return old
        slot = "B" if old and old.slot == "A" else "A"
        generation = old.generation + 1 if old else 1
        if generation >= 1 << 64:
            raise ValueError("card generation exhausted")
        tombstone = member(directory, "OFF.IDX")
        if tombstone.exists():
            # Finish a user-confirmed removal before publishing a fresh install.
            member(directory, "PAUSE.IDX").unlink(missing_ok=True)
            for previous_slot in ("A", "B"):
                for extension in ("IDX", "KVP", "GRT", "H0", "H1"):
                    member(directory, f"{previous_slot}.{extension}").unlink(missing_ok=True)
            tombstone.unlink()
        write_synced(member(directory, slot + ".KVP"), data)
        record = PREFIX.pack(b"KVP1", generation, digest)
        write_synced(member(directory, slot + ".IDX"), record + CRC.pack(zlib.crc32(record)))
        result = read_slot(directory, slot)
        if result is None or result.digest != digest or result.generation != generation:
            raise OSError("card verification failed")
        return result
