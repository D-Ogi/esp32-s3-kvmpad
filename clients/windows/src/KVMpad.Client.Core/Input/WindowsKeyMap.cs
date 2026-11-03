// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System.Collections.Generic;

namespace KVMpad.Client.Core.Input;

/// <summary>Virtual-key codes this mapper needs. Full list in the Windows SDK; only the ambiguous keys matter here.</summary>
public static class VirtualKey
{
    public const int Cancel = 0x03;
    public const int Back = 0x08;
    public const int Tab = 0x09;
    public const int Clear = 0x0C;
    public const int Return = 0x0D;
    public const int Shift = 0x10;
    public const int Control = 0x11;
    public const int Menu = 0x12;
    public const int Pause = 0x13;
    public const int Capital = 0x14;
    public const int Escape = 0x1B;
    public const int Space = 0x20;
    public const int Prior = 0x21;
    public const int Next = 0x22;
    public const int End = 0x23;
    public const int Home = 0x24;
    public const int LeftArrow = 0x25;
    public const int UpArrow = 0x26;
    public const int RightArrow = 0x27;
    public const int DownArrow = 0x28;
    public const int Snapshot = 0x2C;
    public const int Insert = 0x2D;
    public const int Delete = 0x2E;
    public const int LeftWin = 0x5B;
    public const int RightWin = 0x5C;
    public const int Apps = 0x5D;
    public const int NumLock = 0x90;
    public const int ScrollLock = 0x91;
    public const int LeftShift = 0xA0;
    public const int RightShift = 0xA1;
    public const int LeftControl = 0xA2;
    public const int RightControl = 0xA3;
    public const int LeftMenu = 0xA4;
    public const int RightMenu = 0xA5;
}

/// <summary>
/// Windows keyboard input → USB HID usage page 0x07.
/// </summary>
/// <remarks>
/// <para>
/// The scan code is the primary key, not the virtual key: a scan code is the physical position and so is
/// independent of the layout the CLIENT machine happens to have loaded. That is what the protocol needs,
/// because the HOST applies its own layout to the usage it receives (FR-202, FR-743). Mapping from the
/// virtual key instead would double-translate every non-US layout.
/// </para>
/// <para>
/// A handful of scan codes are genuinely ambiguous on Windows (0x45 is both NumLock and Pause, 0x37 is both
/// keypad <c>*</c> and PrintScreen, the whole keypad doubles as the navigation cluster). Those are resolved
/// from the virtual key and the extended flag, which is exactly the information <c>KBDLLHOOKSTRUCT</c> carries.
/// </para>
/// </remarks>
public static class WindowsKeyMap
{
    private readonly record struct ScanKey(int ScanCode, bool Extended);

    private static readonly Dictionary<ScanKey, byte> ScanTable = BuildScanTable();
    private static readonly Dictionary<int, byte> VirtualKeyFallback = BuildVirtualKeyFallback();

    /// <summary>
    /// Resolves one keyboard event to a HID usage.
    /// </summary>
    /// <param name="scanCode">Scan code set 1 make code, as reported by <c>KBDLLHOOKSTRUCT.scanCode</c>.</param>
    /// <param name="virtualKey">Virtual-key code, used only to disambiguate.</param>
    /// <param name="extended">The <c>LLKHF_EXTENDED</c> flag.</param>
    /// <param name="usage">The usage on page 0x07.</param>
    /// <returns>False when this key has no usage the device would accept; the caller then drops the event.</returns>
    public static bool TryMap(int scanCode, int virtualKey, bool extended, out byte usage)
    {
        // Keys whose scan code alone is not decisive.
        switch (virtualKey)
        {
            case VirtualKey.Pause:
            case VirtualKey.Cancel:
                usage = HidUsage.Pause;
                return true;
            case VirtualKey.NumLock:
                usage = HidUsage.NumLock;
                return true;
            case VirtualKey.Snapshot:
                usage = HidUsage.PrintScreen;
                return true;
            case VirtualKey.ScrollLock:
                usage = HidUsage.ScrollLock;
                return true;
        }

        if (ScanTable.TryGetValue(new ScanKey(scanCode, extended), out usage))
            return true;

        // Some keyboards and all injected input report the non-extended form of an extended key.
        if (extended && ScanTable.TryGetValue(new ScanKey(scanCode, false), out usage))
            return true;

        // Injected input may carry scanCode 0; the virtual key is then all there is.
        if (VirtualKeyFallback.TryGetValue(virtualKey, out usage))
            return true;

        usage = HidUsage.None;
        return false;
    }

