# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Check real Tk consent controls on a private Windows desktop."""
import asyncio
import ctypes
from ctypes import wintypes
import os
import subprocess
import sys
import unittest
from unittest.mock import Mock, patch


class ForegroundLayoutTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "nt", "The foreground HOST uses Windows.")
    def test_consent_reflows_and_remains_operable(self):
        subprocess.run([sys.executable, "-B", __file__, "--private-desktop"],
                       check=True, timeout=30)

    def check_bounds(self, root, widget):
        self.assertTrue(widget.winfo_ismapped())
        x = widget.winfo_rootx() - root.winfo_rootx()
        y = widget.winfo_rooty() - root.winfo_rooty()
        self.assertGreaterEqual(x, 0)
        self.assertGreaterEqual(y, 0)
        self.assertGreaterEqual(widget.winfo_width(), widget.winfo_reqwidth(), str(widget))
        self.assertGreaterEqual(widget.winfo_height(), widget.winfo_reqheight())
        self.assertLessEqual(x + widget.winfo_width(), root.winfo_width())
        self.assertLessEqual(y + widget.winfo_height(), root.winfo_height())

    def check_contrast(self, root, style, palette):
        def luminance(color):
            channels = (part / 65535 for part in root.winfo_rgb(color))
            linear = [part / 12.92 if part <= .04045 else ((part + .055) / 1.055) ** 2.4 for part in channels]
            return sum(value * weight for value, weight in zip(linear, (.2126, .7152, .0722)))

        def check_pair(widget, foreground, background, states, minimum):
            fg = style.lookup(widget, foreground, states)
            bg = style.lookup(widget, background, states)
            lo, hi = sorted((luminance(fg), luminance(bg)))
            self.assertGreaterEqual((hi + .05) / (lo + .05), minimum, (widget, states, fg, bg))

        for states in (("focus",), ("focus", "active"), ("focus", "pressed")):
            check_pair("TButton", "focuscolor", "background", states, 3)
            check_pair("TButton", "foreground", "background", states, 4.5)
        for states in ((), ("focus",)):
            check_pair("TEntry", "selectforeground", "selectbackground", states, 4.5)
        check_pair("TEntry", "bordercolor", "fieldbackground", ("focus",), 3)
        self.assertEqual(style.lookup("TEntry", "bordercolor", ("focus",)), palette[4])
        self.assertEqual(style.lookup("TEntry", "lightcolor", ("focus",)), palette[4])

    async def check_window(self):
        import appearance
        import host
        from media import capture_review

        palettes = {
            "day": ("#f4f6f8", "#172332", "#ffffff", "#172332", "#15559a", "#ffffff", "#526174"),
            "night": ("#16202b", "#ecf2f8", "#263442", "#ecf2f8", "#9bceff", "#12202d", "#b2c0cc"),
            "contrast": ("#000000", "#ffffff", "#000000", "#ffffff", "#1aebff", "#000000", "#a6a6a6"),
            "white contrast": ("#ffffff", "#000000", "#ffffff", "#000000", "#37006e", "#ffffff", "#505050"),
        }
        for name, palette in palettes.items():
            with self.subTest(palette=name), patch.object(appearance, "system_palette", return_value=palette):
                ui = host.Foreground(bytes(range(32)))
                ui.broker = Mock()
                root = ui.root
                try:
                    ui.configure_obs({}, None, None)
                    self.check_contrast(root, ui.appearance.style, palette)
                    ui.status("Telemetry active")
                    review = asyncio.create_task(ui.approve("com.example.flight-panel",
                                                           capture_review("cinepak-intra", "OVERVIEW")))
                    await asyncio.sleep(0)
                    heights = []
                    for size in ("600x440", "340x280", "100x100", "600x440"):
                        root.geometry(size)
                        root.update()
                        if size == "340x280":
                            self.assertEqual(root.winfo_width(), 340)
                        self.assertFalse(review.done())
                        widgets = list(descendants(root))
                        for widget in widgets:
                            if widget.winfo_class() in ("TLabel", "TButton", "TEntry"):
                                self.check_bounds(root, widget)
                        heights.append(ui.review.winfo_height())
                    self.assertGreater(heights[1], heights[0])
                    self.assertEqual(heights[0], heights[-1])
                    controls = {w.cget("text"): w for w in widgets if w.winfo_class() == "TButton"}
                    self.assertEqual(ui.allow.tk_focusNext(), ui.deny)
                    self.assertEqual(ui.deny.tk_focusNext(), controls["Stop session"])
                    self.assertEqual(controls["Stop session"].tk_focusNext(), controls["Revoke and exit"])
                    controls["Deny"].invoke()
                    self.assertFalse(await review)
                    self.assertEqual(ui.review.cget("text"), "")
                    self.assertTrue(ui.allow.instate(["disabled"]))
                    review = asyncio.create_task(ui.approve("com.example.flight-panel", "Second request"))
                    await asyncio.sleep(0)
                    controls["Allow once"].invoke()
                    self.assertTrue(await review)
                    ui.broker.stop.assert_not_called()
                    controls["Stop session"].invoke()
                    ui.broker.stop.assert_called_once_with()
                    self.assertFalse(ui.closed)
                    controls["Revoke and exit"].invoke()
                    self.assertTrue(ui.closed)
                finally:
                    ui.decide(False)
                    await asyncio.sleep(0)
                    root.update_idletasks()
                    root.destroy()


def descendants(parent):
    for widget in parent.winfo_children():
        yield widget
        yield from descendants(widget)


def use_private_desktop():
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.CreateDesktopW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR, ctypes.c_void_p,
                                    wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
    user.CreateDesktopW.restype = wintypes.HANDLE
    user.SetThreadDesktop.argtypes = [wintypes.HANDLE]
    desktop = user.CreateDesktopW(f"KVMpadConsentTest{os.getpid()}", None, None, 0, 0x1ff, None)
    if not desktop or not user.SetThreadDesktop(desktop):
        raise ctypes.WinError(ctypes.get_last_error())


if __name__ == "__main__":
    if sys.argv[1:] == ["--private-desktop"]:
        use_private_desktop()
        asyncio.run(ForegroundLayoutTests().check_window())
    else:
        unittest.main()
