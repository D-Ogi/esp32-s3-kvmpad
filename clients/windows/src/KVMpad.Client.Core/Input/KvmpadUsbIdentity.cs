// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;

namespace KVMpad.Client.Core.Input;

/// <summary>
/// Recognizes a KVMpad by the USB identity of its HID interfaces (VID <c>0x303A</c>, prototype PID <c>0x400C</c>,
/// TECH-037) in a Windows device interface path such as
/// <c>\\?\HID#VID_303A&amp;PID_400C&amp;MI_01#7&amp;1b2c3d4e&amp;0&amp;0000#{378de44c-…}</c>.
/// </summary>
/// <remarks>
/// It exists for the feedback-loop guard: when the KVMpad is plugged into the CLIENT machine itself, every key and
/// every mouse movement the client forwards comes back from the KVMpad as new local input, which the client would
/// forward again. The client therefore never forwards input that a KVMpad produced and does not capture while a
/// KVMpad is attached to the CLIENT machine.
/// </remarks>
public static class KvmpadUsbIdentity
{
    public const ushort VendorId = 0x303A;
    public const ushort ProductId = 0x400C;

    private const string Marker = "VID_303A&PID_400C";

    /// <summary>True when <paramref name="devicePath"/> names an interface of a KVMpad.</summary>
    public static bool IsKvmpadDevicePath(string? devicePath) =>
        !string.IsNullOrEmpty(devicePath) && devicePath.Contains(Marker, StringComparison.OrdinalIgnoreCase);
}
