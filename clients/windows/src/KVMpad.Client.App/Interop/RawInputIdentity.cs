// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;
using KVMpad.Client.Core.Input;

namespace KVMpad.Client.App.Interop;

internal static partial class RawInputIdentity
{
    [StructLayout(LayoutKind.Sequential)]
    private readonly record struct PropertyKey(Guid Format, uint Id);
    private static readonly PropertyKey InstanceId = new(new Guid("78c34fc8-104a-4aca-9ea4-524d52996e57"), 256);
    private static readonly PropertyKey BusDescription = new(new Guid("540b947e-8b40-45bc-a8a2-6a0b894cbda2"), 4);
    private static readonly Guid HidClass = new("4d1e55b2-f16f-11cf-88cb-001111000030");

    [LibraryImport("kernel32", EntryPoint = "CreateFileW", StringMarshalling = StringMarshalling.Utf16)]
    private static partial SafeFileHandle Open(string path, uint access, uint share, nint security, uint disposition, uint flags, nint template);
    [LibraryImport("hid", EntryPoint = "HidD_GetManufacturerString")]
    private static partial byte ReadManufacturer(SafeFileHandle handle, [Out] byte[] buffer, uint size);
    [LibraryImport("hid", EntryPoint = "HidD_GetProductString")]
    private static partial byte ReadProduct(SafeFileHandle handle, [Out] byte[] buffer, uint size);
    [LibraryImport("cfgmgr32", EntryPoint = "CM_Get_Device_Interface_PropertyW", StringMarshalling = StringMarshalling.Utf16)]
    private static partial uint InterfaceProperty(string path, in PropertyKey key, out uint type, [Out] byte[] buffer, ref uint bytes, uint flags);
    [LibraryImport("cfgmgr32", EntryPoint = "CM_Get_DevNode_PropertyW")]
    private static partial uint NodeProperty(uint node, in PropertyKey key, out uint type, [Out] byte[] buffer, ref uint bytes, uint flags);
    [LibraryImport("cfgmgr32", EntryPoint = "CM_Locate_DevNodeW", StringMarshalling = StringMarshalling.Utf16)]
    private static partial uint FindNode(out uint node, string instance, uint flags);
    [LibraryImport("cfgmgr32", EntryPoint = "CM_Get_Parent")]
    private static partial uint Parent(out uint parent, uint node, uint flags);
    [LibraryImport("cfgmgr32", EntryPoint = "CM_Get_Device_Interface_ListW", StringMarshalling = StringMarshalling.Utf16)]
    private static partial uint Interfaces(in Guid interfaceClass, string instance, [Out] char[] list, uint chars, uint flags);

    private static string? Text(byte[] buffer, uint bytes)
    {
        if (bytes > buffer.Length) return null;
        string raw = Encoding.Unicode.GetString(buffer, 0, (int)bytes);
        int end = raw.IndexOf('\0');
        string text = end >= 0 ? raw[..end] : raw;
        return text.Length > 0 ? text : null;
    }

    private static void ReadHid(string path, byte[] buffer, ref string? manufacturer, ref string? collection)
    {
        // Zero access is sufficient for descriptor queries on system-owned keyboards and mice.
        using var handle = Open(path, 0, 3, 0, 3, 0, 0);
        if (handle.IsInvalid) return;
        uint size = (uint)buffer.Length;
        if (manufacturer == null && ReadManufacturer(handle, buffer, size) != 0) manufacturer = Text(buffer, size);
        if (collection == null && ReadProduct(handle, buffer, size) != 0) collection = Text(buffer, size);
    }

    private static string? NodeString(uint node, in PropertyKey key, byte[] buffer)
    {
        uint bytes = (uint)buffer.Length;
        return NodeProperty(node, key, out var type, buffer, ref bytes, 0) == 0 && type == 18
            ? Text(buffer, bytes) : null;
    }

