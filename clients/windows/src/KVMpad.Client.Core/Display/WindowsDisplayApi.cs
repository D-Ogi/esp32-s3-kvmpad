// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace KVMpad.Client.Core.Display;

internal sealed class WindowsDisplayApi : IDisplayApi
{
    private const uint OnlyActive = 2;
    private const int InsufficientBuffer = 122;
    public DisplayPath[] ActivePaths()
    {
        if (!OperatingSystem.IsWindows()) throw new InvalidOperationException("USB display settings require a Windows desktop session.");
        for (int attempt = 0; attempt < 5; attempt++)
        {
            int rc = GetDisplayConfigBufferSizes(OnlyActive, out uint count, out uint modeCount);
            if (rc != 0) throw Error("read the active display topology", rc);
            if (count == 0) return [];
            if (count > 4096 || modeCount > 16384) throw new InvalidOperationException("Windows returned an invalid display topology.");
            var paths = new NativePath[count];
            var modes = new NativeMode[modeCount];
            rc = QueryDisplayConfig(OnlyActive, ref count, paths, ref modeCount, modes, IntPtr.Zero);
            if (rc == InsufficientBuffer) continue;
            if (rc != 0) throw Error("read the active display paths", rc);
            var result = new DisplayPath[count];
            for (int i = 0; i < result.Length; i++)
            {
                NativePath p = paths[i];
                if (p.SourceMode >= modeCount || !modes[p.SourceMode].Matches(1, p.Source, p.SourceId))
                    throw new InvalidOperationException("An active Windows desktop could not be identified.");
                var desktop = modes[p.SourceMode];
                if (desktop.Width <= 0 || desktop.Height <= 0) throw new InvalidOperationException("Windows reported an invalid desktop size.");
                DisplayResolution? signal = null;
                if (p.TargetMode < modeCount && modes[p.TargetMode].Matches(2, p.Target, p.TargetId))
                {
                    var target = modes[p.TargetMode];
                    if (target.SignalWidth > 0 && target.SignalHeight > 0) signal = new(target.SignalWidth, target.SignalHeight);
                }
                var name = new SourceName { Header = Header.For<SourceName>(1, p.Source, p.SourceId) };
                rc = DisplayConfigGetDeviceInfo(ref name);
                if (rc != 0 || string.IsNullOrWhiteSpace(name.Name)) throw Error("identify a display source", rc);
                var monitor = new TargetName { Header = Header.For<TargetName>(2, p.Target, p.TargetId) };
                string? id = DisplayConfigGetDeviceInfo(ref monitor) == 0 ? Identity(monitor.Path) : null;
                if (id == null)
                {
                    var adapter = new AdapterName { Header = Header.For<AdapterName>(4, p.Target, 0) };
                    if (DisplayConfigGetDeviceInfo(ref adapter) == 0) id = Identity(adapter.Path);
                }
                result[i] = new(p.Source.Endpoint(p.SourceId), p.Target.Endpoint(p.TargetId), name.Name, id,
                    desktop.X, desktop.Y, new(desktop.Width, desktop.Height), signal, p.Rotation, p.Scaling,
                    p.RefreshNumerator, p.RefreshDenominator, p.Technology, p.Scanline);
            }
            return result;
        }
        throw new InvalidOperationException("The Windows display topology is changing. Refresh and try again.");
    }

    private static string? Identity(string? deviceInterface)
    {
        if (string.IsNullOrEmpty(deviceInterface)) return null;
        var instance = new StringBuilder(200); // MAX_DEVICE_ID_LEN
        uint bytes = (uint)instance.Capacity * 2;
        var key = new PropertyKey { Format = new("78c34fc8-104a-4aca-9ea4-524d52996e57"), Id = 256 };
        if (CM_Get_Device_Interface_PropertyW(deviceInterface, ref key, out uint type, instance, ref bytes, 0) != 0 ||
            type != 0x12 || CM_Locate_DevNodeW(out uint node, instance.ToString(), 0) != 0) return null;
        return UsbMonitorDisplayService.BoardIdentity(Ancestors(node));
    }

