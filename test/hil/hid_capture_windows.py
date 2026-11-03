# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""HOST-side wire check on Windows: capture what KVMpad's keyboard and mouse interfaces deliver.

    python -I test/hil/hid_capture_windows.py --probe keys|rollover|move|chords|soak --seconds 8 [--vid 303A --pid 400C]

Run it on the HOST the board is plugged into, start the matching `hid probe <name>` on the board console
(bench builds, CONFIG_KVM_HID_BENCH_PROBE) while it listens. Raw Input (RIDEV_INPUTSINK on a message-only window)
receives every keyboard and mouse event together with the device handle, so events of other keyboards and mice
are ignored by the VID/PID filter. Raw Input reports what Windows decoded from the HID reports: key make/break
with the virtual key, and relative mouse motion per report without pointer acceleration.

Privacy: only events of the KVMpad device are kept, and only for the probe's fixed usages (F13..F24, motion, and
in chord and soak mode Ctrl, Shift, Alt, Win, Tab, Esc and D).
Any other key of the device is counted as "unexpected" without recording which key it was. Standard library only.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import sys
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

WM_INPUT = 0x00FF
RIM_TYPEMOUSE = 0
RIM_TYPEKEYBOARD = 1
RID_INPUT = 0x10000003
RIDI_DEVICENAME = 0x20000007
RIDEV_INPUTSINK = 0x00000100
RI_KEY_BREAK = 0x01
HWND_MESSAGE = wt.HWND(-3)
PM_REMOVE = 0x0001
VK_F13, VK_F24 = 0x7C, 0x87
# The chord probe's keys (AT-HID-06/08); Raw Input reports the generic modifier VKs.
VK_TAB, VK_SHIFT, VK_CONTROL, VK_MENU, VK_ESCAPE, VK_D, VK_LWIN = 0x09, 0x10, 0x11, 0x12, 0x1B, 0x44, 0x5B
CHORD_VKS = {VK_TAB: "Tab", VK_SHIFT: "Shift", VK_CONTROL: "Ctrl", VK_MENU: "Alt", VK_ESCAPE: "Esc", VK_D: "D",
             VK_LWIN: "Win"}

LRESULT = ctypes.c_ssize_t
WNDPROC = ctypes.WINFUNCTYPE(LRESULT, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)


class RAWINPUTDEVICE(ctypes.Structure):
    _fields_ = [("usUsagePage", wt.USHORT), ("usUsage", wt.USHORT), ("dwFlags", wt.DWORD), ("hwndTarget", wt.HWND)]


class RAWINPUTHEADER(ctypes.Structure):
    _fields_ = [("dwType", wt.DWORD), ("dwSize", wt.DWORD), ("hDevice", wt.HANDLE), ("wParam", wt.WPARAM)]


class RAWMOUSE(ctypes.Structure):
    _fields_ = [("usFlags", wt.USHORT), ("usButtonFlags", wt.USHORT), ("usButtonData", wt.USHORT),
                ("ulRawButtons", wt.ULONG), ("lLastX", wt.LONG), ("lLastY", wt.LONG), ("ulExtraInformation", wt.ULONG)]


class RAWKEYBOARD(ctypes.Structure):
    _fields_ = [("MakeCode", wt.USHORT), ("Flags", wt.USHORT), ("Reserved", wt.USHORT), ("VKey", wt.USHORT),
                ("Message", wt.UINT), ("ExtraInformation", wt.ULONG)]


class _DATA(ctypes.Union):
    _fields_ = [("mouse", RAWMOUSE), ("keyboard", RAWKEYBOARD)]


class RAWINPUT(ctypes.Structure):
    _fields_ = [("header", RAWINPUTHEADER), ("data", _DATA)]


class WNDCLASSW(ctypes.Structure):
    _fields_ = [("style", wt.UINT), ("lpfnWndProc", WNDPROC), ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int),
                ("hInstance", wt.HINSTANCE), ("hIcon", wt.HICON), ("hCursor", wt.HANDLE), ("hbrBackground", wt.HBRUSH),
                ("lpszMenuName", wt.LPCWSTR), ("lpszClassName", wt.LPCWSTR)]