    internal static HidDeviceNames ReadNames(string path)
    {
        if (path.StartsWith(@"\??\", StringComparison.Ordinal)) path = @"\\?\" + path[4..];
        byte[] buffer = new byte[1024];
        string? manufacturer = null, collection = null, product = null;
        ReadHid(path, buffer, ref manufacturer, ref collection);

        uint bytes = (uint)buffer.Length;
        if (InterfaceProperty(path, InstanceId, out var type, buffer, ref bytes, 0) == 0 && type == 18 &&
            Text(buffer, bytes) is string instance) {
            // The raw keyboard/mouse class path may not implement HID IOCTLs. Open the HID interface of this devnode.
            if (manufacturer == null || collection == null) {
                char[] paths = new char[1024];
                if (Interfaces(HidClass, instance, paths, (uint)paths.Length, 0) == 0) {
                    int end = Array.IndexOf(paths, '\0');
                    if (end > 0) ReadHid(new string(paths, 0, end), buffer, ref manufacturer, ref collection);
                }
            }
            // USBCCGP may substitute iInterface for iProduct. The composite parent retains iProduct,
            // including KVMpad GUD on firmware that supplies no HID interface strings.
            string usb = @"USB\" + path.Split('#')[1][..17];
            if (FindNode(out uint node, instance, 0) == 0) {
                for (int i = 0; i < 4 && Parent(out uint parent, node, 0) == 0; i++) {
                    node = parent;
                    string? id = NodeString(node, InstanceId, buffer);
                    if (id == null) break;
                    if (id.StartsWith(usb + @"\", StringComparison.OrdinalIgnoreCase)) {
                        product = NodeString(node, BusDescription, buffer);
                        break;
                    }
                    if (id.StartsWith(usb + "&MI_", StringComparison.OrdinalIgnoreCase))
                        collection ??= NodeString(node, BusDescription, buffer);
                    else if (id.StartsWith(@"USB\", StringComparison.OrdinalIgnoreCase)) break;
                }
            }
        }
        return new(manufacturer, collection, product);
    }

    internal static bool? Resolve(nint device)
    {
        if (device == 0) return false;
        uint chars = 0;
        if (NativeMethods.GetRawInputDeviceInfo(device, NativeMethods.RIDI_DEVICENAME, 0, ref chars) != 0 ||
            chars == 0 || chars > 1024) return null;
        nint buffer = Marshal.AllocHGlobal((int)chars * 2);
        try {
            uint got = NativeMethods.GetRawInputDeviceInfo(device, NativeMethods.RIDI_DEVICENAME, buffer, ref chars);
            return got == uint.MaxValue || got == 0 ? null : KvmpadUsbIdentity.Identify(Marshal.PtrToStringUni(buffer), ReadNames);
        } finally { Marshal.FreeHGlobal(buffer); }
    }

    internal static bool? IsKvmpadAttached(RawInputIdentityCache cache)
    {
        var devices = Devices();
        return devices == null ? null : cache.IsAttached(devices
            .Where(d => d.dwType is NativeMethods.RIM_TYPEMOUSE or NativeMethods.RIM_TYPEKEYBOARD)
            .Select(d => d.hDevice));
    }

    internal static NativeMethods.RAWINPUTDEVICELIST[]? Devices()
    {
        uint size = (uint)Marshal.SizeOf<NativeMethods.RAWINPUTDEVICELIST>();
        uint count = 0;
        if (NativeMethods.GetRawInputDeviceList(null, ref count, size) != 0) return null;
        for (int i = 0; i < 3; i++) {
            var list = new NativeMethods.RAWINPUTDEVICELIST[count + 4];
            count = (uint)list.Length;
            uint got = NativeMethods.GetRawInputDeviceList(list, ref count, size);
            if (got != uint.MaxValue) {
                Array.Resize(ref list, (int)got);
                return list;
            }
            if (Marshal.GetLastPInvokeError() != 122) break; // ERROR_INSUFFICIENT_BUFFER on hotplug
        }
        return null;
    }
}
