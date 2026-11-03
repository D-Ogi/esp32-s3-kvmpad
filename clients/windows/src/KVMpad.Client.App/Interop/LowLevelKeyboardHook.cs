// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using KVMpad.Client.Core.Input;

namespace KVMpad.Client.App.Interop;

/// <summary>One keyboard event as the low-level hook reports it.</summary>
internal readonly struct HookedKey
{
    public HookedKey(int virtualKey, int scanCode, bool extended, bool down, bool injected)
    {
        VirtualKey = virtualKey;
        ScanCode = scanCode;
        Extended = extended;
        Down = down;
        Injected = injected;
    }

    public int VirtualKey { get; }
    public int ScanCode { get; }
    public bool Extended { get; }
    public bool Down { get; }
    public bool Injected { get; }
}

/// <summary>
/// A <c>WH_KEYBOARD_LL</c> hook, installed only while capture is on.
/// </summary>
/// <remarks>
/// <para>
/// The hook exists so that keystrokes can be redirected to the HOST instead of acting on the CLIENT machine,
/// which needs a system-wide hook: there is no other way to see a key the user pressed while another
/// application had focus, and no other way to stop that application receiving it. ADR-021 constrains it
/// accordingly — the hook is installed on entering capture and removed on leaving, never left running in the
/// background.
/// </para>
/// <para>
/// The callback must return quickly or Windows removes the hook, so it does nothing but classify the event
/// and hand it to a delegate that queues work. Nothing is written to a log or a file; the event's key identity
/// leaves this method only as a HID usage in a protocol message (TECH-046).
/// </para>
/// <para>
/// Injected events (<c>LLKHF_INJECTED</c>) are reported so the caller can decide. They are forwarded like any
/// other key: an on-screen keyboard or an accessibility tool is a legitimate way for the user to type.
/// </para>
/// </remarks>
internal sealed class LowLevelKeyboardHook : IDisposable
{
    private readonly Func<HookedKey, CaptureDecision> _handler;
    private readonly NativeMethods.HookProc _proc;
    private readonly IntPtr _procPointer;
    private IntPtr _hook;

    public LowLevelKeyboardHook(Func<HookedKey, CaptureDecision> handler)
    {
        _handler = handler ?? throw new ArgumentNullException(nameof(handler));
        // The delegate is kept in a field for the hook's whole life; letting it be collected would crash the
        // process the next time a key is pressed.
        _proc = HookCallback;
        _procPointer = Marshal.GetFunctionPointerForDelegate(_proc);
    }

    public bool IsInstalled => _hook != IntPtr.Zero;

    public void Install()
    {
        if (IsInstalled)
            return;
        IntPtr module = NativeMethods.GetModuleHandle(null);
        _hook = NativeMethods.SetWindowsHookEx(NativeMethods.WH_KEYBOARD_LL, _procPointer, module, 0);
        if (_hook == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not install the keyboard hook.");
    }

    public void Remove()
    {
        if (!IsInstalled)
            return;
        NativeMethods.UnhookWindowsHookEx(_hook);
        _hook = IntPtr.Zero;
    }

    private IntPtr HookCallback(int code, IntPtr wParam, IntPtr lParam)
    {
        if (code < 0)
            return NativeMethods.CallNextHookEx(_hook, code, wParam, lParam);

        int message = (int)wParam;
        bool isKey = message is NativeMethods.WM_KEYDOWN
            or NativeMethods.WM_KEYUP
            or NativeMethods.WM_SYSKEYDOWN
            or NativeMethods.WM_SYSKEYUP;

        if (!isKey)
            return NativeMethods.CallNextHookEx(_hook, code, wParam, lParam);

        NativeMethods.KBDLLHOOKSTRUCT data = Marshal.PtrToStructure<NativeMethods.KBDLLHOOKSTRUCT>(lParam);
        bool down = message is NativeMethods.WM_KEYDOWN or NativeMethods.WM_SYSKEYDOWN;
        var key = new HookedKey(
            (int)data.vkCode,
            (int)data.scanCode,
            (data.flags & NativeMethods.LLKHF_EXTENDED) != 0,
            down,
            (data.flags & NativeMethods.LLKHF_INJECTED) != 0);

        CaptureDecision decision;
        try
        {
            decision = _handler(key);
        }
        catch (Exception)
        {
            // A fault here must not swallow the user's keyboard. Falling through to the next hook leaves the
            // CLIENT machine usable, which is the safe failure.
            return NativeMethods.CallNextHookEx(_hook, code, wParam, lParam);
        }

        return decision == CaptureDecision.Swallow
            ? new IntPtr(1)
            : NativeMethods.CallNextHookEx(_hook, code, wParam, lParam);
    }

    public void Dispose() => Remove();
}
