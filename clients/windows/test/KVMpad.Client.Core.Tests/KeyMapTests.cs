// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using KVMpad.Client.Core.Input;
using Xunit;

namespace KVMpad.Client.Core.Tests;

/// <summary>
/// Windows input → HID usage page 0x07. The usages are checked against the table in spec 03 §5.2 and against
/// <c>components/hid_core/include/hid_keymap.h</c>, which is where the firmware reads them from.
/// </summary>
public sealed class KeyMapTests
{
    [Theory]
    // 03 §5.2, every row the spec prints.
    [InlineData(0x1E, 'A', false, 0x04)]   // A
    [InlineData(0x2C, 'Z', false, 0x1D)]   // Z
    [InlineData(0x02, '1', false, 0x1E)]   // 1
    [InlineData(0x0B, '0', false, 0x27)]   // 0
    [InlineData(0x1C, 0x0D, false, 0x28)]  // Enter
    [InlineData(0x01, 0x1B, false, 0x29)]  // Escape
    [InlineData(0x0E, 0x08, false, 0x2A)]  // Backspace
    [InlineData(0x0F, 0x09, false, 0x2B)]  // Tab
    [InlineData(0x39, 0x20, false, 0x2C)]  // Space
    [InlineData(0x3A, 0x14, false, 0x39)]  // Caps Lock
    [InlineData(0x3B, 0x70, false, 0x3A)]  // F1
    [InlineData(0x58, 0x7B, false, 0x45)]  // F12
    [InlineData(0x37, 0x2C, true, 0x46)]   // Print Screen
    [InlineData(0x52, 0x2D, true, 0x49)]   // Insert
    [InlineData(0x47, 0x24, true, 0x4A)]   // Home
    [InlineData(0x49, 0x21, true, 0x4B)]   // PageUp
    [InlineData(0x53, 0x2E, true, 0x4C)]   // Delete Forward
    [InlineData(0x4F, 0x23, true, 0x4D)]   // End
    [InlineData(0x51, 0x22, true, 0x4E)]   // PageDown
    [InlineData(0x4D, 0x27, true, 0x4F)]   // RightArrow
    [InlineData(0x4B, 0x25, true, 0x50)]   // LeftArrow
    [InlineData(0x50, 0x28, true, 0x51)]   // DownArrow
    [InlineData(0x48, 0x26, true, 0x52)]   // UpArrow
    [InlineData(0x1D, 0xA2, false, 0xE0)]  // Left Ctrl
    [InlineData(0x2A, 0xA0, false, 0xE1)]  // Left Shift
    [InlineData(0x38, 0xA4, false, 0xE2)]  // Left Alt
    [InlineData(0x5B, 0x5B, true, 0xE3)]   // Left GUI
    [InlineData(0x38, 0xA5, true, 0xE6)]   // Right Alt (AltGr)
    public void TheSpecTableMapsExactly(int scanCode, int virtualKey, bool extended, int expectedUsage)
    {
        Assert.True(WindowsKeyMap.TryMap(scanCode, virtualKey, extended, out byte usage));
        Assert.Equal((byte)expectedUsage, usage);
    }

    [Theory]
    // Scan codes the keypad and the navigation cluster share: only the extended flag tells them apart.
    [InlineData(0x47, 0x24, false, HidUsage.Keypad7)]
    [InlineData(0x47, 0x24, true, HidUsage.Home)]
    [InlineData(0x52, 0x60, false, HidUsage.Keypad0)]
    [InlineData(0x52, 0x2D, true, HidUsage.Insert)]
    [InlineData(0x1C, 0x0D, false, HidUsage.Enter)]
    [InlineData(0x1C, 0x0D, true, HidUsage.KeypadEnter)]
    [InlineData(0x35, 0xBF, false, HidUsage.Slash)]
    [InlineData(0x35, 0x6F, true, HidUsage.KeypadDivide)]
    [InlineData(0x1D, 0xA2, false, HidUsage.LeftCtrl)]
    [InlineData(0x1D, 0xA3, true, HidUsage.RightCtrl)]
    public void TheExtendedFlagResolvesTheSharedScanCodes(int scanCode, int virtualKey, bool extended, byte expected)
    {
        Assert.True(WindowsKeyMap.TryMap(scanCode, virtualKey, extended, out byte usage));
        Assert.Equal(expected, usage);
    }

    [Theory]
    // Keys whose scan code alone is ambiguous or misleading; the virtual key decides.
    [InlineData(0x45, VirtualKey.NumLock, HidUsage.NumLock)]
    [InlineData(0x45, VirtualKey.Pause, HidUsage.Pause)]
    [InlineData(0x46, VirtualKey.ScrollLock, HidUsage.ScrollLock)]
    [InlineData(0x46, VirtualKey.Cancel, HidUsage.Pause)]
    [InlineData(0x37, VirtualKey.Snapshot, HidUsage.PrintScreen)]
    public void TheVirtualKeyResolvesTheAmbiguousKeys(int scanCode, int virtualKey, byte expected)
    {
        Assert.True(WindowsKeyMap.TryMap(scanCode, virtualKey, false, out byte usage));
        Assert.Equal(expected, usage);
    }