    private static IEnumerable<string> Ancestors(uint node)
    {
        var id = new StringBuilder(200);
        for (int depth = 0; depth < 64; depth++)
        {
            id.Clear();
            if (CM_Get_Device_IDW(node, id, (uint)id.Capacity, 0) != 0) yield break;
            yield return id.ToString();
            if (CM_Get_Parent(out uint parent, node, 0) != 0) yield break;
            node = parent;
        }
    }

    public SourceMode Current(string name)
    {
        var dm = DevMode.Empty();
        if (!EnumDisplaySettingsExW(name, -1, ref dm, 0)) throw new InvalidOperationException("Windows could not read the current USB display mode.");
        return dm.Value;
    }

    public IEnumerable<SourceMode> Offered(string name)
    {
        for (int i = 0; i < 4096; i++)
        {
            var dm = DevMode.Empty();
            // Zero flags lists monitor-supported modes, excluding EDS_RAWMODE.
            if (!EnumDisplaySettingsExW(name, i, ref dm, 0)) yield break;
            if (dm.Width <= 0 || dm.Height <= 0) throw new InvalidOperationException("Windows returned an invalid display resolution.");
            yield return dm.Value;
        }
        throw new InvalidOperationException("Windows returned too many display modes.");
    }

    public void Change(string name, SourceMode mode, bool test, DisplayPosition? position)
    {
        var dm = DevMode.Request(mode, position);
        int rc = ChangeDisplaySettingsExW(name, ref dm, IntPtr.Zero, test ? 2u : 0u, IntPtr.Zero);
        if (rc == -1 || rc == -2) {
            string stage = test ? "validation" : "application";
            string reason = rc == -1 ? "DISP_CHANGE_FAILED" : "DISP_CHANGE_BADMODE";
            throw new InvalidOperationException($"Windows rejected {mode.Size} during {stage} ({reason}, {rc}). "
                + "Open Windows display settings on the active desktop and select the KVMpad display.");
        }
        if (rc != 0) throw Error(test ? "validate the USB display resolution" : "apply the USB display resolution", rc);
    }

    private static InvalidOperationException Error(string operation, int status) =>
        new($"Windows could not {operation} (status {status}).");