user32.DefWindowProcW.restype = LRESULT
user32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.GetRawInputData.argtypes = [wt.HANDLE, wt.UINT, ctypes.c_void_p, ctypes.POINTER(wt.UINT), wt.UINT]
user32.GetRawInputDeviceInfoW.argtypes = [wt.HANDLE, wt.UINT, ctypes.c_void_p, ctypes.POINTER(wt.UINT)]
user32.CreateWindowExW.restype = wt.HWND
user32.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, wt.HWND, wt.HMENU, wt.HINSTANCE, wt.LPVOID]
user32.PeekMessageW.argtypes = [ctypes.POINTER(wt.MSG), wt.HWND, wt.UINT, wt.UINT, wt.UINT]


class Capture:
    def __init__(self, vid: str, pid: str):
        self.tag = f"VID_{vid.upper()}&PID_{pid.upper()}"
        self.names: dict[int, bool] = {}
        self.keys: list[tuple[int, bool]] = []   # (vkey F13..F24, down)
        self.chord_keys: list[tuple[int, bool]] = []  # (vkey of CHORD_VKS, down), chord mode only
        self.soak: list[tuple[int, int, bool]] = []  # (device handle, vkey, down), soak mode: F13..F24 + Ctrl/Shift
        self.chord_mode = False
        self.soak_mode = False
        self.unexpected_keys = 0
        self.moves: list[tuple[int, int]] = []
        self.buttons = 0
        self.other_devices = 0

    def is_ours(self, handle) -> bool:
        h = int(handle or 0)
        if h not in self.names:
            size = wt.UINT(0)
            user32.GetRawInputDeviceInfoW(handle, RIDI_DEVICENAME, None, ctypes.byref(size))
            buf = ctypes.create_unicode_buffer(size.value + 1)
            user32.GetRawInputDeviceInfoW(handle, RIDI_DEVICENAME, buf, ctypes.byref(size))
            self.names[h] = self.tag in buf.value.upper()
        return self.names[h]

    def on_input(self, lparam) -> None:
        size = wt.UINT(0)
        hdr = ctypes.sizeof(RAWINPUTHEADER)
        user32.GetRawInputData(lparam, RID_INPUT, None, ctypes.byref(size), hdr)
        buf = ctypes.create_string_buffer(size.value)
        if user32.GetRawInputData(lparam, RID_INPUT, buf, ctypes.byref(size), hdr) != size.value:
            return
        ri = ctypes.cast(buf, ctypes.POINTER(RAWINPUT)).contents
        if not self.is_ours(ri.header.hDevice):
            self.other_devices += 1
            return
        if ri.header.dwType == RIM_TYPEKEYBOARD:
            k = ri.data.keyboard
            if self.chord_mode and (VK_F13 <= k.VKey <= VK_F24 or k.VKey in (VK_CONTROL, VK_SHIFT)) and self.soak_mode:
                self.soak.append((int(ri.header.hDevice or 0), k.VKey, not (k.Flags & RI_KEY_BREAK)))
            elif VK_F13 <= k.VKey <= VK_F24:
                self.keys.append((k.VKey, not (k.Flags & RI_KEY_BREAK)))
            elif self.chord_mode and k.VKey in CHORD_VKS:
                self.chord_keys.append((k.VKey, not (k.Flags & RI_KEY_BREAK)))
            else:
                self.unexpected_keys += 1  # never record which key
        elif ri.header.dwType == RIM_TYPEMOUSE:
            m = ri.data.mouse
            if m.lLastX or m.lLastY:
                self.moves.append((m.lLastX, m.lLastY))
            if m.usButtonFlags:
                self.buttons += 1


def run(vid: str, pid: str, seconds: float, chord_mode: bool = False, soak_mode: bool = False) -> Capture:
    cap = Capture(vid, pid)
    cap.chord_mode = chord_mode or soak_mode
    cap.soak_mode = soak_mode

    @WNDPROC
    def wndproc(hwnd, msg, wparam, lparam):
        if msg == WM_INPUT:
            cap.on_input(lparam)
        return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

    hinst = kernel32.GetModuleHandleW(None)
    wc = WNDCLASSW(lpfnWndProc=wndproc, hInstance=hinst, lpszClassName="KvmpadHidCapture")
    user32.RegisterClassW(ctypes.byref(wc))
    hwnd = user32.CreateWindowExW(0, "KvmpadHidCapture", "kvmpad", 0, 0, 0, 0, 0, HWND_MESSAGE, None, hinst, None)
    if not hwnd:
        raise OSError(ctypes.get_last_error(), "CreateWindowExW")
    devs = (RAWINPUTDEVICE * 2)((0x01, 0x06, RIDEV_INPUTSINK, hwnd), (0x01, 0x02, RIDEV_INPUTSINK, hwnd))
    if not user32.RegisterRawInputDevices(devs, 2, ctypes.sizeof(RAWINPUTDEVICE)):
        raise OSError(ctypes.get_last_error(), "RegisterRawInputDevices")
    msg = wt.MSG()
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        while user32.PeekMessageW(ctypes.byref(msg), hwnd, 0, 0, PM_REMOVE):
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
        time.sleep(0.002)
    user32.DestroyWindow(hwnd)
    return cap


