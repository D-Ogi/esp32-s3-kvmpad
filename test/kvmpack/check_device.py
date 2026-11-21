# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Exercise the device archive/scene reader and A/B storage on a temporary mounted directory."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import shutil
import uuid
import zipfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/kvmpack"))
import card
import kvmpack


def native_png_objects(cc, work):
    """Build the same pinned libpng sources as the ESP component."""
    src = ROOT / "managed_components/espressif__libpng/libpng"
    shutil.copyfile(src / "scripts/pnglibconf.h.prebuilt", work / "pnglibconf.h")
    sources = sorted(path for path in src.glob("png*.c") if path.stem != "pngtest")
    subprocess.run([cc, "-O2", "-std=gnu11", "-I", work, "-I", src, "-c", *sources], cwd=work, check=True)
    return ["-I", work, "-I", src, *(work / (p.stem + ".o") for p in sources)]


def check_publishers(base, compiler, default, runtime, files):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey
    from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey

    directory = base / "trust"
    directory.mkdir()
    mount = directory / "card"
    mount.mkdir()
    trusted, second, foreign = (Ed25519PrivateKey.generate() for _ in range(3))
    def pem(key):
        return key.public_key().public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)

    bundle = directory / "publishers.pem"
    header = directory / "pack_publisher_keys.h"
    generate = [sys.executable, runtime / "publisher_keys.py", bundle, header]
    secret = trusted.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                   serialization.NoEncryption())
    for bad in (b"", pem(trusted) + pem(trusted), pem(trusted) + b"garbage", secret, pem(X25519PrivateKey.generate())):
        bundle.write_bytes(bad)
        assert subprocess.run(generate, capture_output=True).returncode != 0
    # Include malformed compressed points to ensure even an incorrectly provisioned key fails closed.
    identity = b"\x01" + bytes(31)
    other_bad = (bytes(32), identity, (2**255 - 18).to_bytes(32, "little"), b"\xff" * 32)
    invalid_keys = b"".join(Ed25519PublicKey.from_public_bytes(raw).public_bytes(
        serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo) for raw in other_bad)
    bundle.write_bytes(pem(second) + pem(trusted) + invalid_keys)
    subprocess.run(generate, check=True)
    exe = directory / default.name
    subprocess.run([*compiler, "-DKVM_PACK_HAS_PUBLISHERS=1", "-I", directory, runtime / "pack_publisher.c",
                    "-o", exe], check=True)
    package = mount / "signed.kvmpack"
    def run(binary, command, path, ok):
        p = subprocess.run([binary, command, path], cwd=directory, capture_output=True, text=True)
        assert p.returncode == (0 if ok else 1), (command, path, p.returncode, p.stdout, p.stderr)
        return p.stdout

    def reject(sig, payload=files):
        out = io.BytesIO()
        with zipfile.ZipFile(io.BytesIO(kvmpack.build(payload)), "r") as unsigned:
            with zipfile.ZipFile(out, "w") as z:
                for row in unsigned.infolist():
                    z.writestr(row, unsigned.read(row))
                z.writestr(kvmpack.SIGNATURE, sig)
        package.write_bytes(out.getvalue())
        run(exe, "install", package, False)
        run(exe, "invalid-integrity", package, True)

    package.write_bytes(kvmpack.build(files))
    assert "unsigned" in run(exe, "tap", package, True)
    package.write_bytes(kvmpack.build(files, trusted))
    run(default, "install", package, False)
    pub = trusted.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    fingerprint = "publisher=" + hashlib.sha256(pub).hexdigest()
    assert fingerprint in run(exe, "install", package, True)
    installed = card.card_path(mount, json.loads(files["manifest.json"])["id"]).relative_to(directory).as_posix()
    assert fingerprint in run(exe, "recover", installed, True)
    run(default, "recover", installed, False)
    package.write_bytes(kvmpack.build(files, foreign))
    run(exe, "tap", package, False)
    package.write_bytes(kvmpack.build(files, second))
    assert "publisher=" in run(exe, "tap", package, True)
    sig = trusted.sign(kvmpack.signature_input(files))
    order = 2**252 + 27742317777372353535851937790883648493
    for bad in (sig[:-1], sig + b"\0", b"\0"*64, identity + bytes(32), sig[:32] + order.to_bytes(32, "little"),
                sig[:32] + (int.from_bytes(sig[32:], "little") + order).to_bytes(32, "little"),
                bytes([sig[0] ^ 1]) + sig[1:]):
        reject(bad)
    reject(sig, {**files, "ui.json": files["ui.json"] + b" "})
    assert fingerprint in run(exe, "recover", installed, True)
    from check_assets import check_assets, rgba_png
    signed_assets = {**files, "assets/valid.png": rgba_png(13, 9, True)}
    signature_workspace = 64 + len(kvmpack.signature_input(signed_assets))
    check_interrupted_updates(directory, files, exe, kvmpack.build(signed_assets, trusted),
                              ("png-memory", f"publisher-memory-{signature_workspace}",
                               "signature-hash", "key-fingerprint"))
    check_assets(directory, compiler, exe, default, trusted, files, run)
    print("Signed pack checks passed for two provisioned publishers, empty trust, revocation, tampering and storage.")