    /// <summary>True when the device would accept the usage this event maps to.</summary>
    public static bool IsMappable(int scanCode, int virtualKey, bool extended)
        => TryMap(scanCode, virtualKey, extended, out byte usage) && HidUsage.IsValidKey(usage);

    private static Dictionary<ScanKey, byte> BuildScanTable()
    {
        var table = new Dictionary<ScanKey, byte>(128);

        void Plain(int scan, byte hid) => table[new ScanKey(scan, false)] = hid;
        void Extended(int scan, byte hid) => table[new ScanKey(scan, true)] = hid;

        Plain(0x01, HidUsage.Escape);
        Plain(0x02, HidUsage.D1);
        Plain(0x03, HidUsage.D2);
        Plain(0x04, HidUsage.D3);
        Plain(0x05, HidUsage.D4);
        Plain(0x06, HidUsage.D5);
        Plain(0x07, HidUsage.D6);
        Plain(0x08, HidUsage.D7);
        Plain(0x09, HidUsage.D8);
        Plain(0x0A, HidUsage.D9);
        Plain(0x0B, HidUsage.D0);
        Plain(0x0C, HidUsage.Minus);
        Plain(0x0D, HidUsage.Equal);
        Plain(0x0E, HidUsage.Backspace);
        Plain(0x0F, HidUsage.Tab);

        Plain(0x10, HidUsage.Q);
        Plain(0x11, HidUsage.W);
        Plain(0x12, HidUsage.E);
        Plain(0x13, HidUsage.R);
        Plain(0x14, HidUsage.T);
        Plain(0x15, HidUsage.Y);
        Plain(0x16, HidUsage.U);
        Plain(0x17, HidUsage.I);
        Plain(0x18, HidUsage.O);
        Plain(0x19, HidUsage.P);
        Plain(0x1A, HidUsage.LeftBracket);
        Plain(0x1B, HidUsage.RightBracket);
        Plain(0x1C, HidUsage.Enter);
        Extended(0x1C, HidUsage.KeypadEnter);
        Plain(0x1D, HidUsage.LeftCtrl);
        Extended(0x1D, HidUsage.RightCtrl);

        Plain(0x1E, HidUsage.A);
        Plain(0x1F, HidUsage.S);
        Plain(0x20, HidUsage.D);
        Plain(0x21, HidUsage.F);
        Plain(0x22, HidUsage.G);
        Plain(0x23, HidUsage.H);
        Plain(0x24, HidUsage.J);
        Plain(0x25, HidUsage.K);
        Plain(0x26, HidUsage.L);
        Plain(0x27, HidUsage.Semicolon);
        Plain(0x28, HidUsage.Apostrophe);
        Plain(0x29, HidUsage.Grave);
        Plain(0x2A, HidUsage.LeftShift);
        Plain(0x2B, HidUsage.Backslash);

        Plain(0x2C, HidUsage.Z);
        Plain(0x2D, HidUsage.X);
        Plain(0x2E, HidUsage.C);
        Plain(0x2F, HidUsage.V);
        Plain(0x30, HidUsage.B);
        Plain(0x31, HidUsage.N);
        Plain(0x32, HidUsage.M);
        Plain(0x33, HidUsage.Comma);
        Plain(0x34, HidUsage.Period);
        Plain(0x35, HidUsage.Slash);
        Extended(0x35, HidUsage.KeypadDivide);
        Plain(0x36, HidUsage.RightShift);
        Plain(0x37, HidUsage.KeypadMultiply);
        Extended(0x37, HidUsage.PrintScreen);
        Plain(0x38, HidUsage.LeftAlt);
        Extended(0x38, HidUsage.RightAlt);
        Plain(0x39, HidUsage.Space);
        Plain(0x3A, HidUsage.CapsLock);

        Plain(0x3B, HidUsage.F1);
        Plain(0x3C, HidUsage.F2);
        Plain(0x3D, HidUsage.F3);
        Plain(0x3E, HidUsage.F4);
        Plain(0x3F, HidUsage.F5);
        Plain(0x40, HidUsage.F6);
        Plain(0x41, HidUsage.F7);
        Plain(0x42, HidUsage.F8);
        Plain(0x43, HidUsage.F9);
        Plain(0x44, HidUsage.F10);
        Plain(0x45, HidUsage.NumLock);
        Plain(0x46, HidUsage.ScrollLock);

        Plain(0x47, HidUsage.Keypad7);
        Extended(0x47, HidUsage.Home);
        Plain(0x48, HidUsage.Keypad8);
        Extended(0x48, HidUsage.Up);
        Plain(0x49, HidUsage.Keypad9);
        Extended(0x49, HidUsage.PageUp);
        Plain(0x4A, HidUsage.KeypadSubtract);
        Plain(0x4B, HidUsage.Keypad4);
        Extended(0x4B, HidUsage.Left);
        Plain(0x4C, HidUsage.Keypad5);
        Plain(0x4D, HidUsage.Keypad6);
        Extended(0x4D, HidUsage.Right);
        Plain(0x4E, HidUsage.KeypadAdd);
        Plain(0x4F, HidUsage.Keypad1);
        Extended(0x4F, HidUsage.End);
        Plain(0x50, HidUsage.Keypad2);
        Extended(0x50, HidUsage.Down);
        Plain(0x51, HidUsage.Keypad3);
        Extended(0x51, HidUsage.PageDown);
        Plain(0x52, HidUsage.Keypad0);
        Extended(0x52, HidUsage.Insert);
        Plain(0x53, HidUsage.KeypadPeriod);
        Extended(0x53, HidUsage.Delete);

        Plain(0x56, HidUsage.NonUsBackslash); // ISO 102nd key
        Plain(0x57, HidUsage.F11);
        Plain(0x58, HidUsage.F12);

        Extended(0x5B, HidUsage.LeftGui);
        Extended(0x5C, HidUsage.RightGui);
        Extended(0x5D, HidUsage.Application);

        return table;
    }

