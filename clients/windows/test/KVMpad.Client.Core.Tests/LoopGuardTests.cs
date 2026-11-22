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

    [Theory]
    [InlineData("303A", "2986", "01", "D-Ogi", "KVMpad Keyboard", null, true)]
    [InlineData("303A", "2986", "02", "D-Ogi", "KVMpad Mouse", null, true)]
    [InlineData("303A", "2986", "02", "D-Ogi", "esp32s3udisp0_R640x480_Ejpg4_Fps10_Bl96", null, true)]
    [InlineData("303A", "2986", "01", "D-Ogi", null, "esp32s3udisp0_R1024x600_Ejpg4_Fps10_Bl96", true)]
    [InlineData("303A", "2986", "02", "D-Ogi", null, "esp32s3udisp0_Ejpg4_Fps10_Bl96", true)]
    [InlineData("303A", "400A", "01", null, "KVMpad Keyboard", null, true)]
    [InlineData("303A", "400A", "02", "D-Ogi", "USB Input Device", "KVMpad Screenshot", true)]
    [InlineData("FFFE", "F00D", "01", "D-Ogi", "KVMpad GUD", null, true)]
    [InlineData("1D50", "614D", "02", null, null, "KVMpad GUD", true)]
    [InlineData("303A", "2986", "02", null, "Generic Mouse", null, false)]
    [InlineData("303A", "2986", "02", "Other", null, "esp32s3udisp0_Ejpg4_Fps10_Bl96", false)]
    [InlineData("303A", "2986", "02", null, null, "esp32s3udisp0_Ejpg4_Fps10_Bl96", null)]
    [InlineData("303A", "2986", "01", "D-Ogi", "KVMpad Mouse", null, false)]
    [InlineData("303A", "400A", "02", null, "Vendor Mouse", null, false)]
    [InlineData("303A", "400A", "01", null, null, null, null)]
    [InlineData("303A", "400C", "01", "Other", "Generic Mouse", null, false)]
    [InlineData("303A", "400C", "00", null, "Office Keyboard", null, false)]
    [InlineData("303A", "400C", "00", null, null, null, true)]
    [InlineData("303A", "2987", "01", "D-Ogi", null, "KVMpad GUD", false)]
    [InlineData("046D", "C52B", "02", "Other", null, "KVMpad GUD", false)]
    [InlineData("FFFE", "F00D", "00", "D-Ogi", "KVMpad GUD", null, false)]
    public void ClassifiesUsbCollection(string vid, string pid, string mi, string? maker, string? collection, string? product, bool? match)
    {
        string path = $@"\\?\HID#VID_{vid}&PID_{pid}&MI_{mi}&Col01#7&test&0&0000#{{4d1e55b2-f16f-11cf-88cb-001111000030}}";
        Assert.Equal(match, KvmpadUsbIdentity.Identify(path, _ => new(maker, collection, product)));
    }

    [Theory]
    [InlineData(@"\\?\USB#VID_303A&PID_2986&MI_01#0#{884b96c3-56ef-11d1-bc8c-00a0c91405dd}")]
    [InlineData(@"\\?\HID#VID_0001&PID_0002&MI_00#VID_303A&PID_400C#{884b96c3-56ef-11d1-bc8c-00a0c91405dd}")]
    [InlineData(@"\\?\HID#VID_303A&PID_400C0&MI_00#0#{884b96c3-56ef-11d1-bc8c-00a0c91405dd}")]
    [InlineData(@"\\?\HID#VID_046D&PID_C52B#ordinary-mouse#{378de44c-56ef-11d1-bc8c-00a0c91405dd}")]
    [InlineData(@"\\?\Root#RDP_KBD#0000#{884b96c3-56ef-11d1-bc8c-00a0c91405dd}")]
    public void UnrelatedPathsDoNotQueryDescriptors(string path)
    {
        Assert.False(KvmpadUsbIdentity.Identify(path, _ => throw new System.InvalidOperationException()));
    }

    [Fact]
    public void ReusedHandleAndNewCaptureRequireFreshIdentity()
    {
        int reads = 0;
        bool attached = false;
        var cache = new RawInputIdentityCache(_ => { reads++; return attached; });
        for (int i = 0; i < 1000; i++) Assert.False(cache.IsKvmpad(12));
        Assert.Equal(1, reads);
        cache.Forget(12);
        attached = true;
        Assert.True(cache.IsKvmpad(12));
        Assert.Equal(2, reads);
        cache.Clear();
        attached = false;
        Assert.False(cache.IsKvmpad(12));
        Assert.Equal(3, reads);
    }

    [Fact]
    public void ArrivalWithPendingPropertiesRetriesWithoutPollingEveryPacket()
    {
        int queries = 0;
        var cache = new RawInputIdentityCache(_ => ++queries > 1 ? true : null);
        Assert.Null(cache.IsKvmpad(55, 2000));
        for (int ms = 2001; ms < 3000; ms++) Assert.Null(cache.IsKvmpad(55, ms));
        Assert.True(cache.IsKvmpad(55, 3000));
        Assert.True(cache.IsKvmpad(55, 5000));
        Assert.Equal(2, queries);
    }

    [Fact]
    public void PreflightRetainsPendingInputUntilAllKeyboardsAndMiceAreIdentified()
    {
        bool? candidate = null;
        var cache = new RawInputIdentityCache(handle => handle == 2 ? candidate : handle == 3);
        Assert.False(cache.IsAttached([]));
        Assert.False(cache.IsAttached([1]));
        Assert.Null(cache.IsAttached([1, 2]));
        Assert.Null(cache.IsAttached([2, 1]));
        Assert.True(cache.IsAttached([2, 3]));

        candidate = false;
        cache.Clear(); // a new capture attempt immediately rechecks even within the retry interval
        Assert.False(cache.IsAttached([1, 2]));
    }

    [Theory]
    [InlineData(null, false)]
    [InlineData(true, false)]
    [InlineData(false, true)]
    public void CaptureStopsForPendingOrConfirmedDevicesAtArrivalAndBeforeForwarding(bool? match, bool allow)
    {
        bool? confirmed = null;
        Assert.Equal(allow, RawInputIdentityCache.AllowCapture(match, result => confirmed = result));
        Assert.Equal(allow ? null : match == true, confirmed);
    }

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
