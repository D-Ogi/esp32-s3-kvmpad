# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Regression tests for the PC side of the board firmware tool."""
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch, MagicMock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "firmware"))
import firmware
import firmware_image


class FirmwareTests(unittest.TestCase):
    def test_update_url(self):
        firmware.validate_url("https://files.example/firmware.bin?rev=1")
        for url in ("http://files.example/fw", "https://user@files.example/fw", "https://x/a\ncmd",
                    "https://x/a#extra", "https://x/" + "a" * 110):
            with self.subTest(url=url), self.assertRaises(ValueError):
                firmware.validate_url(url)

    def test_local_package_must_match_release(self):
        with tempfile.TemporaryDirectory() as directory:
            local = Path(directory)
            path = local / "objects.zip"
            path.write_bytes(b"abc")
            package = {"bytes": 3, "sha256": hashlib.sha256(b"abc").hexdigest()}
            self.assertEqual(path, firmware_image.obtain(local, path.name, package, "", local, print))
            path.write_bytes(b"bad")
            with self.assertRaisesRegex(ValueError, "incorrect"):
                firmware_image.obtain(local, path.name, package, "", local, print)

    def run_ota(self, result):
        initial = ("confirmed", "ota_0 state=valid confirmed", "ota_1", "none", "")
        with patch.object(firmware.serial, "Serial"), patch.object(firmware.time, "sleep"), \
             patch.object(firmware, "reply", return_value="update started;"), \
             patch.object(firmware, "status", side_effect=[initial, initial, result]):
            return firmware.update("COM1", "https://files.example/fw.bin", MagicMock())

    def test_wait_for_confirmation_in_new_slot(self):
        result = ("confirmed", "ota_1 state=valid confirmed", "ota_0", "none", "app  kvmpad build2")
        self.assertEqual(self.run_ota(result), "Board confirmed firmware build2 in ota_1.")

    def test_surface_signature_refusal(self):
        result = ("failed", "ota_0 state=valid confirmed", "ota_1", "signature", "")
        with self.assertRaisesRegex(ValueError, "signature"):
            self.run_ota(result)


if __name__ == "__main__":
    unittest.main()