    private static Dictionary<int, byte> BuildVirtualKeyFallback()
    {
        var table = new Dictionary<int, byte>(64)
        {
            [VirtualKey.Back] = HidUsage.Backspace,
            [VirtualKey.Tab] = HidUsage.Tab,
            [VirtualKey.Return] = HidUsage.Enter,
            [VirtualKey.Escape] = HidUsage.Escape,
            [VirtualKey.Space] = HidUsage.Space,
            [VirtualKey.Capital] = HidUsage.CapsLock,
            [VirtualKey.Prior] = HidUsage.PageUp,
            [VirtualKey.Next] = HidUsage.PageDown,
            [VirtualKey.End] = HidUsage.End,
            [VirtualKey.Home] = HidUsage.Home,
            [VirtualKey.LeftArrow] = HidUsage.Left,
            [VirtualKey.UpArrow] = HidUsage.Up,
            [VirtualKey.RightArrow] = HidUsage.Right,
            [VirtualKey.DownArrow] = HidUsage.Down,
            [VirtualKey.Insert] = HidUsage.Insert,
            [VirtualKey.Delete] = HidUsage.Delete,
            [VirtualKey.LeftWin] = HidUsage.LeftGui,
            [VirtualKey.RightWin] = HidUsage.RightGui,
            [VirtualKey.Apps] = HidUsage.Application,
            [VirtualKey.LeftShift] = HidUsage.LeftShift,
            [VirtualKey.RightShift] = HidUsage.RightShift,
            [VirtualKey.LeftControl] = HidUsage.LeftCtrl,
            [VirtualKey.RightControl] = HidUsage.RightCtrl,
            [VirtualKey.LeftMenu] = HidUsage.LeftAlt,
            [VirtualKey.RightMenu] = HidUsage.RightAlt,
            [VirtualKey.Shift] = HidUsage.LeftShift,
            [VirtualKey.Control] = HidUsage.LeftCtrl,
            [VirtualKey.Menu] = HidUsage.LeftAlt,
        };

        // 0-9 and A-Z occupy their ASCII code points as virtual keys.
        for (int digit = 0; digit <= 9; digit++)
            table[0x30 + digit] = digit == 0 ? HidUsage.D0 : (byte)(HidUsage.D1 + digit - 1);
        for (int letter = 0; letter < 26; letter++)
            table[0x41 + letter] = (byte)(HidUsage.A + letter);
        for (int f = 0; f < 12; f++)
            table[0x70 + f] = (byte)(HidUsage.F1 + f);

        return table;
    }
}