    [Fact]
    public void TheIsoHundredAndSecondKeyMaps()
    {
        // Present on every ISO keyboard and absent from ANSI; a layout-independent map must carry it.
        Assert.True(WindowsKeyMap.TryMap(0x56, 0xE2, false, out byte usage));
        Assert.Equal(HidUsage.NonUsBackslash, usage);
    }

    [Fact]
    public void InjectedInputWithoutAScanCodeStillMaps()
    {
        // An on-screen keyboard reports scanCode 0; the virtual key is all there is to go on.
        Assert.True(WindowsKeyMap.TryMap(0, 'K', false, out byte usage));
        Assert.Equal(HidUsage.K, usage);
    }

    [Fact]
    public void AnUnknownKeyMapsToNothingRatherThanToAGuess()
    {
        // 03 §6.5 answers an unknown usage with ERR_USAGE, so the client must not invent one.
        Assert.False(WindowsKeyMap.TryMap(0x7F, 0xFF, false, out byte usage));
        Assert.Equal(HidUsage.None, usage);
    }

    [Fact]
    public void EveryMappedUsageIsOneTheDeviceAccepts()
    {
        // The same predicate as kvm_usage_is_valid_key(): 0x04..0xA4 plus the modifiers.
        for (int scan = 0; scan <= 0x5F; scan++)
        {
            foreach (bool extended in new[] { false, true })
            {
                if (WindowsKeyMap.TryMap(scan, 0, extended, out byte usage))
                    Assert.True(HidUsage.IsValidKey(usage), $"scan 0x{scan:X2} extended={extended} gave 0x{usage:X2}");
            }
        }
    }

    [Fact]
    public void UsageValidationMatchesTheFirmwarePredicate()
    {
        Assert.False(HidUsage.IsValidKey(0x00));
        Assert.False(HidUsage.IsValidKey(0x03));
        Assert.True(HidUsage.IsValidKey(0x04));
        Assert.True(HidUsage.IsValidKey(HidUsage.LastRegular));
        Assert.False(HidUsage.IsValidKey(0xA5));
        Assert.False(HidUsage.IsValidKey(0xDF));
        Assert.True(HidUsage.IsValidKey(HidUsage.LeftCtrl));
        Assert.True(HidUsage.IsValidKey(HidUsage.RightGui));
        Assert.False(HidUsage.IsValidKey(0xE8));
    }

    [Fact]
    public void ModifierBitsMatchTheBootReportLayout()
    {
        // KVM_MOD_BIT(): 0xE0 is bit 0 through 0xE7 as bit 7 (03 §5.1).
        Assert.Equal(HidModifiers.LeftCtrl, HidModifierUsages.BitFor(HidUsage.LeftCtrl));
        Assert.Equal(HidModifiers.LeftShift, HidModifierUsages.BitFor(HidUsage.LeftShift));
        Assert.Equal(HidModifiers.RightAlt, HidModifierUsages.BitFor(HidUsage.RightAlt));
        Assert.Equal(HidModifiers.RightGui, HidModifierUsages.BitFor(HidUsage.RightGui));
        Assert.Equal(HidModifiers.None, HidModifierUsages.BitFor(HidUsage.A));
    }

    [Fact]
    public void ModifierExpansionIsOrderedLowestBitFirst()
    {
        byte[] usages = HidModifierUsages.Expand(HidModifiers.LeftShift | HidModifiers.RightAlt);
        Assert.Equal([HidUsage.LeftShift, HidUsage.RightAlt], usages);
        Assert.Empty(HidModifierUsages.Expand(HidModifiers.None));
    }

    [Fact]
    public void TheChordAllowlistMatchesTheFirmwareNames()
    {
        // The strings in hid_keymap.c's k_chords[] name column.
        Assert.Equal(14, ChordIds.All.Count);
        foreach (string id in new[]
        {
            "ctrl_alt_del", "alt_tab", "task_manager", "show_desktop", "close_window", "screenshot",
            "layout_switch_win", "layout_switch_ctrl",
            "media_play_pause", "media_next", "media_prev", "media_mute", "media_volume_up", "media_volume_down",
        })
        {
            Assert.True(ChordIds.IsAllowlisted(id), id);
        }

        Assert.False(ChordIds.IsAllowlisted("ctrl_alt_f2"));
        Assert.False(ChordIds.IsAllowlisted(null));
        Assert.False(ChordIds.IsAllowlisted(string.Empty));
    }
}
