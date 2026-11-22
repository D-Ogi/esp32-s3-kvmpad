# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Package the matching FFmpeg and ESP-IDF libssh sources for a KVMpad release."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import tarfile
import tempfile
import zipfile

from build_ffmpeg import TARBALL, decoder_inputs

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
# Registry manifest for david-cermak/libssh 0.12.2 in dependencies.lock.
SSH_CHECKSUMS = "8ef19cfdb12d0090bd861b4e5c5f6e8a12d4ef21f8ae7cec990927923950fe16"
SSH_FILES = {"CMakeLists.txt", "Kconfig", "idf_component.yml", "README.md", "sbom.yml",
             "sbom_libssh.yml", "libssh-mirror/AUTHORS", "libssh-mirror/BSD",
             "libssh-mirror/COPYING", "libssh-mirror/README.mbedtls"}
SSH_TREES = ("port/", "patches/", "libssh-mirror/src/", "libssh-mirror/include/")


def package(handoff, ssh, output):
    files = decoder_inputs(handoff)
    checksums = (ssh / "CHECKSUMS.json").read_bytes()
    if hashlib.sha256(checksums).hexdigest() != SSH_CHECKSUMS:
        raise ValueError("libssh does not match the release registry manifest")
    for entry in json.loads(checksums)["files"]:
        name = entry["path"]
        if name in SSH_FILES or name.startswith(SSH_TREES):
            data = (ssh / name).read_bytes()
            if hashlib.sha256(data).hexdigest() != entry["hash"]:
                raise ValueError(f"unexpected libssh content: {name}")
            files["libssh/" + name] = data
    with tarfile.open(fileobj=io.BytesIO(files[TARBALL]), mode="r:xz") as upstream:
        files["licenses/FFmpeg-LGPL-2.1.txt"] = upstream.extractfile("ffmpeg-6.1.1/COPYING.LGPLv2.1").read()
    files["README.md"] = (HERE / "DECODER-SOURCE.md").read_bytes()
    files["licenses/KVMpad-tools.md"] = (REPO / "LICENSE.md").read_bytes()
    for name in ("build_ffmpeg.py", "ffmpeg-workspace.patch"):
        files["build/" + name] = (HERE / name).read_bytes()
    files["build/kvmpad_ssh_buffers.cmake"] = (REPO / "tools/cmake/kvmpad_ssh_buffers.cmake").read_bytes()
    # Checkouts may use native line endings for the project-owned instructions and scripts.
    for name in ("README.md", "licenses/KVMpad-tools.md", "build/build_ffmpeg.py",
                 "build/ffmpeg-workspace.patch", "build/kvmpad_ssh_buffers.cmake"):
        files[name] = files[name].replace(b"\r\n", b"\n")
    files["SHA256SUMS"] = "".join(f"{hashlib.sha256(data).hexdigest()}  {name}\n"
                                  for name, data in sorted(files.items())).encode()
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as temp:
        archive = Path(temp) / output.name
        with zipfile.ZipFile(archive, "w") as result:
            for name, data in sorted(files.items()):
                info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
                info.create_system = 3
                info.external_attr = 0o100644 << 16
                # Storing all entries also makes the release digest independent of zlib versions.
                result.writestr(info, data)
        archive.replace(output)
    print(f"{output.name}: {len(files)} files, SHA-256 {hashlib.sha256(output.read_bytes()).hexdigest()}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("handoff", type=Path)
    parser.add_argument("--libssh", type=Path, required=True, help="pinned ESP-IDF component directory")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    package(args.handoff, args.libssh, args.output)
