# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Verify the staged Windows esptool package and build its separate source ZIP."""
import argparse
import hashlib
import io
from pathlib import Path
import tarfile
import tempfile
import urllib.request
import zipfile

SDIST = "esptool-5.6.0.tar.gz"
SDIST_URL = ("https://files.pythonhosted.org/packages/96/4c/a949409e527b87f863f237cea9f997d2"
             "5644607f799293a3d247f1b40e67/" + SDIST)
# Each directory is relative to the root of the v1.4.1 checkout. The last four are its test gitlinks.
GITHUB = [
    ("esptool-legacy-flasher-stub-v1.11.1.tar.gz", "espressif/esptool-legacy-flasher-stub",
     "1ec06fd3b6dc85254c425aa93be694eb1c06b0ed", "1425e634b4ba8c360b97e863084577cee3d015d5026da5d1e40ff54bbba989ab", None),
    ("esp-flasher-stub-v1.4.1.tar.gz", "espressif/esp-flasher-stub",
     "c79b242586695bd7b199de6499f0ca89b32a3071", "46645219f97238ac96fc7a545aaaac7e2212e4d479b6fe5a0f1e66d4d8c12902", "."),
    ("esp-stub-lib-26e5798.tar.gz", "espressif/esp-stub-lib",
     "26e5798f1d9ab335072fa87102ec3314f87024a7", "fcd59cde36cd858be7c37aea5d12bcf9c7cfabf1edb448e546e81709e280a2c0", "esp-stub-lib"),
    ("CMock-94dcd39.tar.gz", "ThrowTheSwitch/CMock",
     "94dcd39765859fb681d500307bbe1b5da4851df2", "639460b0d5b1280bb265bdc494cbd2932e3da985e9f5eeae02ffb06c47ba1e8f", "unittests/CMock"),
    ("Unity-36e9b19.tar.gz", "ThrowTheSwitch/Unity",
     "36e9b197ec3c36951ec6abf1f57252116e04acc3", "3edba4b090ce4aec37c7c15ca16cbe0de35671bb20a2054cf6d7bf88d3fdeb58", "unittests/Unity"),
    ("CException-37673b7.tar.gz", "ThrowTheSwitch/CException",
     "37673b75954b33ebd593d180993c64d3bef45bc0", "664500c6cd96ccc49dc92a8cccb8efbabff31f435574a2fee3dd945d1b9f5226", "unittests/CMock/vendor/c_exception"),
    ("Unity-eb79bce.tar.gz", "ThrowTheSwitch/Unity",
     "eb79bce1b5b242ff580ccc99d8d4660625e8ffa9", "9d7a5bc821694ce6af7a3e80a5af379432a942b35db0acd396f27bcfb8ffd0ba", "unittests/CMock/vendor/unity"),
]
PACKAGES = ("esptool", "espsecure", "espefuse", "esp_rfc2217_server")


def source(cache, name, url, checksum):
    path = cache / name
    if path.exists():
        data = path.read_bytes()
    else:
        with urllib.request.urlopen(url, timeout=60) as response:
            data = response.read()
    if hashlib.sha256(data).hexdigest() != checksum:
        raise ValueError(f"upstream archive does not match its pinned SHA-256: {name}")
    if not path.exists():
        path.write_bytes(data)
    return data


def verify_package(sdist, site):
    with tarfile.open(fileobj=io.BytesIO(sdist), mode="r:gz") as archive:
        upstream = {member.name.split("/", 1)[1]: archive.extractfile(member).read()
                    for member in archive if member.isfile()}
    expected = {name for name in upstream if name.split("/", 1)[0] in PACKAGES}
    actual = {path.relative_to(site).as_posix() for package in PACKAGES for path in (site / package).rglob("*")
              if path.is_file() and "__pycache__" not in path.parts}
    if actual != expected:
        raise ValueError(f"esptool file list differs: {sorted(actual ^ expected)}")
    for name in sorted(expected):
        if (site / name).read_bytes() != upstream[name]:
            raise ValueError(f"esptool payload differs from its published source distribution: {name}")
    if (site / "esptool-5.6.0.dist-info/licenses/LICENSE").read_bytes() != upstream["LICENSE"]:
        raise ValueError("the installed esptool license differs from the source package")
    stubs = "esptool/targets/stub_flasher"
    licenses = {"GPL-2.0.txt": upstream["LICENSE"]}
    for license_name in ("LICENSE-APACHE", "LICENSE-MIT"):
        licenses[license_name] = upstream[f"{stubs}/2/{license_name}"]
    for version in ("1", "2"):
        licenses[f"stub-{version}-README.md"] = upstream[f"{stubs}/{version}/README.md"]
    print(f"Verified {len(expected)} installed programmer files and the GPL text.")
    return licenses


def package(runtime, cache, output):
    cache.mkdir(parents=True, exist_ok=True)
    data = source(cache, SDIST, SDIST_URL, "86c7249e9cc3a824e032ede9ba20abad057a2c49e231418cc4f02b177bb36f42")
    files = {"upstream/" + SDIST: data}
    for name, content in verify_package(data, runtime / "Lib/site-packages").items():
        files["licenses/" + name] = content
    notes = [
        "Windows programmer sources for esptool 5.6.0", "",
        "The esptool source distribution includes its Python modules, build scripts and stub JSON.",
        "Use its pyproject.toml to build the standalone Python package.",
        "The legacy stub archive is the source tree for release v1.11.1.",
        "esptool names the upstream esp32p4rc1.json file esp32p4-rev1.json.",
        "The second stub archive is the source tree for release v1.4.1.",
        "To recreate the recursive v1.4.1 checkout, extract each archive listed below",
        "to its indicated directory, stripping the archive's single top-level directory.",
        "The two stub READMEs describe the toolchains and commands required for the builds.",
        "", f"{SDIST}: {SDIST_URL}", "",
    ]
    for name, repo, commit, checksum, directory in GITHUB:
        url = f"https://codeload.github.com/{repo}/tar.gz/{commit}"
        files[f"upstream/{name}"] = source(cache, name, url, checksum)
        destination = directory if directory is not None else "separate legacy source tree"
        notes.extend((f"{name} -> {destination}", f"  {url}"))
    notes.extend(("", "SHA256SUMS lists the original archives and accompanying notices."))
    files["README.txt"] = ("\n".join(notes) + "\n").encode()
    files["PROGRAMMER-NOTICES.txt"] = Path(__file__).with_name("PROGRAMMER-NOTICES.txt").read_bytes().replace(b"\r\n", b"\n")
    files["SHA256SUMS"] = "".join(f"{hashlib.sha256(data).hexdigest()}  {name}\n"
                                  for name, data in sorted(files.items())).encode()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as temp:
        staged = Path(temp) / output.name
        with zipfile.ZipFile(staged, "w") as archive:
            for name, data in sorted(files.items()):
                info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
                info.create_system = 3
                info.external_attr = 0o100644 << 16
                archive.writestr(info, data)
        staged.replace(output)
    print(f"{output.name}: {output.stat().st_size} bytes, SHA-256 {hashlib.sha256(output.read_bytes()).hexdigest()}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", required=True, type=Path, help="staged CLIENT runtime directory")
    parser.add_argument("--cache", required=True, type=Path, help="downloaded upstream archives for offline reuse")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    package(args.runtime.resolve(), args.cache.resolve(), args.output.resolve())
