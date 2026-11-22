# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
import json
import io
import base64
import re
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import uuid
import zipfile
import zlib
from pathlib import Path
from unittest.mock import patch

SDK = Path(__file__).resolve().parents[2] / "tools/kvmpack"
sys.path.insert(0, str(SDK))
import kvmpack as pack
import card


class PackTests(unittest.TestCase):
    def test_rate_slider_contract(self):
        files = pack.read_members(SDK / "samples/rate-slider")
        self.archive.write_bytes(pack.build(files))
        self.assertEqual(pack.read_members(self.archive), files)
        for kind, field, value in [
            ("action", "delta", 0), ("action", "delta", 1 << 31), ("action", "delta", True),
            ("action", "name", "unknown"), ("widget", "props", {"value": 2}),
            ("widget", "bounds", {"x": 0, "y": 0, "w": 47, "h": 96}),
            ("widget", "bounds", {"x": 0, "y": 0, "w": 48, "h": 95}),
            ("widget", "type", "Button"),
        ]:
            with self.subTest(field=field, value=value):
                name = "bindings.json" if kind == "action" else "ui.json"
                doc = json.loads(files[name])
                node = doc["actions"][0]["args"] if kind == "action" else doc["root"]["children"][-1]
                node[field] = value
                with self.assertRaises(ValueError):
                    pack.validate_scene({**files, name: json.dumps(doc).encode()})

    @unittest.skipUnless(shutil.which("node"), "Node is required for preview interactions")
    def test_rate_slider_preview(self):
        files = pack.read_members(SDK / "samples/rate-slider")
        preview = self.archive.with_suffix(".html")
        self.addCleanup(preview.unlink, missing_ok=True)
        preview.write_text(pack.preview_html(files), encoding="utf-8")
        subprocess.run(["node", Path(__file__).with_name("rate_slider_preview.cjs"), preview], check=True)

    def test_usb_console_contract(self):
        files = pack.read_members(SDK.parents[1] / "extensions/usb-monitor-console")
        pack.validate_scene(files)
        limited = json.loads(files["manifest.json"])
        limited["limits"]["max_memory_kib"] = 80
        candidate = {**files, "manifest.json": json.dumps(limited).encode()}
        with self.assertRaisesRegex(pack.ProfileBudgetError, "KeyboardSurface"):
            pack.validate_scene(candidate)
        pack.validate_scene(candidate, editing=True)
        for cap in ("usb.monitor", "hid.keyboard", "hid.mouse"):
            for field in ("permissions", "required_permissions", "usb"):
                with self.subTest(cap=cap, field=field):
                    manifest = json.loads(files["manifest.json"])
                    values = manifest[field]["requires"] if field == "usb" else manifest[field]
                    values.remove(cap)
                    with self.assertRaises(ValueError):
                        pack.validate_scene({**files, "manifest.json": json.dumps(manifest).encode()})
        for field, value in (("mode", "static_superset"), ("requires", ["usb.monitor", "hid.joystick"])):
            manifest = json.loads(files["manifest.json"])
            manifest["usb"][field] = value
            with self.assertRaises(ValueError):
                pack.validate_scene({**files, "manifest.json": json.dumps(manifest).encode()})
        for index, name, w, h in ((0, "MonitorViewport", 160, 120), (1, "TouchpadSurface", 132, 88),
                                  (2, "KeyboardSurface", 440, 176)):
            for field, value in (("w", w-1), ("h", h-1), ("events", {"tap": "start"}),
                                 ("props", {"layout": "absolute"}), ("duplicate", None), ("minimum", None)):
                with self.subTest(kind=name, field=field):
                    ui = json.loads(files["ui.json"])
                    items = ui["root"]["children"][0]["children"]
                    node = items[index]
                    self.assertEqual(node["type"], name)
                    if field == "duplicate":
                        items.append({**node, "id": "additional"})
                    elif field == "minimum":
                        node["bounds"].update(w=w, h=h)
                    elif field in ("w", "h"):
                        node["bounds"][field] = value
                    else:
                        node[field] = value
                    candidate = {**files, "ui.json": json.dumps(ui).encode()}
                    if field == "minimum":
                        pack.validate_scene(candidate)
                    else:
                        with self.assertRaises(ValueError):
                            pack.validate_scene(candidate)
        ui = json.loads(files["ui.json"])
        for screen in ui["root"]["children"]:
            screen["children"] = [n for n in screen["children"] if n["type"] != "MonitorViewport"]
        pending = {**files, "ui.json": json.dumps(ui).encode()}
        pack.validate_scene(pending, editing=True)
        with self.assertRaisesRegex(ValueError, "local start"):
            pack.validate_scene(pending)

    @unittest.skipUnless(shutil.which("node"), "Node is required for preview interactions")
    def test_usb_console_export(self):
        files = pack.read_members(SDK.parents[1] / "extensions/usb-monitor-console")
        self.archive.write_bytes(pack.build(files))
        restored = pack.read_members(self.archive)
        self.assertEqual(files, restored)
        preview = self.archive.with_suffix(".html")
        self.addCleanup(preview.unlink, missing_ok=True)
        with preview.open("x", encoding="utf-8") as stream:
            stream.write(pack.preview_html(restored))
        subprocess.run(["node", Path(__file__).with_name("usb_console_preview.cjs"), preview], check=True)

    def setUp(self):
        self.files = pack.read_members(SDK / "examples/flight-yoke")
        with tempfile.NamedTemporaryFile(suffix=".kvmpack", delete=False) as f:
            self.archive = Path(f.name)
        self.addCleanup(self.archive.unlink, missing_ok=True)

    def change(self, name, edit):
        value = json.loads(self.files[name])
        edit(value)
        self.files[name] = json.dumps(value).encode()

    @unittest.skipUnless(shutil.which("node"), "Node is required for preview interactions")
    def test_pilot_panel_export(self):
        project = SDK.parents[1] / "extensions/pilot-panel"
        members = pack.read_members(project)
        pack.validate_scene(members)
        self.archive.write_bytes(pack.build(members))
        preview = self.archive.with_suffix(".html")
        self.addCleanup(preview.unlink, missing_ok=True)
        with preview.open("x", encoding="utf-8") as stream:
            stream.write(pack.preview_html(pack.read_members(self.archive)))
        subprocess.run(["node", Path(__file__).with_name("pilot_preview.cjs"), preview], check=True)

    def test_pilot_selection_and_panel_contract(self):
        files = pack.read_members(SDK.parents[1] / "extensions/pilot-panel")
        for field, value in (("max", 2), ("min", -1), ("type", "float")):
            with self.subTest(field=field):
                bindings = json.loads(files["bindings.json"])
                bindings["state"][0][field] = value
                with self.assertRaises(ValueError):
                    pack.validate_scene({**files, "bindings.json": json.dumps(bindings).encode()})
        for key, value in (("bounds", {"x": 0, "y": 0, "w": 539, "h": 680}),
                           ("events", {"tap": "select_aircraft_setup"})):
            with self.subTest(key=key):
                ui = json.loads(files["ui.json"])
                radio = next(page for page in ui["root"]["children"] if page["id"] == "radio")
                radio["children"][0][key] = value
                with self.assertRaises(ValueError):
                    pack.validate_scene({**files, "ui.json": json.dumps(ui).encode()})

    def test_examples(self):
        for path in (SDK / "examples").iterdir():
            with self.subTest(path=path):
                files = pack.read_members(path)
                pack.validate_scene(files)
                data = pack.build(files)
                self.assertEqual(data, pack.build(dict(reversed(list(files.items())))))
                self.archive.write_bytes(data)
                self.assertEqual(files, pack.read_members(self.archive))

    def test_tampered_inventory(self):
        self.files[pack.INTEGRITY] = json.dumps(pack.integrity(self.files)).encode()
        self.files["ui.json"] += b" "
        with zipfile.ZipFile(self.archive, "w") as archive:
            for name, data in self.files.items():
                archive.writestr(name, data)
        with self.assertRaisesRegex(ValueError, "digest"):
            pack.read_members(self.archive)

    def test_path_and_signature_policy(self):
        for name in ("assets/../outside.png", "/ui.json", "assets/image.exe", "signature.ed25519",
                     "assets/NUL.png", "assets\\file.png"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                pack.check_path(name)

    def test_ed25519_signed_inventory(self):
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
        key = Ed25519PrivateKey.generate()
        data = pack.build(self.files, key)
        self.archive.write_bytes(data)
        self.assertEqual(pack.read_members(self.archive, key.public_key()), self.files)
        self.assertEqual(pack.read_package(self.archive, key.public_key())[1], data)
        with self.assertRaises(ValueError):
            pack.read_members(self.archive)
        with self.assertRaises(ValueError):
            pack.read_members(self.archive, Ed25519PrivateKey.generate().public_key())
        with zipfile.ZipFile(self.archive) as archive:
            signature = archive.read(pack.SIGNATURE)
        self.files["ui.json"] += b" "
        with zipfile.ZipFile(self.archive, "w") as archive:
            for name, content in self.files.items():
                archive.writestr(name, content)
            archive.writestr(pack.INTEGRITY, json.dumps(pack.integrity(self.files)))
            archive.writestr(pack.SIGNATURE, signature)
        with self.assertRaisesRegex(ValueError, "untrusted"):
            pack.read_members(self.archive, key.public_key())

    def test_shared_button_png_in_archive_and_offline_preview(self):
        from PIL import Image
        files = pack.read_members(SDK / "examples/flight-checklist")
        ui = json.loads(files["ui.json"])
        image = Image.new("RGBA", (65, 17))
        image.putdata([(x, y, 128, (4*x+y) % 256) for y in range(17) for x in range(65)])
        png = io.BytesIO()
        image.save(png, format="PNG")
        name = "assets/Deck/mark.png"
        files[name] = png.getvalue()
        buttons = [n for n in ui["root"]["children"] if n["type"] == "Button"]
        for node in buttons:
            node["props"]["icon"] = name
        files["ui.json"] = json.dumps(ui).encode()
        self.archive.write_bytes(pack.build(files))
        loaded = pack.read_members(self.archive)
        pack.validate_scene(loaded)
        preview = pack.preview_html(loaded)
        payload = json.loads(re.search(r'<script id="pack" type="application/json">(.*?)</script>', preview)[1])
        self.assertEqual(list(payload["icons"]), [name])
        encoded = payload["icons"][name].removeprefix("data:image/png;base64,")
        with Image.open(io.BytesIO(base64.b64decode(encoded))) as thumb:
            self.assertEqual(thumb.size, (32, 8))
            for y in range(8):
                for x in range(32):
                    self.assertEqual(thumb.getpixel((x,y)), image.getpixel((x*65//32,y*17//8)))
        buttons[0]["props"]["icon"] = name.lower()
        files["ui.json"] = json.dumps(ui).encode()
        with self.assertRaisesRegex(ValueError, "icon"):
            pack.validate_scene(files)

    def test_16bit_gray_icon_preserves_alpha_key_before_reducing_depth(self):
        from PIL import Image
        image = Image.new("I;16", (2, 1))
        image.putdata([0x4000, 0x40ff])
        png = io.BytesIO()
        image.save(png, format="PNG", transparency=0x4000)
        thumb = pack.icon_thumbnail(png.getvalue())
        self.assertEqual(thumb.getpixel((0, 0)), (64, 64, 64, 0))
        self.assertEqual(thumb.getpixel((1, 0)), (64, 64, 64, 255))

    def test_checked_png(self):
        from PIL import Image
        root = self.archive.parent / ("pack-png-" + uuid.uuid4().hex)
        root.mkdir()
        self.addCleanup(shutil.rmtree, root)
        for name, data in self.files.items():
            (root / name).write_bytes(data)
        assets = root / "assets"
        assets.mkdir()
        path = assets / "panel.png"
        Image.new("RGB", (2, 3)).save(path)
        self.assertIn("assets/panel.png", pack.read_members(root))
        data = bytearray(path.read_bytes())
        data[data.index(b"IDAT") + 4] ^= 1
        path.write_bytes(data)
        with self.assertRaises((ValueError, OSError, SyntaxError)):
            pack.read_members(root)

    def test_7b_scene_and_sequential_png_budget(self):
        # This file needed 14,668 decoder bytes in the Xtensa threshold test.
        png = bytes.fromhex(
            "89504e470d0a1a0a0000000d4948445200000200000000010806000000a8bb1676"
            "0000001d49444154789c6394749bfd9f61148c8251300a46c12818050c23090000"
            "d46a01fb7a8825870000000049454e44ae426082")
        self.assertEqual(pack.png_workspace_floor(png, "sample.png"), 14668)
        with self.assertRaises(pack.ProfileBudgetError) as error:
            pack.check_memory_budget({}, 48)
        self.assertEqual(error.exception.minimum_kib, 49)
        pack.check_memory_budget({}, 49)
        files = {f"assets/tile{i}.png": png for i in range(3)}
        with self.assertRaises(pack.ProfileBudgetError) as error:
            pack.check_memory_budget(files, 62)
        self.assertEqual(error.exception.minimum_kib, 63)
        pack.check_memory_budget(files, 63)

    def test_7b_budget_tracks_png_reads_and_transparency(self):
        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

        # A single grayscale row. The zlib header alone produces no pixels.
        header = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">2I5B", 512, 1, 8, 0, 0, 0, 0))
        pixels = zlib.compress(b"\0" + bytes(range(256)) * 2)
        def cost(cut, extra=b""):
            data = (header + extra + chunk(b"IDAT", pixels[:cut]) +
                    chunk(b"IDAT", pixels[cut:]) + chunk(b"IEND", b""))
            return pack.png_workspace_floor(data, "gray.png")

        whole = cost(len(pixels))
        self.assertEqual(cost(2), whole - 2)
        self.assertEqual(cost(len(pixels) - 1), whole - 1 + 32768 + 16)
        self.assertEqual(cost(len(pixels), chunk(b"tRNS", b"\0\x01")), whole + 2 * (256 + 16))

    def test_json_limits(self):
        for data in (b'{"x":1,"x":2}', b"[1e999]", b"[NaN]", b'"\xff"', b"[" * 13 + b"]" * 13,
                     b" " * (pack.FILE_LIMIT + 1)):
            with self.subTest(data=data[:15]), self.assertRaises(ValueError):
                pack.parse_json(data, "ui.json")

    def test_viewport_overflow(self):
        self.change("ui.json", lambda ui: ui["root"]["bounds"].update(y=1))
        with self.assertRaisesRegex(ValueError, "content area"):
            pack.validate_scene(self.files)

    def test_binding_locations_in_source_and_archive(self):
        files = pack.read_members(SDK / "examples/flight-instruments")
        for index, field, value, code in (
            (2, "from", "missing", "BINDING_SOURCE"),
            (4, "from", "missing", "BINDING_SOURCE"),
            (3, "to", "missing.value", "BINDING_TARGET"),
            (1, "to", "gauge_ias.value", "BINDING_DUPLICATE"),
            (2, "transform", "boolean_threshold", "BINDING_TYPE"),
            (0, "transform", "missing", "JSON_SCHEMA"),
        ):
            with self.subTest(index=index, field=field, code=code):
                bindings = json.loads(files["bindings.json"])
                bindings["connections"][index][field] = value
                text = json.dumps(bindings, indent=2).replace("\n", "\r\n")
                files_under_test = {**files, "bindings.json": text.encode()}
                self.archive.write_bytes(pack.build(files_under_test))
                for members in (files_under_test, pack.read_members(self.archive)):
                    with self.assertRaises(pack.DocumentError) as caught:
                        pack.validate_scene(members)
                    error = caught.exception
                    bad_field = "to" if code == "BINDING_TYPE" else field
                    bad_value = bindings["connections"][index][bad_field]
                    self.assertEqual(error.path, f"$.connections[{index}].{bad_field}")
                    self.assertEqual((error.name, error.code), ("bindings.json", code))
                    self.assertTrue(text.splitlines()[error.line - 1][error.column - 1:].startswith(json.dumps(bad_value)))
                    if code == "BINDING_DUPLICATE":
                        self.assertGreater(error.line, next(i for i, s in enumerate(text.splitlines(), 1)
                                                            if json.dumps(bad_value) in s))

    def test_json_syntax_location_after_unicode(self):
        for data, code in (('{"name":"ż", "x":?}'.encode(), "JSON_SYNTAX"),
                           ('{"name":"ż", "x":'.encode() + b"\xff}", "JSON_ENCODING")):
            with self.subTest(code=code), self.assertRaises(pack.DocumentError) as caught:
                pack.parse_json(b"\n" + data, "ui.json")
            error = caught.exception
            self.assertEqual((error.name, error.code, error.line, error.column),
                             ("ui.json", code, 2, len('{"name":"ż", "x":') + 1))

    def test_wrong_capability(self):
        self.change("bindings.json", lambda b: b["actions"][0].update(capability="ui.basic"))
        with self.assertRaisesRegex(ValueError, "permission"):
            pack.validate_scene(self.files)

    def test_unknown_security_fields(self):
        self.change("manifest.json", lambda m: m["usb"].update(dynamic=True))
        with self.assertRaisesRegex(ValueError, "manifest.json"):
            pack.validate_scene(self.files)

    def test_unknown_arguments(self):
        self.change("bindings.json", lambda b: b["actions"][0]["args"].update(extra=1))
        with self.assertRaisesRegex(ValueError, "joystick argument"):
            pack.validate_scene(self.files)

    def test_stale_trigger(self):
        self.change("bindings.json", lambda b: b["actions"][0].update(trigger="unbound.tap"))
        with self.assertRaisesRegex(ValueError, "trigger"):
            pack.validate_scene(self.files)

    def test_cycle(self):
        def edit(b):
            b["state"] = [{"id": "yoke.value", "type": "float", "initial": 0}]
            b["connections"] = [{"from": "yoke.value", "to": "yoke.value", "transform": "identity"}]
        self.change("bindings.json", edit)
        with self.assertRaisesRegex(ValueError, "cyclic"):
            pack.validate_scene(self.files)

    def test_expanded_size(self):
        with zipfile.ZipFile(self.archive, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("assets/image.png", b" " * (pack.EXPANDED_LIMIT + 1))
        with self.assertRaisesRegex(ValueError, "size limit"):
            pack.read_members(self.archive)


class CardTests(unittest.TestCase):
    def setUp(self):
        self.mount = Path(tempfile.gettempdir()) / ("kvmpad-card-" + uuid.uuid4().hex)
        self.mount.mkdir()
        self.addCleanup(shutil.rmtree, self.mount)
        self.files = pack.read_members(SDK / "examples/flight-yoke")
        self.profile = pack.validate_scene(self.files)["id"]
        self.original = pack.build(self.files)
        self.files["ui.json"] += b" "
        self.update = pack.build(self.files)

    def test_missing_card_and_retry(self):
        self.assertIsNone(card.recover(self.mount / "absent", self.profile))
        first = card.stage(self.mount, self.profile, self.original)
        self.assertEqual(first, card.stage(self.mount, self.profile, self.original))
        self.assertEqual(first.payload, card.recover(self.mount, self.profile).payload)
        self.assertIsNone(card.recover(self.mount, "org.kvmpad.other"))

    def test_interrupted_publish(self):
        first = card.stage(self.mount, self.profile, self.original)
        write = card.write_synced
        for extension in (".KVP", ".IDX"):
            for cut in (0, 1, -1):
                def interrupt(path, data):
                    if path.suffix == extension:
                        write(path, data[:cut])
                        raise OSError("card removed")
                    write(path, data)
                with self.subTest(extension=extension, cut=cut), patch.object(card, "write_synced", interrupt):
                    with self.assertRaises(OSError):
                        card.stage(self.mount, self.profile, self.update)
                self.assertEqual(card.recover(self.mount, self.profile), first)
        updated = card.stage(self.mount, self.profile, self.update)
        self.assertEqual((updated.slot, updated.generation, updated.payload), ("B", 2, self.update))
        directory = card.card_path(self.mount, self.profile)
        (directory / "B.KVP").write_bytes(b"damaged")
        self.assertEqual(card.recover(self.mount, self.profile), first)
        os.remove(directory / "A.IDX")
        self.assertIsNone(card.recover(self.mount, self.profile))

    def test_concurrent_writer(self):
        directory = card.card_path(self.mount, self.profile, create=True)
        with card.writer(directory):
            with self.assertRaises(OSError):
                card.stage(self.mount, self.profile, self.original)
        self.assertEqual(card.stage(self.mount, self.profile, self.original).generation, 1)

    def test_health_journal_selection(self):
        first = card.stage(self.mount, self.profile, self.original)
        second = card.stage(self.mount, self.profile, self.update)
        directory = card.card_path(self.mount, self.profile)
        # The digest of B can be quarantined by the journal for slot A.
        def journal(sequence, disabled, count=0, reserved=0):
            prefix = struct.pack("<4sQ32s44xBBH", b"KPH1", sequence, second.digest, count, disabled, reserved)
            return prefix + struct.pack("<I", zlib.crc32(prefix))
        (directory / "A.H1").write_bytes(journal(1, 1))
        clear = journal(2, 0)
        for invalid in (clear[:-1], clear + b"x", clear[:-1] + bytes([clear[-1] ^ 1]), journal(3, 0),
                        journal(2, 2), journal(2, 0, count=3), journal(2, 0, reserved=1)):
            with self.subTest(invalid=invalid.hex()):
                (directory / "A.H0").write_bytes(invalid)
                self.assertEqual(card.recover(self.mount, self.profile), first)
        (directory / "A.H0").write_bytes(clear)
        self.assertEqual(card.recover(self.mount, self.profile), second)


if __name__ == "__main__":
    unittest.main()
