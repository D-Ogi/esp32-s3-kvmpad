// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

namespace KVMpad.Client.Core.Input;

/// <summary>
/// USB HID usages on page 0x07, mirroring <c>components/hid_core/include/hid_keymap.h</c> so the two sides
/// of the protocol cannot drift. A usage is never derived from a character or a legend (03 §5.2, FR-743):
/// the character tables in <c>TypeText</c> map to these constants explicitly.
/// </summary>
public static class HidUsage
{
    public const byte None = 0x00;

    public const byte A = 0x04;
    public const byte B = 0x05;
    public const byte C = 0x06;
    public const byte D = 0x07;
    public const byte E = 0x08;
    public const byte F = 0x09;
    public const byte G = 0x0A;
    public const byte H = 0x0B;
    public const byte I = 0x0C;
    public const byte J = 0x0D;
    public const byte K = 0x0E;
    public const byte L = 0x0F;
    public const byte M = 0x10;
    public const byte N = 0x11;
    public const byte O = 0x12;
    public const byte P = 0x13;
    public const byte Q = 0x14;
    public const byte R = 0x15;
    public const byte S = 0x16;
    public const byte T = 0x17;
    public const byte U = 0x18;
    public const byte V = 0x19;
    public const byte W = 0x1A;
    public const byte X = 0x1B;
    public const byte Y = 0x1C;
    public const byte Z = 0x1D;

    public const byte D1 = 0x1E;
    public const byte D2 = 0x1F;
    public const byte D3 = 0x20;
    public const byte D4 = 0x21;
    public const byte D5 = 0x22;
    public const byte D6 = 0x23;
    public const byte D7 = 0x24;
    public const byte D8 = 0x25;
    public const byte D9 = 0x26;
    public const byte D0 = 0x27;

    public const byte Enter = 0x28;
    public const byte Escape = 0x29;
    public const byte Backspace = 0x2A;
    public const byte Tab = 0x2B;
    public const byte Space = 0x2C;
    public const byte Minus = 0x2D;
    public const byte Equal = 0x2E;
    public const byte LeftBracket = 0x2F;
    public const byte RightBracket = 0x30;
    public const byte Backslash = 0x31;
    public const byte NonUsHash = 0x32;
    public const byte Semicolon = 0x33;
    public const byte Apostrophe = 0x34;
    public const byte Grave = 0x35;
    public const byte Comma = 0x36;
    public const byte Period = 0x37;
    public const byte Slash = 0x38;
    public const byte CapsLock = 0x39;

    public const byte F1 = 0x3A;
    public const byte F2 = 0x3B;
    public const byte F3 = 0x3C;
    public const byte F4 = 0x3D;
    public const byte F5 = 0x3E;
    public const byte F6 = 0x3F;
    public const byte F7 = 0x40;
    public const byte F8 = 0x41;
    public const byte F9 = 0x42;
    public const byte F10 = 0x43;
    public const byte F11 = 0x44;
    public const byte F12 = 0x45;

    public const byte PrintScreen = 0x46;
    public const byte ScrollLock = 0x47;
    public const byte Pause = 0x48;
    public const byte Insert = 0x49;
    public const byte Home = 0x4A;
    public const byte PageUp = 0x4B;
    public const byte Delete = 0x4C;
    public const byte End = 0x4D;
    public const byte PageDown = 0x4E;
    public const byte Right = 0x4F;
    public const byte Left = 0x50;
    public const byte Down = 0x51;
    public const byte Up = 0x52;

    public const byte NumLock = 0x53;
    public const byte KeypadDivide = 0x54;
    public const byte KeypadMultiply = 0x55;
    public const byte KeypadSubtract = 0x56;
    public const byte KeypadAdd = 0x57;
    public const byte KeypadEnter = 0x58;
    public const byte Keypad1 = 0x59;
    public const byte Keypad2 = 0x5A;
    public const byte Keypad3 = 0x5B;
    public const byte Keypad4 = 0x5C;
    public const byte Keypad5 = 0x5D;
    public const byte Keypad6 = 0x5E;
    public const byte Keypad7 = 0x5F;
    public const byte Keypad8 = 0x60;
    public const byte Keypad9 = 0x61;
    public const byte Keypad0 = 0x62;
    public const byte KeypadPeriod = 0x63;

    public const byte NonUsBackslash = 0x64;
    public const byte Application = 0x65;

    public const byte F13 = 0x68;
    public const byte F24 = 0x73;

    /// <summary>Highest non-modifier usage the device accepts (hid_keymap.h KVM_KEY_LAST_REGULAR).</summary>
    public const byte LastRegular = 0xA4;

    public const byte LeftCtrl = 0xE0;
    public const byte LeftShift = 0xE1;
    public const byte LeftAlt = 0xE2;
    public const byte LeftGui = 0xE3;
    public const byte RightCtrl = 0xE4;
    public const byte RightShift = 0xE5;
    public const byte RightAlt = 0xE6;
    public const byte RightGui = 0xE7;

    /// <summary>True for a modifier usage (0xE0..0xE7).</summary>
    public static bool IsModifier(byte usage) => usage >= LeftCtrl && usage <= RightGui;

    /// <summary>
    /// The same predicate as <c>kvm_usage_is_valid_key()</c>: 0x04..0xA4 plus the modifiers. Anything else
    /// would come back as <c>ERR_USAGE</c>, so the client refuses to send it in the first place (03 §6.5).
    /// </summary>
    public static bool IsValidKey(byte usage) => (usage >= A && usage <= LastRegular) || IsModifier(usage);
}

/// <summary>Modifier bitmap bits, as the 8-byte boot keyboard report orders them (03 §5.1).</summary>
[System.Flags]
public enum HidModifiers : byte
{
    None = 0,
    LeftCtrl = 1 << 0,
    LeftShift = 1 << 1,
    LeftAlt = 1 << 2,
    LeftGui = 1 << 3,
    RightCtrl = 1 << 4,
    RightShift = 1 << 5,
    RightAlt = 1 << 6,
    RightGui = 1 << 7,
}

/// <summary>Helpers that turn a modifier bitmap into the usages the client must press and release.</summary>
public static class HidModifierUsages
{
    /// <summary>The modifier bit for a modifier usage, matching <c>KVM_MOD_BIT()</c>.</summary>
    public static HidModifiers BitFor(byte usage)
        => HidUsage.IsModifier(usage) ? (HidModifiers)(byte)(1u << (usage - HidUsage.LeftCtrl)) : HidModifiers.None;

    /// <summary>The usages in a bitmap, lowest bit first, so DOWN order and UP order mirror each other.</summary>
    public static byte[] Expand(HidModifiers modifiers)
    {
        int count = System.Numerics.BitOperations.PopCount((uint)(byte)modifiers);
        byte[] usages = new byte[count];
        int index = 0;
        for (int bit = 0; bit < 8; bit++)
        {
            if (((byte)modifiers & (1 << bit)) != 0)
                usages[index++] = (byte)(HidUsage.LeftCtrl + bit);
        }
        return usages;
    }
}
