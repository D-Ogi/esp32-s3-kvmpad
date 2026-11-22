// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
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
    private readonly Action<bool> _onLoopGuard;
    private readonly RawInputIdentityCache _devices = new(RawInputIdentity.Resolve);
    private bool _rawInputRegistered;
    private bool _sessionRegistered;

    public SystemEventWindow(
        Action<RawMouseEvent> onMouse, Action onSessionChanged, Action onSuspend, Action<bool> onLoopGuard)
    {
        _onMouse = onMouse ?? throw new ArgumentNullException(nameof(onMouse));
        _onSessionChanged = onSessionChanged ?? throw new ArgumentNullException(nameof(onSessionChanged));
        _onSuspend = onSuspend ?? throw new ArgumentNullException(nameof(onSuspend));
        _onLoopGuard = onLoopGuard ?? throw new ArgumentNullException(nameof(onLoopGuard));

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

    /// <summary>Registers for mouse motion and local keyboard/mouse device notifications.</summary>
    public bool EnableRawInput()
    {
        if (_rawInputRegistered)
            return true;

        _devices.Clear(); // a user retry must see properties that PnP has published since the last attempt
        _rawInputRegistered = RegisterInput(NativeMethods.RIDEV_INPUTSINK | NativeMethods.RIDEV_DEVNOTIFY, Handle);
        return _rawInputRegistered;
    }

    public void DisableRawInput()
    {
        if (_rawInputRegistered) RegisterInput(NativeMethods.RIDEV_REMOVE, IntPtr.Zero);
        _rawInputRegistered = false;
    }

    private static bool RegisterInput(uint flags, nint target)
    {
        // Raw keyboard notifications identify the first collection to arrive. The low-level hook
        // still handles keyboard capture. No NOLEGACY flag is set on either registration.
        var devices = new[] {
            new NativeMethods.RAWINPUTDEVICE { usUsagePage = NativeMethods.HID_USAGE_PAGE_GENERIC, usUsage = NativeMethods.HID_USAGE_GENERIC_MOUSE, dwFlags = flags, hwndTarget = target },
            new NativeMethods.RAWINPUTDEVICE { usUsagePage = NativeMethods.HID_USAGE_PAGE_GENERIC, usUsage = NativeMethods.HID_USAGE_GENERIC_KEYBOARD, dwFlags = flags, hwndTarget = target },
        };
        return NativeMethods.RegisterRawInputDevices(devices, (uint)devices.Length, (uint)Marshal.SizeOf<NativeMethods.RAWINPUTDEVICE>());
    }

    protected override void WndProc(ref Message m)
    {
        switch (m.Msg)
        {
            case NativeMethods.WM_INPUT:
                HandleRawInput(m.LParam);
                break;

            case NativeMethods.WM_INPUT_DEVICE_CHANGE:
                _devices.Forget(m.LParam); // both removal and arrival invalidate a reused handle
                if (_rawInputRegistered && (int)m.WParam == NativeMethods.GIDC_ARRIVAL)
                    RawInputIdentityCache.AllowCapture(_devices.IsKvmpad(m.LParam), _onLoopGuard);
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
        if (!_rawInputRegistered) return;
        uint size = 0;
        uint headerSize = (uint)Marshal.SizeOf<NativeMethods.RAWINPUTHEADER>();
        if (NativeMethods.GetRawInputData(handle, NativeMethods.RID_INPUT, IntPtr.Zero, ref size, headerSize) != 0)
            return;
        if (size < headerSize || size > 1024)
            return;

        IntPtr buffer = Marshal.AllocHGlobal((int)size);
        try
        {
            uint read = NativeMethods.GetRawInputData(handle, NativeMethods.RID_INPUT, buffer, ref size, headerSize);
            if (read == uint.MaxValue || read != size || read < headerSize)
                return;

            var header = Marshal.PtrToStructure<NativeMethods.RAWINPUTHEADER>(buffer);
            if (!RawInputIdentityCache.AllowCapture(_devices.IsKvmpad(header.hDevice), _onLoopGuard)) return;
            if (header.dwType != NativeMethods.RIM_TYPEMOUSE || size < Marshal.SizeOf<NativeMethods.RAWINPUTMOUSE>())
                return;
            var input = Marshal.PtrToStructure<NativeMethods.RAWINPUTMOUSE>(buffer);

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

    public bool? IsKvmpadAttached() => RawInputIdentity.IsKvmpadAttached(_devices);

    public void Dispose()
    {
        DisableRawInput();
        if (_sessionRegistered && Handle != IntPtr.Zero)
        {
            NativeMethods.WTSUnRegisterSessionNotification(Handle);
            _sessionRegistered = false;
        }
        if (Handle != IntPtr.Zero)
            DestroyHandle();
    }
}
