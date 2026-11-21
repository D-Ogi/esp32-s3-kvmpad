# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Exercise a built Extension SDK in a new folder with Python's site packages disabled."""
import argparse
import hashlib
from io import BytesIO
import json
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tkinter as tk
from unittest.mock import patch
from zipfile import ZipFile


def edit(start, workspace):
    # Set up exactly the import paths used by the distributed launcher.
    with patch.object(sys, "argv", [str(start), "validate", str(start.parent / "tools/kvmpack/examples/flight-checklist")]):
        runpy.run_path(str(start), run_name="__main__")
    import extension_studio as studio
    import jsonschema
    from PIL import Image
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

    assert Path(jsonschema.__file__).is_relative_to(start.parent / "lib")
    assert Path(Image.__file__).is_relative_to(start.parent / "lib")
    key = Ed25519PrivateKey.generate()
    assert key.public_key().verify(key.sign(b"offline"), b"offline") is None
    root = tk.Tk()
    root.withdraw()
    try:
        editor = studio.Editor(root, studio.EXAMPLES / "flight-checklist")
        schema = json.loads((studio.kvmpack.SCHEMAS / "kvmpack-ui.schema.json").read_bytes())
        assert set(editor.widget_kind["values"]) == set(schema["$defs"]["node"]["properties"]["type"]["enum"])
        editor.widget_kind.set("Row")
        editor.add_button.invoke()
        parent = editor.selected
        assert editor.nodes[parent]["type"] == "Row"
        editor.parent_choice.set(parent)
        editor.widget_kind.set("TabStrip")
        editor.add_button.invoke()
        assert editor.nodes[parent]["children"][0]["type"] == "TabStrip"
        studio.kvmpack.validate_scene(editor.current.members)
        with patch.object(studio.messagebox, "askyesno", return_value=True):
            editor.open(studio.EXAMPLES / "flight-checklist")
        editor.select("title")
        editor.fields["label"].set("Departure checks")
        editor.apply_widget()
        tab = editor.documents["manifest.json"]
        manifest = json.loads(tab.get("1.0", "end"))
        manifest.update(id="org.example.departure", title="Departure checks")
        tab.delete("1.0", "end")
        tab.insert("1.0", json.dumps(manifest))
        editor.validate()
        with patch.object(studio.filedialog, "asksaveasfilename", return_value=str(workspace / "departure")):
            editor.save_source()
        archive = workspace / "departure.kvmpack"
        with patch.object(studio.filedialog, "asksaveasfilename", return_value=str(archive)):
            editor.package()
        editor.open(archive)
        assert editor.current.manifest["id"] == "org.example.departure"
        png = BytesIO()
        Image.new("RGB", (8, 8), (40, 135, 90)).save(png, format="PNG")
        member = "assets/status.png"
        payload = {**editor.current.members, member: png.getvalue()}
        signed = workspace / "signed-example.kvmpack"
        signed.write_bytes(studio.kvmpack.build(payload, key))
        editor.open(signed, verify_key=key.public_key())
        assert editor.current.members[member] == png.getvalue()
        assert editor.current.package() == signed.read_bytes()
        editor.preview_dir = workspace
        with patch.object(studio.webbrowser, "open", return_value=True) as opened:
            editor.simulate()
        opened.assert_called_once_with((workspace / "preview-1.html").as_uri())
    finally:
        root.destroy()


def verify(archive, workspace):
    workspace.mkdir()  # Each verification uses a fresh directory.
    with ZipFile(archive) as z:
        z.extractall(workspace)
    sdk = workspace / "KVMpad-Extension-SDK"
    for line in (sdk / "SHA256SUMS").read_text(encoding="utf-8").splitlines():
        expected, name = line.split("  ", 1)
        assert hashlib.sha256((sdk / name).read_bytes()).hexdigest() == expected, name
    isolated = [sys.executable, "-I", "-S", "-B"]
    start = sdk / "start.py"
    subprocess.run([*isolated, Path(__file__).resolve(), "--edit", start, workspace], cwd=workspace, check=True)
    def cli(*args):
        return subprocess.check_output([*isolated, start, *args], cwd=workspace, text=True)

    examples = sdk / "tools/kvmpack/examples"
    for example in sorted(examples.iterdir()):
        assert "valid desktop source" in cli("validate", example)
        cli("preview", example, "-o", workspace / (example.name + ".html"))
    assert len(list(examples.iterdir())) == 4
    console = sdk / "extensions/usb-monitor-console"
    assert "valid desktop source" in cli("validate", console)
    cli("build", console, "-o", workspace / "usb-monitor-console.kvmpack")
    built = workspace / "departure-v1.kvmpack"
    cli("build", workspace / "departure", "-o", built)
    assert built.read_bytes() == (workspace / "departure.kvmpack").read_bytes()
    assert json.loads(cli("inspect", built))["id"] == "org.example.departure"
    card = workspace / "card"
    card.mkdir()
    cli("stage", built, "--card", card)
    assert next(card.glob("*.kvmpack")).read_bytes() == built.read_bytes()
    shutil.copyfile(workspace / "preview-1.html", workspace / "departure.html")
    print("PASS: isolated imports, native dependencies, editor palette and nesting, four templates, preview, build, inspect and card staging")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--edit", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("archive", type=Path)
    parser.add_argument("new_directory", type=Path)
    args = parser.parse_args()
    (edit if args.edit else verify)(args.archive.resolve(), args.new_directory.resolve())