# `hid probe chords` sends, 600 ms apart: Alt+Tab, Alt+Tab (back), Win+D, Win+D (back), Ctrl+Shift+Esc, then an
# Alt+Tab interrupted by RELEASE_ALL (AT-HID-08, revoke half). Expected order per chord (02 section 7.3): every
# modifier make, then the key make, the key break, then the modifier breaks.
CHORDS = [((VK_MENU,), VK_TAB), ((VK_MENU,), VK_TAB), ((VK_LWIN,), VK_D), ((VK_LWIN,), VK_D),
          ((VK_CONTROL, VK_SHIFT), VK_ESCAPE)]


CHORD_NOTES: list[str] = []


def _chord_ok(got: list[tuple[int, bool]], mods: tuple, key: int, key_make: bool) -> bool:
    k = len(mods)
    want_n = 2 * k + (2 if key_make else 1)
    if len(got) != want_n or not all(d for _, d in got[:k]) or sorted(v for v, _ in got[:k]) != sorted(mods):
        return False
    j = k
    if key_make:
        if got[j] != (key, True):
            return False
        j += 1
    return (got[j] == (key, False) and not any(d for _, d in got[j + 1:])
            and sorted(v for v, _ in got[j + 1:]) == sorted(mods))


def check_chords(ev: list[tuple[int, bool]]) -> list[str]:
    # Windows does not deliver to Raw Input the make of a key it consumes as a system hotkey (Alt+Tab, Win+D,
    # Ctrl+Shift+Esc), only its break - found on board #1, where the chords visibly acted on the desktop. A chord
    # whose key make is missing but whose modifier makes, key break and modifier breaks come in order is accepted
    # and listed in CHORD_NOTES; the HOST-side effect is the evidence that the make reached the OS.
    fails = []
    CHORD_NOTES.clear()
    i = 0
    for n, (mods, key) in enumerate(CHORDS, 1):
        name = f"chord {n} ({'+'.join(CHORD_VKS[m] for m in mods)}+{CHORD_VKS[key]})"
        full = 2 * len(mods) + 2
        if _chord_ok(ev[i:i + full], mods, key, True):
            i += full
            continue
        if _chord_ok(ev[i:i + full - 1], mods, key, False):
            CHORD_NOTES.append(f"{name}: key make consumed by the system hotkey (not delivered to Raw Input)")
            i += full - 1
            continue
        got = ev[i:i + full]
        i += full
        fails.append(f"{name}: got {[(CHORD_VKS[v], 'make' if d else 'break') for v, d in got]}")
    # The cut Alt+Tab may reach the wire whole, in part or not at all; only the breaks are checked.
    held: dict[int, int] = {}
    for vk, d in ev:
        held[vk] = max(0, held.get(vk, 0) + (1 if d else -1))
    if any(held.values()):
        fails.append(f"keys without a break at the end: {[CHORD_VKS[v] for v, c in held.items() if c]}")
    return fails


SOAK_NOTES: list[str] = []


def check_soak(ev: list[tuple[int, int, bool]]) -> tuple[list[str], dict]:
    """`hid probe soak` (AT-HID-08 unplug half, AT-HID-09): Ctrl / Shift / Ctrl+Shift + F13..F24 while the cable is
    pulled at random. Each re-plug is a new Raw Input device handle. Fails: a make of a key already down on the same
    handle (a phantom repeat), a break of a key that is not down, or a key still down on the last handle at the end.
    Keys down on a handle that went away are the shortcut the unplug cut: Windows drops them with the device; they
    are counted in `cut_by_unplug`, not failed."""
    fails = []
    held: dict[int, set[int]] = {}
    order: list[int] = []
    shortcuts = 0
    repeats = 0
    stray_breaks = 0
    for h, vk, down in ev:
        if h not in held:
            held[h] = set()
            order.append(h)
        s = held[h]
        if down:
            if vk in s:
                repeats += 1
            s.add(vk)
            if VK_F13 <= vk <= VK_F24:
                shortcuts += 1
        else:
            if vk not in s:
                stray_breaks += 1
            s.discard(vk)
    if repeats:
        fails.append(f"{repeats} phantom repeats (make of a key already down)")
    if stray_breaks:
        fails.append(f"{stray_breaks} breaks of a key that was not down")
    last = order[-1] if order else None
    if last is not None and held[last]:
        fails.append(f"keys still down at the end: {sorted(CHORD_VKS.get(v, 'F%d' % (v - VK_F13 + 13)) for v in held[last])}")
    cut = sum(1 for h in order[:-1] if held[h])
    return fails, {"device_handles": len(order), "shortcut_makes": shortcuts, "cut_by_unplug": cut}


