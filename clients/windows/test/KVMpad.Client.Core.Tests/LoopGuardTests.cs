// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using KVMpad.Client.Core.Input;
using Xunit;

namespace KVMpad.Client.Core.Tests;

/// <summary>
/// The feedback-loop guard recognizes a KVMpad by the USB identity in a Windows device interface path
/// (VID 0x303A, prototype PID 0x400C; <c>components/usb_hid_device/src/usb_hid_device.c</c>).
/// </summary>
public sealed class LoopGuardTests
{
    [Theory]
    [InlineData(@"\\?\HID#VID_303A&PID_400C&MI_00#7&2a1b3c4d&0&0000#{884b96c3-56ef-11d1-bc8c-00a0c91405dd}")] // keyboard
    [InlineData(@"\\?\HID#VID_303A&PID_400C&MI_01#7&1b2c3d4e&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}")] // mouse
    [InlineData(@"\\?\hid#vid_303a&pid_400c&mi_02#7&3c4d5e6f&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}")] // lower case
    public void RecognizesKvmpadInterfaces(string path) => Assert.True(KvmpadUsbIdentity.IsKvmpadDevicePath(path));

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData(@"\\?\HID#VID_046D&PID_C52B&MI_01&Col01#8&2f6b4f8&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}")]
    [InlineData(@"\\?\HID#VID_303A&PID_1001&MI_00#7&1&0&0000#{884b96c3-56ef-11d1-bc8c-00a0c91405dd}")] // another ESP32-S3 device
    [InlineData(@"\\?\Root#RDP_MOU#0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}")]
    public void IgnoresOtherDevices(string? path) => Assert.False(KvmpadUsbIdentity.IsKvmpadDevicePath(path));

    [Fact]
    public void IdentityMatchesTheFirmware()
    {
        Assert.Equal(0x303A, KvmpadUsbIdentity.VendorId);
        Assert.Equal(0x400C, KvmpadUsbIdentity.ProductId);
    }

    [Fact]
    public void LoopGuardExitStillSendsTheUps()
    {
        // The lease is still held when the guard stops capture, so every held key gets its UP.
        Assert.True(ReleaseAllResult.RequiresTransmit(ReleaseReason.LoopGuard));
    }
}
