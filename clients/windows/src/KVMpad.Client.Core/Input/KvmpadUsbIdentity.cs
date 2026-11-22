// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Globalization;
using System.Text.RegularExpressions;

namespace KVMpad.Client.Core.Input;

public readonly record struct HidDeviceNames(string? Manufacturer, string? Collection, string? Product);

/// <summary>Identifies the USB HID collections whose input would loop back to the local client.</summary>
public static partial class KvmpadUsbIdentity
{
    public const ushort VendorId = 0x303A;
    public const ushort ProductId = 0x400C;

    // Raw Input may return the mouse, keyboard or HID class interface of the same HID devnode.
    [GeneratedRegex(@"\A(?:\\\\\?\\|\\\?\?\\)HID#VID_([0-9A-F]{4})&PID_([0-9A-F]{4})(?:&REV_[0-9A-F]{4})?&MI_(0[0-3])(?:&COL[0-9A-F]{2})?#[^#\\]+#\{(?:884b96c3-56ef-11d1-bc8c-00a0c91405dd|378de44c-56ef-11d1-bc8c-00a0c91405dd|4d1e55b2-f16f-11cf-88cb-001111000030)\}(?:\\(?:kbd|mou))?\z",
        RegexOptions.IgnoreCase | RegexOptions.CultureInvariant)]
    private static partial Regex CollectionPath();

    /// <summary>Recognizes the standard firmware's original USB identity without a descriptor reader.</summary>
    public static bool IsKvmpadDevicePath(string? devicePath) => Identify(devicePath, _ => default) == true;

    /// <summary>Null means Windows has yet to supply descriptors for a possible composite HID.</summary>
    public static bool? Identify(string? devicePath, Func<string, HidDeviceNames> readNames)
    {
        if (string.IsNullOrEmpty(devicePath)) return null;
        Match m = CollectionPath().Match(devicePath);
        if (!m.Success) return false;
        int vid = int.Parse(m.Groups[1].Value, NumberStyles.HexNumber, CultureInfo.InvariantCulture);
        int pid = int.Parse(m.Groups[2].Value, NumberStyles.HexNumber, CultureInfo.InvariantCulture);
        int mi = m.Groups[3].Value[1] - '0';
        bool standard = vid == VendorId && pid == ProductId;
        if (!standard && (mi is not (1 or 2) || vid is 0 or 0xffff || pid == 0 || (vid == VendorId && pid == 0x2987)))
            return false;

        HidDeviceNames names = readNames(devicePath);
        if (names.Manufacturer is not (null or "D-Ogi")) return false;
        if (standard) {
            if (names.Product == "KVMpad" || names.Collection == "KVMpad") return true;
            if (names.Collection == null && names.Product == null) return true; // original boot HID contract
            return mi switch {
                0 => names.Collection == "KVMpad Keyboard",
                1 => names.Collection == "KVMpad Mouse",
                2 => names.Collection is "KVMpad Precision Touchpad" or "KVMpad Consumer Control",
                3 => names.Collection == "KVMpad Radial and Control Deck",
                _ => false,
            };
        }

        bool HasProduct(string value) => names.Product == value || names.Collection == value;
        if (vid == VendorId && (pid == 0x2986 || pid == 0x400a)) {
            if (names.Collection == (mi == 1 ? "KVMpad Keyboard" : "KVMpad Mouse")) return true;
            if (pid == 0x400a && HasProduct("KVMpad Screenshot")) return true;
            // Other display adapters share this VID/PID and the display driver's capability strings.
            if (pid == 0x2986 && (HasProduct("esp32s3udisp0_R640x480_Ejpg4_Fps10_Bl96") ||
                HasProduct("esp32s3udisp0_R1024x600_Ejpg4_Fps10_Bl96") || HasProduct("esp32s3udisp0_Ejpg4_Fps10_Bl96")))
                return names.Manufacturer == null ? null : true;
        } else if (HasProduct("KVMpad GUD")) return true; // GUD's VID/PID are set by the build
        return names.Product == null && names.Collection == null ? null : false;
    }
}
