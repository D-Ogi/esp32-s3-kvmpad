// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using KVMpad.Client.Core.Input;
using System.Windows.Forms;

namespace KVMpad.Client.App.Interop;

/// <summary>Relative mouse motion and button or wheel edges, as Raw Input reports them.</summary>
internal readonly struct RawMouseEvent
{
    public RawMouseEvent(int dx, int dy, ushort buttonFlags, short wheelDelta)
    {
        Dx = dx;
        Dy = dy;
        ButtonFlags = buttonFlags;
        WheelDelta = wheelDelta;
    }

    public int Dx { get; }
    public int Dy { get; }
    public ushort ButtonFlags { get; }
    public short WheelDelta { get; }
}

/// <summary>
/// A message-only window that receives Raw Input, session-change and power notifications.
/// </summary>
/// <remarks>
/// <para>
/// Raw Input is how the client gets mouse motion the way the HOST needs it: the raw counts the device
/// reported, with no pointer acceleration, no clamping to the CLIENT machine's screen and no dependence on
/// where the cursor happens to be. <c>WM_MOUSEMOVE</c> would give none of that.
/// </para>
/// <para>
/// The same window subscribes to <c>WM_WTSSESSION_CHANGE</c> and <c>WM_POWERBROADCAST</c> because a lock
/// screen or a suspend is a loss of the user's attention, and anything held down must be released then — the
/// client cannot be the reason a key stays down on the HOST while the CLIENT machine is locked (ADR-021).
/// </para>
/// <para>
/// Feedback-loop guard: Raw Input from a KVMpad's own mouse interface is dropped (when the KVMpad is plugged into
/// this machine, that motion is the client's own output coming back), and the arrival of a KVMpad while capturing is
/// reported so the caller can stop (<see cref="KvmpadUsbIdentity"/>).
/// </para>
/// </remarks>
internal sealed class SystemEventWindow : NativeWindow, IDisposable
{
    private readonly Action<RawMouseEvent> _onMouse;
    private readonly Action _onSessionChanged;
    private readonly Action _onSuspend;
    private readonly Action _onKvmpadArrived;
    private readonly Dictionary<IntPtr, bool> _isKvmpad = new();
    private bool _rawInputRegistered;
    private bool _sessionRegistered;

    public SystemEventWindow(
        Action<RawMouseEvent> onMouse, Action onSessionChanged, Action onSuspend, Action onKvmpadArrived)
    {
        _onMouse = onMouse ?? throw new ArgumentNullException(nameof(onMouse));
        _onSessionChanged = onSessionChanged ?? throw new ArgumentNullException(nameof(onSessionChanged));
        _onSuspend = onSuspend ?? throw new ArgumentNullException(nameof(onSuspend));
        _onKvmpadArrived = onKvmpadArrived ?? throw new ArgumentNullException(nameof(onKvmpadArrived));

        CreateHandle(new CreateParams
        {
            Caption = "KVMpad client event sink",
            X = 0,
            Y = 0,
            Height = 0,
            Width = 0,
            Style = 0,
        });

        _sessionRegistered = NativeMethods.WTSRegisterSessionNotification(Handle, NativeMethods.NOTIFY_FOR_THIS_SESSION);
    }

    /// <summary>Starts delivering Raw Input mouse events. Called when capture begins.</summary>
    public bool EnableRawMouse()
    {
        if (_rawInputRegistered)
            return true;

        var devices = new[]
        {
            new NativeMethods.RAWINPUTDEVICE
            {
                usUsagePage = NativeMethods.HID_USAGE_PAGE_GENERIC,
                usUsage = NativeMethods.HID_USAGE_GENERIC_MOUSE,
                // INPUTSINK: deliver even when this window is not in the foreground, which is the whole point
                // — the user's focus is wherever they left it, not on the client. DEVNOTIFY: report mice that
                // arrive meanwhile (WM_INPUT_DEVICE_CHANGE), so a KVMpad plugged in during capture stops it.
                dwFlags = NativeMethods.RIDEV_INPUTSINK | NativeMethods.RIDEV_DEVNOTIFY,
                hwndTarget = Handle,
            },
        };

        _rawInputRegistered = NativeMethods.RegisterRawInputDevices(
            devices, (uint)devices.Length, (uint)Marshal.SizeOf<NativeMethods.RAWINPUTDEVICE>());
        return _rawInputRegistered;
    }

    /// <summary>Stops delivering Raw Input mouse events. Called when capture ends.</summary>
    public void DisableRawMouse()
    {
        if (!_rawInputRegistered)
            return;

        var devices = new[]
        {
            new NativeMethods.RAWINPUTDEVICE
            {
                usUsagePage = NativeMethods.HID_USAGE_PAGE_GENERIC,
                usUsage = NativeMethods.HID_USAGE_GENERIC_MOUSE,
                dwFlags = NativeMethods.RIDEV_REMOVE,
                hwndTarget = IntPtr.Zero,
            },
        };

        NativeMethods.RegisterRawInputDevices(
            devices, (uint)devices.Length, (uint)Marshal.SizeOf<NativeMethods.RAWINPUTDEVICE>());
        _rawInputRegistered = false;
    }