def check_category_pages(exe, base, members):
    directory = base / "pagination"
    mount = directory / "card"
    mount.mkdir(parents=True)
    expected = {"installed": set(), "installers": set(), "video": set()}

    def page(kind, number):
        result = subprocess.run([exe, "page", kind, str(number)], cwd=directory, check=True,
                                capture_output=True, text=True)
        header, *lines = result.stdout.splitlines()
        count, more = map(int, header.split())
        assert len(lines) == count <= 8 and more in (0, 1), (kind, number, header, lines)
        return [tuple(line.split("\t")) for line in lines], bool(more)

    for kind in (*expected, "all", "legacy"):
        assert page(kind, 0) == ([], False)

    # Interleave root entries. Their enumeration order is filesystem-dependent.
    for i in range(17):
        video = f"clip{i:02d}" + (".Mp4", ".264", ".H264")[i % 3]
        (mount / video).touch()
        flags = "011" if video.lower().endswith(".mp4") else "010"
        expected["video"].add((flags, f"card/{video}", video))
        if i < 10:
            package = f"import{i:02d}" + (".kvmpack", ".KVMPACK")[i % 2]
            (mount / package).write_bytes(kvmpack.build(members))
            expected["installers"].add(("000", f"card/{package}", package))
        if i < 16:
            profile = f"org.kvmpad.test.page{i:02d}"
            title = f"Installed profile {i:02d}"
            manifest = {**json.loads(members["manifest.json"]), "id": profile, "title": title}
            payload = kvmpack.build({**members, "manifest.json": json.dumps(manifest).encode()})
            card.stage(mount, profile, payload)
            path = card.card_path(mount, profile).relative_to(directory).as_posix()
            expected["installed"].add(("100", path, title))

    # Exclude unrelated extensions, suffix-only names, package-like directories,
    # hidden installation internals and root-eligible names in arbitrary subdirectories.
    for name in ("notes.txt", "clip.mp4.bak", "import.kvmpack.tmp", ".kvmpack", ".h264", "A.IDX", "B.KVP",
                 "z" * 97 + ".mp4"):
        (mount / name).touch()
    for name in ("folder.kvmpack", "folder.mp4", "private"):
        nested = mount / name
        nested.mkdir()
        (nested / "not-listed.kvmpack").touch()
        (nested / "not-listed.mp4").touch()
    removed = "org.kvmpad.test.removed"
    manifest = {**json.loads(members["manifest.json"]), "id": removed, "title": "Removed profile"}
    card.stage(mount, removed, kvmpack.build({**members, "manifest.json": json.dumps(manifest).encode()}))
    (card.card_path(mount, removed) / "OFF.IDX").write_bytes(b"OFF")

    expected["all"] = set.union(*expected.values())
    for kind, wanted in expected.items():
        found = []
        for index in range((len(wanted) + 7) // 8 + 1):
            rows, more = page(kind, index)
            assert len(rows) == min(8, max(0, len(wanted) - index * 8)), (kind, index, rows)
            assert more == ((index + 1) * 8 < len(wanted)), (kind, index, more)
            if kind == "all":
                assert page("legacy", index) == (rows, more), (kind, index)
            found.extend(rows)
        assert len(found) == len(set(found)) and set(found) == wanted, (kind, set(found) ^ wanted)
        if kind == "all":
            assert set(found[:len(expected["installed"])]) == expected["installed"]
    print("Category pagination passed: 16 installed, 10 installers, 17 videos, 43 combined; legacy pages match.")


def check_review(base, files, run):
    manifest = json.loads(files["manifest.json"])
    manifest["id"] = "org.kvmpad.test.permission-review"
    path = base / "review.kvmpack"

    def build(version, capture=False, mandatory=False):
        manifest["version"] = version
        manifest["permissions"] = ["ui.basic"] + (["host.screen.region"] if capture else [])
        manifest["required_permissions"] = ["ui.basic"] + (["host.screen.region"] if mandatory else [])
        manifest["host"]["requires"] = ["capture.windows"] if capture else []
        return kvmpack.build({**files, "manifest.json": json.dumps(manifest).encode()})

    original = build("1.9.9")
    path.write_bytes(original)
    assert f"previous=none permissions=0 required=0 downgrade=0 extra=0 same=0 bytes={len(original)}" in run("review",path)
    run("install",path)
    assert "downgrade=0 extra=0 same=1" in run("review",path)
    newer = build("1.10.0", True)
    path.write_bytes(newer)
    assert "previous=1.9.9 permissions=1 required=1 downgrade=0 extra=0" in run("review",path)
    run("stage",path)
    assert "previous=1.9.9 " in run("review",path)
    run("install",path)
    healthy = card.recover(base / "card",manifest["id"])
    folder = card.card_path(base / "card",manifest["id"])
    assert "granted=1" in run("review", path)
    run("approve-mask", path, False, "8")   # The UI permission must accompany approval.
    run("approve-mask", path, False, "17")  # The archive never requested this capability.
    run("approve-mask", path, True, "9")
    assert "granted=9" in run("review", path)
    grant_path = folder / (healthy.slot + ".GRT")
    saved = grant_path.read_bytes()
    assert saved[4:36] == hashlib.sha256(newer).digest()
    for cut in range(len(saved)):
        grant_path.write_bytes(saved[:cut])
        assert "granted=1" in run("review", path)
    grant_path.write_bytes(saved[:-1] + bytes([saved[-1] ^ 1]))
    assert "granted=1" in run("review", path)
    grant_path.write_bytes(saved)
    path.write_bytes(build("1.10.1", True))
    assert "granted=1" in run("review", path)
    path.write_bytes(build("1.10.1", True, True))
    run("approve-mask", path, False, "1")  # A required capture permission cannot be silently omitted.
    path.write_bytes(newer)
    record = folder / (healthy.slot + ".IDX")
    saved = record.read_bytes()
    record.write_bytes(saved[:-1])
    assert "previous=1.9.9 permissions=1 required=1" in run("review",path)
    assert "granted=1" in run("review", path)
    record.write_bytes(saved)
    assert "granted=9" in run("review", path)
    run("approve-mask", path, True, "1")
    assert "granted=1" in run("review", path) and card.recover(base / "card", manifest["id"]) == healthy
    print("Optional capture grant follows the recovered slot. A changed digest and 44 torn lengths default off.")
    path.write_bytes(build("1.9.8",True))
    assert "downgrade=1 extra=0" in run("review",path)
    path.write_bytes(build("1.9.8",True,True))
    assert "downgrade=1 extra=1" in run("review",path)
    run("stage",path,False)
    path.write_bytes(original)
    assert "previous=1.10.0 permissions=9 required=1 downgrade=1 extra=1" in run("review",path)
    run("install",path,False)
    assert card.recover(base / "card",manifest["id"]) == healthy
    run("allow-downgrade",path)
    assert card.recover(base / "card",manifest["id"]).payload == original
    (base / "replacement.kvmpack").write_bytes(newer)
    run("stale-review",path)
    assert card.recover(base / "card",manifest["id"]).payload == newer
    run("remove",folder.relative_to(base).as_posix())
    assert "previous=none" in run("review",path)
    print("Review compared the healthy slot, numeric versions and required grants; unapproved and stale downgrades refused.")


def check_activation_health(base, files, exe):
    work = base / "health"
    mount = work / "card"
    mount.mkdir(parents=True)
    candidate = work / "candidate.kvmpack"
    manifest = json.loads(files["manifest.json"])
    name = manifest["id"]

    def version(number):
        manifest["version"] = number
        return kvmpack.build({**files, "manifest.json": json.dumps(manifest).encode()})

    def command(action, *args, ok=True):
        p = subprocess.run([exe,action,candidate,*map(str,args)],cwd=work,capture_output=True,text=True)
        assert p.returncode == (0 if ok else 1), (action,args,p.stdout,p.stderr)
        return p.stdout

    def strike(boot, seconds, disabled=False):
        assert f"failure disabled={int(disabled)}" in command("activation-failure",boot,seconds)

    healthy, bad = version("1.0.0"), version("1.1.0")
    candidate.write_bytes(healthy)
    command("install")
    folder = card.card_path(mount,name)
    candidate.write_bytes(bad)
    command("stage")
    assert not list(folder.glob("?.H?"))
    strike(10,5)
    strike(10,15)
    strike(11,1)  # new boot begins a new partial window
    strike(11,110)
    strike(11,125)  # time 1 has expired, but 110 is retained
    strike(11,130,True)
    assert "disabled=1" in command("review")
    command("install",ok=False)
    command("allow-downgrade",ok=False)
    assert card.recover(mount,name).payload == healthy

    journals = list(folder.glob("?.H?"))
    assert len(journals) == 2
    newest = max(journals,key=lambda path:int.from_bytes(path.read_bytes()[4:12],"little"))
    saved = newest.read_bytes()
    assert len(saved) == 96
    # Simulate power loss at every byte in the third-strike update. The last
    # complete journal and the healthy package remain independently readable.
    for n in range(len(saved)):
        newest.write_bytes(saved[:n])
        assert "disabled=0" in command("review") and card.recover(mount,name).payload == healthy
    broken = bytearray(saved)
    broken[12] ^= 1
    newest.write_bytes(broken)
    assert "disabled=0" in command("review")
    newest.write_bytes(saved)
    assert "disabled=1" in command("review")
    fixed = version("1.2.0")
    candidate.write_bytes(fixed)
    strike(12,1)
    strike(12,2)
    command("install")  # presentation clears these two failures
    strike(12,3)
    candidate.write_bytes(bad)
    assert "disabled=1" in command("review")
    candidate.write_bytes(fixed)
    strike(12,4)
    fixed_index = folder / (card.recover(mount,name).slot + ".IDX")
    published = fixed_index.read_bytes()
    strike(12,5,True)  # the newest already-published slot itself becomes disabled
    assert card.recover(mount,name).payload == healthy
    assert "previous=1.0.0" in command("review") and "disabled=1" in command("review")
    retired = fixed_index.read_bytes()
    fixed_index.write_bytes(published)  # power lost after quarantine, before legacy index retirement
    assert "previous=1.0.0" in command("review") and "disabled=1" in command("review")
    previous = card.recover(mount, name)
    assert previous is not None and previous.payload == healthy
    with patch.object(card, "write_synced") as writes:
        try:
            card.stage(mount, name, fixed)
        except ValueError:
            pass
        else:
            raise AssertionError("Desktop staging accepted a quarantined archive")
        writes.assert_not_called()

    replacement = version("1.3.0")
    write_synced = card.write_synced
    for interrupted in (".KVP", ".IDX"):
        fixed_index.with_suffix(".KVP").write_bytes(fixed)
        fixed_index.write_bytes(published)
        def cut_write(path, data):
            assert path.stem == fixed_index.stem, "Staging touched the only healthy slot"
            if path.suffix == interrupted:
                write_synced(path, data[:len(data) // 2])
                raise InterruptedError("Simulated card removal")
            write_synced(path, data)
        with patch.object(card, "write_synced", cut_write):
            try:
                card.stage(mount, name, replacement)
            except InterruptedError:
                pass
            else:
                raise AssertionError("Staging skipped the simulated power cut")
        assert card.recover(mount, name) == previous
        assert "previous=1.0.0" in command("review")
    assert card.stage(mount, name, replacement).slot == fixed_index.stem
    assert "previous=1.3.0" in command("review")
    print("Desktop and firmware preserved the healthy slot across post-quarantine power loss, interrupted payload/index writes and retry.")
    fixed_index.with_suffix(".KVP").write_bytes(fixed)
    fixed_index.write_bytes(retired)
    candidate.write_bytes(healthy)
    assert "same=1" in command("review")
    command("install")
    assert len(list(folder.glob("?.H?"))) <= 4
    print("Three-strike activation health passed: rolling 120s, cold boot, success reset, 96 torn journal lengths, disabled reopen and verified rollback.")


def check_interrupted_removal(base, files, exe):
    work = base / "interrupted-removal"
    mount = work / "card"
    mount.mkdir(parents=True)
    candidate = work / "candidate.kvmpack"
    manifest = json.loads(files["manifest.json"])
    profile = manifest["id"]
    folder = card.card_path(mount, profile)

    def run(*args):
        return subprocess.check_output([exe, *args], cwd=work, text=True)

    candidate.write_bytes(kvmpack.build(files))
    run("install", candidate)
    manifest["version"] = "2.0.0"
    payload = kvmpack.build({**files, "manifest.json": json.dumps(manifest).encode()})
    candidate.write_bytes(payload)
    for seconds in ("1", "2", "3"):
        run("activation-failure", candidate, "1", seconds)
    assert "disabled=1" in run("review", candidate)
    run("disable", folder.relative_to(work).as_posix())
    (folder / "OFF.IDX").write_bytes(b"OFF")
    (folder / "OWNER.TXT").write_bytes(b"keep")
    snapshot = {p.name: p.read_bytes() for p in folder.iterdir()}
    records = [name for name in snapshot if name not in ("OFF.IDX", "OWNER.TXT")]
    for installer in ("device", "desktop"):
        for cut in range(len(records) + 1):
            for p in folder.iterdir():
                p.unlink()
            for name, data in snapshot.items():
                if name not in records[:cut]:
                    (folder / name).write_bytes(data)
            before = {p.name: p.read_bytes() for p in folder.iterdir()}
            assert card.recover(mount, profile) is None
            review = run("review", candidate)
            assert "previous=none" in review and "disabled=0 manual=0" in review, review
            assert {p.name: p.read_bytes() for p in folder.iterdir()} == before
            if installer == "device":
                run("stage", candidate)
                assert card.recover(mount, profile) is None
                run("install", candidate)
            else:
                card.stage(mount, profile, payload)
            installed = card.recover(mount, profile)
            assert (installed.generation, installed.payload) == (1, payload)
            assert not list(folder.glob("?.H?")) and not (folder / "PAUSE.IDX").exists()
            assert not (folder / "B.IDX").exists() and not (folder / "B.GRT").exists()
            assert (folder / "OWNER.TXT").read_bytes() == b"keep"
        for seconds in ("4", "5", "6"):
            assert "disabled=" + str(int(seconds == "6")) in run("activation-failure", candidate, "1", seconds)
    print(f"Interrupted uninstall: both installers passed {len(records) + 1} cleanup boundaries and fresh quarantine.")


def check_manual_lifecycle(base, files, exe):
    work = base / "manual"
    mount = work / "card"
    mount.mkdir(parents=True)
    package = mount / "candidate.kvmpack"
    manifest = json.loads(files["manifest.json"])
    profile = manifest["id"]
    folder = card.card_path(mount, profile)
    relative = folder.relative_to(work).as_posix()
    marker = folder / "PAUSE.IDX"

    def version(number):
        manifest["version"] = number
        return kvmpack.build({**files, "manifest.json": json.dumps(manifest).encode()})

    def run(action, path=package, *args, ok=True):
        p = subprocess.run([exe, action, path, *args], cwd=work, capture_output=True, text=True)
        assert p.returncode == (0 if ok else 1), (action, p.stdout, p.stderr)
        return p.stdout

    def snapshots():
        return {p.name: p.read_bytes() for p in folder.iterdir() if p.name != marker.name}

    first, second, third = (version(v) for v in ("1.0.0", "2.0.0", "3.0.0"))
    package.write_bytes(first)
    run("install")
    package.write_bytes(second)
    run("install")
    before = snapshots()
    run("pause-trial", relative)
    run("disable", relative)
    assert marker.read_bytes() == b"1" and snapshots() == before
    assert "manual=1" in run("review")
    assert profile in run("recover", relative)
    assert relative in run("page", "installed", "0")
    # Interrupted marker creation, fresh process and failed active index.
    marker.write_bytes(b"")
    current = card.recover(mount, profile)
    record = folder / (current.slot + ".IDX")
    record.write_bytes(b"KVP")
    assert "previous=1.0.0" in run("review") and "manual=1" in run("review")
    package.write_bytes(third)
    run("stage", ok=False)
    run("allow-downgrade", ok=False)
    record.write_bytes(before[record.name])
    run("enable", relative)
    run("enable", relative)
    assert not marker.exists() and snapshots() == before
    for sec in ("1", "2", "3"):
        run("activation-failure", package, "17", sec)
    before = snapshots()
    run("disable", relative)
    assert "disabled=1 manual=1" in run("review")
    run("enable", relative)
    assert "disabled=1 manual=0" in run("review") and snapshots() == before
    run("install", ok=False)
    # Manual removal is permitted while paused. A reinstall completes an interrupted
    # uninstall even if the old preference directory entry was never cleared.
    run("disable", relative)
    run("remove", relative)
    assert not marker.exists() and (folder / "OFF.IDX").exists()
    marker.write_bytes(b"")
    package.write_bytes(first)
    assert "manual=0" in run("review")
    run("install")
    assert not marker.exists() and card.recover(mount, profile).payload == first
    # A damaged sole healthy index leaves the local card installer as the recovery path.
    run("disable", relative)
    (folder / (card.recover(mount, profile).slot + ".IDX")).write_bytes(b"K")
    assert "previous=none" in run("review") and "manual=1" in run("review")
    run("stage", ok=False)
    run("enable-orphan")
    run("install")
    package.write_bytes(second)
    run("install")
    assert card.recover(mount, profile).payload == second
    print("Manual disable/enable passed: reboot, zero-byte marker, A/B fallback, quarantined update, trial confirm, uninstall/reinstall and orphan repair.")


def check_card_swap(base, files, exe):
    original = kvmpack.build(files)
    manifest = json.loads(files["manifest.json"])
    manifest["version"] = "2.0.0"
    update = kvmpack.build({**files, "manifest.json": json.dumps(manifest).encode()})
    for action in ("swap-after-review", "swap-after-stage", "swap-before-remove", "swap-during-remove"):
        work = base / action
        mount = work / "card"
        mount.mkdir(parents=True)
        package = mount / "candidate.kvmpack"
        def run(command, path="card/candidate.kvmpack"):
            p = subprocess.run([exe, command, path], cwd=work, capture_output=True, text=True)
            assert p.returncode == 0, (command, p.stdout, p.stderr)
        package.write_bytes(original)
        run("install")
        package.write_bytes(update)
        if action == "swap-after-stage":
            run("stage")
        second = work / "second"
        shutil.copytree(mount, second)
        (mount / "volume-id").write_bytes(b"0001")
        (second / "volume-id").write_bytes(b"0002")
        def contents(root):
            return {p.relative_to(root): p.read_bytes() for p in root.rglob("*") if p.is_file()}
        before = contents(second)
        installed = card.card_path(mount, manifest["id"]).relative_to(work).as_posix()
        run(action, installed if action.endswith("-remove") else "card/candidate.kvmpack")
        assert contents(mount) == before
        assert card.recover(mount, manifest["id"]).payload == original
        if action == "swap-during-remove":
            first = work / "first"
            assert card.recover(first, manifest["id"]) is None
            assert (card.card_path(first, manifest["id"]) / "OFF.IDX").read_bytes() == b"OFF"
        else:
            assert card.recover(work / "first", manifest["id"]).payload == original
        if action == "swap-before-remove":
            (work / "replacement.kvmpack").write_bytes(update)
            run("stale-remove", installed)
            assert card.recover(mount, manifest["id"]).payload == update
            run("remove", installed)
            assert card.recover(mount, manifest["id"]) is None
        run("install")
        assert card.recover(mount, manifest["id"]).payload == update
    print("Card swap after review/staging, before uninstall and after its tombstone rejected; stale uninstall preserved the update.")


def check_interrupted_updates(base, files, exe, original, targets):
    manifest = json.loads(files["manifest.json"])
    manifest["version"] = "2.0.0"
    update = kvmpack.build({**files, "manifest.json": json.dumps(manifest).encode()})
    failed = []
    for target in targets:
        for phase in ("review", "stage"):
            work = base / f"interruption-{target}-{phase}"
            mount = work / "card"
            mount.mkdir(parents=True)
            package = mount / "candidate.kvmpack"
            package.write_bytes(original)
            subprocess.run([exe, "install", package], cwd=work, check=True, capture_output=True)
            folder = card.card_path(mount, manifest["id"])
            installed = folder.relative_to(work).as_posix()
            package.write_bytes(update)
            before = {p.name: p.read_bytes() for p in folder.iterdir()}
            fault = subprocess.run([exe, "interrupted-update", package, target, phase], cwd=work,
                                   capture_output=True, text=True)
            reboot = subprocess.run([exe, "recover", installed], cwd=work, capture_output=True, text=True)
            unchanged = before == {p.name: p.read_bytes() for p in folder.iterdir()}
            print(f"{fault.stdout.strip()} reboot={reboot.returncode} original_slots_unchanged={unchanged}", flush=True)
            if fault.returncode or reboot.returncode or "1.0.0" not in reboot.stdout or not unchanged:
                failed.append((target, phase))
                continue
            subprocess.run([exe, "stage", package], cwd=work, check=True, capture_output=True)
            reboot = subprocess.run([exe, "recover", installed], cwd=work, check=True, capture_output=True, text=True)
            assert "1.0.0" in reboot.stdout
            subprocess.run([exe, "install", package], cwd=work, check=True, capture_output=True)
            reboot = subprocess.run([exe, "recover", installed], cwd=work, check=True, capture_output=True, text=True)
            assert "2.0.0" in reboot.stdout
    assert not failed, failed
    print(f"Verified {2 * len(targets)} interrupted updates, recovery after tentative staging, and confirmed retry.")


def check_wrong_digest(base, files, exe):
    records = kvmpack.integrity(files)
    records["manifest.json"]["sha256"] = "00" * 32
    package = base / "bad-digest.kvmpack"
    with patch.object(kvmpack, "integrity", return_value=records):
        package.write_bytes(kvmpack.build(files))
    subprocess.run([exe, "invalid-integrity", package], cwd=base, check=True)
    print("A wrong member digest was rejected as invalid data.")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cc", required=True)
    ap.add_argument("--work", type=Path, required=True)
    ap.add_argument("--idf", type=Path, required=True)
    ap.add_argument("--config", type=Path, required=True)
    args = ap.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    base = args.work.resolve() / ("pack-test-" + uuid.uuid4().hex)
    base.mkdir()
    try:
        mount = base / "card"
        mount.mkdir()
        runtime = ROOT / "components/pack_runtime"
        remote = ROOT / "components/remote_protocol"
        host = Path(__file__).parent / "host"
        exe = base / ("check.exe" if os.name == "nt" else "check")
        compiler = [args.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O2",
                        "-DPACK_MOUNT=\"card\"", "-I", args.idf / "components/esp_common/include",
                        "-I", args.config, "-I", runtime, "-I", runtime / "include", "-I", remote / "include",
                        *(runtime / file for file in ("pack_archive.c","pack_load.c","pack_png.c","pack_scene.c","pack_store.c")),
                        remote / "src/remote_json.c", host / "pack_port_host.c", host / "device_check.c",
                        *native_png_objects(args.cc, base), "-lz", "-lcrypto", "-lm"]
        subprocess.run([*compiler, "-o", exe], check=True)

        io_dir = base / "directory-io"
        (io_dir / "card/KVPACKS/01234567").mkdir(parents=True)
        (io_dir / "card/sample.h264").touch()
        io_compiler = [arg for arg in compiler if arg not in (runtime / "pack_store.c", host / "device_check.c")]
        io_test = io_dir / exe.name
        subprocess.run([*io_compiler, host / "list_io.c", "-o", io_test], check=True)
        subprocess.run([io_test], cwd=io_dir, check=True)

        def run(command, path, success=True, *args):
            p = subprocess.run([exe,command,path,*args], cwd=base, capture_output=True, text=True)
            assert p.returncode == (0 if success else 1), (command,path,p.returncode,p.stderr,p.stdout)
            return p.stdout

        files = kvmpack.read_members(ROOT / "tools/kvmpack/examples/flight-checklist")
        check_interrupted_updates(base, files, exe, kvmpack.build(files), ("directory", "member"))
        check_wrong_digest(base, files, exe)
        check_card_swap(base, files, exe)
        check_manual_lifecycle(base, files, exe)
        check_interrupted_removal(base, files, exe)
        check_activation_health(base, files, exe)
        check_review(base, files, run)
        check_publishers(base, compiler, exe, runtime, files)
        check_category_pages(exe, base, files)
        manifest = kvmpack.validate_scene(files)
        name = manifest["id"]
        package = mount / "flight.kvmpack"
        package.write_bytes(kvmpack.build(files))
        run("tap", package)
        run("stage", package)
        assert card.recover(mount, name) is None
        assert "Flight checklist" not in run("list", "-")
        run("install", package)
        # The firmware directory and index are readable by the desktop implementation.
        first = card.recover(mount, name)
        assert first and first.generation == 1 and first.payload == package.read_bytes()
        folder = card.card_path(mount,name)
        rel = folder.relative_to(base).as_posix()
        assert "1.0.0" in run("recover",rel)
        assert "Flight checklist" in run("list","-")
        manifest["version"]="1.1.0"
        files["manifest.json"]=json.dumps(manifest).encode()
        package.write_bytes(kvmpack.build(files))
        run("stage", package)
        assert "1.0.0" in run("recover", rel)
        assert card.recover(mount, name) == first
        run("install",package)
        assert "1.1.0" in run("recover",rel)
        second=card.recover(mount,name)
        assert second.generation==2
        index_path = folder / (second.slot + ".IDX")
        healthy_index = index_path.read_bytes()
        for cut in range(len(healthy_index)):
            index_path.write_bytes(healthy_index[:cut])
            assert "1.0.0" in run("recover", rel)
        index_path.write_bytes(healthy_index)
        assert "1.1.0" in run("recover", rel)
        for suffix in ("IDX","KVP"):
            path=folder / (second.slot+"."+suffix)
            original=path.read_bytes()
            for cut in (0,1,len(original)-1):
                path.write_bytes(original[:cut])
                assert "1.0.0" in run("recover",rel)
            path.write_bytes(original)
        # The uninstall tombstone must keep an old, higher-generation slot hidden
        # even if removal stopped before deleting either original slot.
        (folder / "OFF.IDX").write_bytes(b"OFF")
        run("recover",rel,False)
        run("install",package)
        assert card.recover(mount,name).generation == 1
        assert "1.1.0" in run("recover",rel)
        run("remove",rel)
        run("recover",rel,False)
        run("install",package)
        assert "1.1.0" in run("recover",rel)
        # Same consumer rejects invalid state, escaping bounds, and unsupported permissions
        # even after a desktop writer recomputes all CRCs and digests.
        for doc, mutate in [
            ("ui.json",lambda v:v["root"]["children"][0]["bounds"].update(x=1000)),
            ("bindings.json",lambda v:v["actions"][0]["args"].update(name="missing")),
            ("manifest.json",lambda v:v["permissions"].append("hid.keyboard")),
            ("ui.json",lambda v:v["root"]["children"][1].update(id="title")),
        ]:
            value=json.loads(files[doc]); mutate(value)
            package.write_bytes(kvmpack.build({**files,doc:json.dumps(value).encode()}))
            run("tap",package,False)
        for example, doc, mutate in [
            ("flight-yoke", "bindings.json", lambda b: b["actions"][4]["args"]["buttons"].update({"1": True})),
            ("flight-yoke", "bindings.json", lambda b: b["actions"][1]["args"]["axes"].update(Z="$state.throttle")),
            ("flight-yoke", "manifest.json", lambda m: m["permissions"].remove("hid.joystick")),
            ("flight-instruments", "bindings.json", lambda b: b["sources"][0].update(topic="unknown.topic")),
            ("flight-instruments", "bindings.json", lambda b: b["connections"][0].update(to="horizon.unknown")),
            ("cursor-magnifier", "bindings.json", lambda b: b["actions"][3]["args"].update(requires_foreground_consent=False)),
            ("flight-checklist", "manifest.json", lambda m: m["limits"].update(max_memory_kib=16)),
        ]:
            members=kvmpack.read_members(ROOT / "tools/kvmpack/examples" / example)
            package.write_bytes(kvmpack.build(members))
            run("inspect",package)
            value=json.loads(members[doc])
            mutate(value)
            members[doc]=json.dumps(value).encode()
            package.write_bytes(kvmpack.build(members))
            run("inspect",package,False)
        original=kvmpack.build(files)
        for pos in (0,6,14,original.rfind(b"PK\x01\x02")+42,len(original)-12):
            broken=bytearray(original); broken[pos]^=0x80
            package.write_bytes(broken)
            run("tap",package,False)
        print("Device offline scene, archive bounds, physical-grant input subset, A/B compatibility, rollback and removal passed.")
    finally:
        if base.resolve().parent == args.work.resolve():
            shutil.rmtree(base)


if __name__ == "__main__":
    main()
