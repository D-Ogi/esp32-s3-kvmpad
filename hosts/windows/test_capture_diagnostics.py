# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Check capture-helper failures using synthetic pipes without opening a display."""
import asyncio
import os
import sys
import unittest
from unittest.mock import AsyncMock, patch

import media


class Diagnostics(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.enterContext(patch.dict(os.environ))
        os.environ.pop("KVMPAD_HOST_DISPLAY", None)

    def test_monitor_entry_exit_and_geometry_have_distinct_foreground_reasons(self):
        for stage, explanation in (("cursorwait", "did not reach"), ("cursor", "left the selected"),
                                   ("cursorpos", "physical pointer"), ("geometry", "physical geometry")):
            record = f"KCE1 {stage} 800704C7\n".encode("ascii")
            self.assertIn(explanation, str(media.CaptureError.from_record(record, "frame")))

    def test_untrusted_messages_remain_local(self):
        fallback = str(media.CaptureError("picker"))
        for record in (b"window title\n", b"KCE1 unknown 80004002\n", b"KCE1 monitor non-code\n",
                       b"KCE1 monitor 80004002 private\n"):
            self.assertEqual(str(media.CaptureError.from_record(record, "picker")), fallback)
        self.assertEqual(str(media.CaptureError("unknown text", 123)), fallback)

    def synthetic(self, output):
        capture = media.Capture(None, None)
        spawn = media.child

        def start(*args, diagnostics=False):
            return spawn(sys.executable, "-u", "-c", output, diagnostics=diagnostics)

        self.addAsyncCleanup(capture.close)
        return capture, patch.object(media, "child", start)

    async def test_rejected_item_before_monitor_identity(self):
        capture, mock = self.synthetic(
            "import sys; sys.stderr.buffer.write(b'KCE1 monitor 80004002\\n'); sys.exit(3)")
        with mock, self.assertRaisesRegex(media.CaptureError, r"selected monitor.*0x80004002"):
            await capture.open("jpeg", "PIXEL_1X")

    async def test_failure_after_the_selected_monitor_was_announced(self):
        capture, mock = self.synthetic(
            "import sys; sys.stdout.buffer.write(rb'\\\\.\\DISPLAY1' + b'\\n'); sys.stdin.buffer.read(1); "
            "sys.stderr.buffer.write(b'KCE1 start 80070005\\n'); sys.exit(3)")
        with mock:
            self.assertEqual(await capture.open("jpeg", "PIXEL_1X"), r"\\.\DISPLAY1")
        with self.assertRaisesRegex(media.CaptureError, r"start capture.*0x80070005"):
            await asyncio.wait_for(capture.frame(1, 0), 4)
        with self.assertRaisesRegex(media.CaptureError, r"start capture.*0x80070005"):
            await capture.wait_closed()

    async def test_cursor_entry_can_expire_without_approving_a_monitor(self):
        capture, fake = self.synthetic(
            "import sys; sys.stdout.buffer.write(b'KCP1 CURSOR\\n'); sys.stdin.buffer.read(1)")
        with fake, patch.object(media, "CURSOR_ENTRY_TIMEOUT", .1), self.assertRaises(media.CaptureError) as failed:
            await capture.open("jpeg", "PIXEL_1X")
        self.assertEqual(failed.exception.stage, "cursorwait")
        self.assertFalse(capture.selected)
        self.assertTrue(capture.revoked)

    async def test_cursor_progress_cannot_replace_the_selected_display_identity(self):
        for second in (b"KCP1 CURSOR\n", b"private title\n"):
            output = b"KCP1 CURSOR\n" + second
            capture, fake = self.synthetic(f"import sys; sys.stdout.buffer.write({output!r})")
            with fake, self.assertRaises(media.CaptureError) as failed:
                await capture.open("jpeg", "PIXEL_1X")
            self.assertEqual(failed.exception.stage, "name")
            self.assertFalse(capture.selected)

    async def test_pointer_exit_before_the_monitor_identity(self):
        for progress in (b"", b"KCP1 CURSOR\n"):
            with self.subTest(progress=bool(progress)):
                capture, fake = self.synthetic(
                    f"import sys; sys.stdout.buffer.write({progress!r}); "
                    "sys.stderr.buffer.write(b'KCE1 cursor 800704C7\\n'); sys.exit(3)")
                with fake, self.assertRaises(media.CaptureError) as failed:
                    await capture.open("jpeg", "PIXEL_1X")
                self.assertEqual(failed.exception.stage, "cursor")
                self.assertFalse(capture.selected)
                self.assertTrue(capture.revoked)

    async def test_named_pointer_hint_requires_the_same_final_display(self):
        for initial, final, valid_hint in (
                (b"window title", rb"\\.\DISPLAY2", False),
                (rb"\\.\DISPLAY123456", rb"\\.\DISPLAY2", False),
                (rb"\\.\DISPLAY2", rb"\\.\DISPLAY3", True)):
            data = b"KCP2 CURSOR " + initial + b"\n" + final + b"\n"
            capture, fake = self.synthetic(f"import sys; sys.stdout.buffer.write({data!r})")
            progress = AsyncMock()
            with fake, self.assertRaises(media.CaptureError) as failed:
                await capture.open("jpeg", "PIXEL_1X", progress)
            self.assertEqual(failed.exception.stage, "name")
            if valid_hint:
                progress.assert_awaited_once_with(r"\\.\DISPLAY2")
            else:
                progress.assert_not_awaited()
            self.assertFalse(capture.selected)
            self.assertTrue(capture.revoked)

    async def test_display_restriction_is_explicit_and_checked_before_launch(self):
        self.assertIsNone(media.required_display())
        for value in ("", "display2", "DISPLAY", "DISPLAY123456", "DISPLAY２", " DISPLAY2", "DISPLAY2\n",
                      r"\\.\DISPLAY2\other", "Monitor 2"):
            with self.subTest(value=value), patch.dict(os.environ, KVMPAD_HOST_DISPLAY=value):
                capture = media.Capture(None, None)
                with patch.object(media, "child") as picker, self.assertRaises(media.CaptureError) as failed:
                    await capture.open("jpeg", "PIXEL_1X")
                self.assertEqual(failed.exception.stage, "displayenv")
                self.assertFalse(capture.selected)
                picker.assert_not_called()
        for value in ("DISPLAY2", r"\\.\DISPLAY2"):
            with patch.dict(os.environ, KVMPAD_HOST_DISPLAY=value):
                self.assertEqual(media.required_display(), r"\\.\DISPLAY2")
                self.assertIn("choose DISPLAY2", media.capture_review("jpeg", "PIXEL_1X"))

    async def test_display_restriction_checks_hint_and_legacy_final_identity(self):
        required, other = rb"\\.\DISPLAY2" + b"\n", rb"\\.\DISPLAY1" + b"\n"
        for prefix, final, accepted, named_progress in (
                (b"KCP2 CURSOR " + other, b"", False, False),
                (b"KCP2 CURSOR " + required, other, False, True),
                (b"KCP1 CURSOR\n", other, False, False),
                (b"", other, False, False),
                (b"KCP2 CURSOR " + required, required, True, True),
                (b"KCP1 CURSOR\n", required, True, False),
                (b"", required, True, False)):
            with self.subTest(prefix=prefix, final=final), patch.dict(os.environ, KVMPAD_HOST_DISPLAY="DISPLAY2"):
                data = prefix + final
                capture, fake = self.synthetic(f"import sys; sys.stdout.buffer.write({data!r}); sys.stdin.buffer.read(1)")
                progress = AsyncMock()
                with fake:
                    if accepted:
                        self.assertEqual(await capture.open("jpeg", "PIXEL_1X", progress), r"\\.\DISPLAY2")
                    else:
                        with self.assertRaises(media.CaptureError) as failed:
                            await capture.open("jpeg", "PIXEL_1X", progress)
                        self.assertEqual(failed.exception.stage, "wrongdisplay")
                        with self.assertRaises(PermissionError):
                            await capture.frame(1, 1)
                self.assertEqual(capture.selected, accepted)
                self.assertEqual(capture.revoked, not accepted)
                if named_progress:
                    progress.assert_awaited_once_with(r"\\.\DISPLAY2")
                else:
                    progress.assert_not_awaited()

    @unittest.skipUnless(os.environ.get("KVMPAD_TEST_PICKER"), "Native picker executable required")
    async def test_native_invalid_parent_has_a_bounded_diagnostic(self):
        capture = media.Capture(os.environ["KVMPAD_TEST_PICKER"], None)
        self.addAsyncCleanup(capture.close)
        spawn = media.child

        def no_parent(*args, diagnostics=False):
            # The production helper rejects PID 0 before creating any UI or capture item.
            return spawn(*args[:-1], "0", diagnostics=diagnostics)

        with patch.object(media, "child", no_parent), self.assertRaisesRegex(
                media.CaptureError, r"verify its parent process.*0x[0-9A-F]{8}"):
            await capture.open("jpeg", "PIXEL_1X")


if __name__ == "__main__":
    unittest.main(verbosity=2)