def check(probe: str, cap: Capture) -> list[str]:
    fails = []
    if cap.unexpected_keys:
        fails.append(f"{cap.unexpected_keys} key events outside the probe keys")
    if cap.buttons:
        fails.append(f"{cap.buttons} mouse button events (the probes never click)")
    if probe == "keys":
        want = []
        for vk in range(VK_F13, VK_F24 + 1):
            want += [(vk, True), (vk, False)]
        if cap.keys != want:
            fails.append(f"key sequence differs: got {len(cap.keys)} events, want {len(want)} (F13..F24 make/break)")
    elif probe == "rollover":
        downs = [vk for vk, d in cap.keys if d]
        ups = [vk for vk, d in cap.keys if not d]
        if downs != list(range(VK_F13, VK_F13 + 6)):
            fails.append(f"downs {['F%d' % (v - VK_F13 + 13) for v in downs]} != F13..F18 (7th must be refused)")
        if sorted(ups) != sorted(downs):
            fails.append("every make must have its break")
    elif probe == "chords":
        fails += check_chords(cap.chord_keys)
    elif probe == "soak":
        f, info = check_soak(cap.soak)
        fails += f
        SOAK_NOTES.clear()
        SOAK_NOTES.append(json.dumps(info))
    elif probe == "move":
        sx = sum(x for x, _ in cap.moves)
        sy = sum(y for _, y in cap.moves)
        if (sx, sy) != (0, 0):
            fails.append(f"motion does not return to the start: sum=({sx},{sy})")
        # Windows may merge consecutive motion reports into one Raw Input event (seen: 127 + 46 = 173), so the
        # int8 split is not visible per event. An int8 overflow would show as wrong totals (300 -> 44 wraps), and
        # lost or repeated reports as a non-zero sum: those are the AT-HID-04 checks.
        if sum(abs(x) for x, _ in cap.moves) != 600 or sum(abs(y) for _, y in cap.moves) != 400:
            fails.append("total |dx| / |dy| differ from 600 / 400 (overflow, lost or duplicated motion)")
        if any(x and y for x, y in cap.moves):
            fails.append("an event moved on both axes: the probe moves one axis at a time")
    return fails


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--probe", choices=["keys", "rollover", "move", "chords", "soak"], required=True)
    ap.add_argument("--seconds", type=float, default=8.0)
    ap.add_argument("--vid", default="303A")
    ap.add_argument("--pid", default="400C")
    a = ap.parse_args()
    if sys.platform != "win32":
        print("Windows only")
        return 2
    cap = run(a.vid, a.pid, a.seconds, chord_mode=a.probe == "chords", soak_mode=a.probe == "soak")
    fails = check(a.probe, cap)
    summary = {
        "probe": a.probe,
        "kvmpad_devices_seen": sum(1 for v in cap.names.values() if v),
        "key_events": len(cap.keys),
        "chord_events": [(CHORD_VKS[v], "make" if d else "break") for v, d in cap.chord_keys],
        "motion_reports": len(cap.moves),
        "motion_sum": [sum(x for x, _ in cap.moves), sum(y for _, y in cap.moves)],
        "motion_abs_total": [sum(abs(x) for x, _ in cap.moves), sum(abs(y) for _, y in cap.moves)],
        "max_step": max([max(abs(x), abs(y)) for x, y in cap.moves], default=0),
        "other_device_events": cap.other_devices,
        "result": "PASS" if not fails else "FAIL",
        "fails": fails,
        "notes": list(CHORD_NOTES) + list(SOAK_NOTES),
    }
    print("HID_CAPTURE " + json.dumps(summary))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