    protected override void WndProc(ref Message m)
    {
        switch (m.Msg)
        {
            case NativeMethods.WM_INPUT:
                HandleRawInput(m.LParam);
                break;

            case NativeMethods.WM_INPUT_DEVICE_CHANGE:
                if ((int)m.WParam == NativeMethods.GIDC_ARRIVAL && IsKvmpad(m.LParam))
                    _onKvmpadArrived();
                break;

            case NativeMethods.WM_WTSSESSION_CHANGE:
                int reason = (int)m.WParam;
                if (reason is NativeMethods.WTS_SESSION_LOCK
                    or NativeMethods.WTS_SESSION_LOGOFF
                    or NativeMethods.WTS_CONSOLE_DISCONNECT
                    or NativeMethods.WTS_REMOTE_DISCONNECT)
                    _onSessionChanged();
                break;

            case NativeMethods.WM_POWERBROADCAST:
                if ((int)m.WParam == NativeMethods.PBT_APMSUSPEND)
                    _onSuspend();
                break;
        }

        base.WndProc(ref m);
    }

    private void HandleRawInput(IntPtr handle)
    {
        uint size = 0;
        uint headerSize = (uint)Marshal.SizeOf<NativeMethods.RAWINPUTHEADER>();
        if (NativeMethods.GetRawInputData(handle, NativeMethods.RID_INPUT, IntPtr.Zero, ref size, headerSize) != 0)
            return;
        if (size == 0 || size > 1024)
            return;

        IntPtr buffer = Marshal.AllocHGlobal((int)size);
        try
        {
            uint read = NativeMethods.GetRawInputData(handle, NativeMethods.RID_INPUT, buffer, ref size, headerSize);
            if (read != size)
                return;

            var input = Marshal.PtrToStructure<NativeMethods.RAWINPUTMOUSE>(buffer);
            if (input.header.dwType != NativeMethods.RIM_TYPEMOUSE)
                return;

            // The KVMpad's own mouse: forwarding it would loop (KvmpadUsbIdentity).
            if (IsKvmpad(input.header.hDevice))
                return;

            // An absolute-coordinate device (a tablet or a remote-desktop pointer) reports positions, not
            // deltas, so its motion is skipped rather than reinterpreted as a delta of thousands of counts.
            bool absolute = (input.mouse.usFlags & NativeMethods.MOUSE_MOVE_ABSOLUTE) != 0;
            int dx = absolute ? 0 : input.mouse.lLastX;
            int dy = absolute ? 0 : input.mouse.lLastY;

            short wheel = 0;
            if ((input.mouse.usButtonFlags & (NativeMethods.RI_MOUSE_WHEEL | NativeMethods.RI_MOUSE_HWHEEL)) != 0)
                wheel = unchecked((short)input.mouse.usButtonData);

            _onMouse(new RawMouseEvent(dx, dy, input.mouse.usButtonFlags, wheel));
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    private bool IsKvmpad(IntPtr device)
    {
        if (device == IntPtr.Zero)
            return false; // injected input has no device
        if (!_isKvmpad.TryGetValue(device, out bool known))
        {
            known = KvmpadUsbIdentity.IsKvmpadDevicePath(DeviceName(device));
            _isKvmpad[device] = known;
        }
        return known;
    }

    /// <summary>True when a KVMpad (any of its HID interfaces) is attached to this machine.</summary>
    public static bool IsKvmpadAttached()
    {
        uint count = 0;
        uint itemSize = (uint)Marshal.SizeOf<NativeMethods.RAWINPUTDEVICELIST>();
        if (NativeMethods.GetRawInputDeviceList(null, ref count, itemSize) != 0 || count == 0)
            return false;
        var list = new NativeMethods.RAWINPUTDEVICELIST[count + 4]; // room for a device that arrives meanwhile
        count = (uint)list.Length;
        uint got = NativeMethods.GetRawInputDeviceList(list, ref count, itemSize);
        if (got == uint.MaxValue)
            return false;
        for (int i = 0; i < (int)got; i++)
        {
            if (KvmpadUsbIdentity.IsKvmpadDevicePath(DeviceName(list[i].hDevice)))
                return true;
        }
        return false;
    }

    private static string? DeviceName(IntPtr device)
    {
        uint chars = 0;
        if (NativeMethods.GetRawInputDeviceInfo(device, NativeMethods.RIDI_DEVICENAME, IntPtr.Zero, ref chars) != 0
            || chars == 0 || chars > 1024)
            return null;
        IntPtr buffer = Marshal.AllocHGlobal((int)chars * 2);
        try
        {
            uint got = NativeMethods.GetRawInputDeviceInfo(device, NativeMethods.RIDI_DEVICENAME, buffer, ref chars);
            return got == uint.MaxValue || got == 0 ? null : Marshal.PtrToStringUni(buffer);
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    public void Dispose()
    {
        DisableRawMouse();
        if (_sessionRegistered && Handle != IntPtr.Zero)
        {
            NativeMethods.WTSUnRegisterSessionNotification(Handle);
            _sessionRegistered = false;
        }
        if (Handle != IntPtr.Zero)
            DestroyHandle();
    }
}