    // Plain Win32 DISPLAYCONFIG / Unicode DEVMODE layouts. LUID alignment is four
    // bytes even on x64. MODE_INFO overlays a source mode with a 48-byte signal.
    [StructLayout(LayoutKind.Sequential)]
    internal struct Luid
    {
        public uint Low;
        public int High;
        public readonly DisplayEndpoint Endpoint(uint id) => new(Low, High, id);
    }
    [StructLayout(LayoutKind.Sequential)]
    internal struct NativePath
    {
        public Luid Source;
        public uint SourceId, SourceMode, SourceStatus;
        public Luid Target;
        public uint TargetId, TargetMode, Technology, Rotation, Scaling, RefreshNumerator, RefreshDenominator;
        public uint Scanline, TargetAvailable, TargetStatus, Flags;
    }
    [StructLayout(LayoutKind.Explicit, Size = 64)]
    internal struct NativeMode
    {
        [FieldOffset(0)] public uint Type;
        [FieldOffset(4)] public uint Id;
        [FieldOffset(8)] public Luid Adapter;
        [FieldOffset(16)] public int Width;
        [FieldOffset(20)] public int Height;
        [FieldOffset(28)] public int X;
        [FieldOffset(32)] public int Y;
        [FieldOffset(40)] public int SignalWidth;
        [FieldOffset(44)] public int SignalHeight;
        public readonly bool Matches(uint type, Luid adapter, uint id) =>
            Type == type && Id == id && Adapter.High == adapter.High && Adapter.Low == adapter.Low;
    }
    [StructLayout(LayoutKind.Sequential)]
    internal struct Header
    {
        public uint Type, Size;
        public Luid Adapter;
        public uint Id;
        internal static Header For<T>(uint type, Luid adapter, uint id) =>
            new() { Type = type, Size = (uint)Marshal.SizeOf<T>(), Adapter = adapter, Id = id };
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    internal struct SourceName
    {
        public Header Header;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string Name;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    internal struct TargetName
    {
        public Header Header;
        public uint Flags, Technology;
        public ushort Manufacturer, Product;
        public uint Connector;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string Friendly;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Path;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    internal struct AdapterName
    {
        public Header Header;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Path;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct PropertyKey { public Guid Format; public uint Id; }
    [StructLayout(LayoutKind.Explicit, Size = 220)]
    internal struct DevMode
    {
        [FieldOffset(68)] public ushort Size;
        [FieldOffset(72)] public uint Fields;
        [FieldOffset(76)] public int X;
        [FieldOffset(80)] public int Y;
        [FieldOffset(84)] public int Orientation;
        [FieldOffset(168)] public int Bits;
        [FieldOffset(172)] public int Width;
        [FieldOffset(176)] public int Height;
        [FieldOffset(180)] public int Flags;
        [FieldOffset(184)] public int Hertz;
        internal static DevMode Empty() => new() { Size = (ushort)Marshal.SizeOf<DevMode>() };
        internal readonly SourceMode Value => new(new(Width, Height), Hertz, Orientation, Bits, Flags);
        internal static DevMode Request(SourceMode mode, DisplayPosition? position = null)
        {
            var dm = Empty();
            // DM_PELSWIDTH/HEIGHT, BITSPERPEL, DISPLAYFREQUENCY, DISPLAYFLAGS and
            // DISPLAYORIENTATION. Request a position only to retain a shared edge.
            dm.Fields = 0x80000 | 0x100000 | 0x40000 | 0x400000 | 0x200000 | 0x80;
            dm.Width = mode.Size.Width; dm.Height = mode.Size.Height;
            dm.Hertz = mode.Hertz; dm.Orientation = mode.Orientation;
            dm.Bits = mode.Bits; dm.Flags = mode.Flags;
            if (position is { } origin)
            {
                dm.Fields |= 0x20; // DM_POSITION
                dm.X = origin.X; dm.Y = origin.Y;
            }
            return dm;
        }
    }
    [DllImport("user32.dll")] private static extern int GetDisplayConfigBufferSizes(uint flags, out uint paths, out uint modes);
    [DllImport("user32.dll")] private static extern int QueryDisplayConfig(uint flags, ref uint count, [Out] NativePath[] paths,
        ref uint modeCount, [Out] NativeMode[] modes, IntPtr topology);
    [DllImport("user32.dll")] private static extern int DisplayConfigGetDeviceInfo(ref SourceName request);
    [DllImport("user32.dll")] private static extern int DisplayConfigGetDeviceInfo(ref TargetName request);
    [DllImport("user32.dll")] private static extern int DisplayConfigGetDeviceInfo(ref AdapterName request);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)] private static extern bool EnumDisplaySettingsExW(
        string device, int number, ref DevMode mode, uint flags);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)] private static extern int ChangeDisplaySettingsExW(
        string device, ref DevMode mode, IntPtr window, uint flags, IntPtr param);
    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)] private static extern int CM_Get_Device_Interface_PropertyW(
        string device, ref PropertyKey key, out uint type, [Out] StringBuilder buffer, ref uint size, uint flags);
    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)] private static extern int CM_Locate_DevNodeW(
        out uint node, string instance, uint flags);
    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)] private static extern int CM_Get_Device_IDW(
        uint node, [Out] StringBuilder id, uint length, uint flags);
    [DllImport("cfgmgr32.dll")] private static extern int CM_Get_Parent(out uint parent, uint node, uint flags);
}
