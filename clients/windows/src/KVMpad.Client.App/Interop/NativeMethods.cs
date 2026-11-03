// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Runtime.InteropServices;

namespace KVMpad.Client.App.Interop;

/// <summary>
/// The Win32 surface this client uses. Declared in one place, with the struct layouts the 64-bit ABI defines;
/// the project builds as win-x64 so there is no 32-bit variant to keep correct as well.
/// </summary>
internal static partial class NativeMethods
{
    // ---- window messages -------------------------------------------------------------------------
    internal const int WM_INPUT = 0x00FF;
    internal const int WM_POWERBROADCAST = 0x0218;
    internal const int WM_WTSSESSION_CHANGE = 0x02B1;

    internal const int PBT_APMSUSPEND = 0x0004;
    internal const int PBT_APMRESUMESUSPEND = 0x0007;

    internal const int WTS_CONSOLE_DISCONNECT = 0x2;
    internal const int WTS_REMOTE_DISCONNECT = 0x4;
    internal const int WTS_SESSION_LOGOFF = 0x6;
    internal const int WTS_SESSION_LOCK = 0x7;

    // ---- low-level keyboard hook ----------------------------------------------------------------
    internal const int WH_KEYBOARD_LL = 13;

    internal const int WM_KEYDOWN = 0x0100;
    internal const int WM_KEYUP = 0x0101;
    internal const int WM_SYSKEYDOWN = 0x0104;
    internal const int WM_SYSKEYUP = 0x0105;

    internal const uint LLKHF_EXTENDED = 0x01;
    internal const uint LLKHF_INJECTED = 0x10;
    internal const uint LLKHF_UP = 0x80;

    [StructLayout(LayoutKind.Sequential)]
    internal struct KBDLLHOOKSTRUCT
    {
        public uint vkCode;
        public uint scanCode;
        public uint flags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    internal delegate IntPtr HookProc(int code, IntPtr wParam, IntPtr lParam);

    [LibraryImport("user32.dll", EntryPoint = "SetWindowsHookExW", SetLastError = true)]
    internal static partial IntPtr SetWindowsHookEx(int idHook, IntPtr lpfn, IntPtr hMod, uint dwThreadId);

    [LibraryImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static partial bool UnhookWindowsHookEx(IntPtr hhk);

    [LibraryImport("user32.dll")]
    internal static partial IntPtr CallNextHookEx(IntPtr hhk, int nCode, IntPtr wParam, IntPtr lParam);

    [LibraryImport("kernel32.dll", EntryPoint = "GetModuleHandleW", SetLastError = true, StringMarshalling = StringMarshalling.Utf16)]
    internal static partial IntPtr GetModuleHandle(string? lpModuleName);

    // ---- raw input -------------------------------------------------------------------------------
    internal const ushort HID_USAGE_PAGE_GENERIC = 0x01;
    internal const ushort HID_USAGE_GENERIC_MOUSE = 0x02;

    internal const uint RIDEV_INPUTSINK = 0x00000100;
    internal const uint RIDEV_REMOVE = 0x00000001;
    internal const uint RID_INPUT = 0x10000003;
    internal const uint RIM_TYPEMOUSE = 0;

    internal const ushort RI_MOUSE_LEFT_BUTTON_DOWN = 0x0001;
    internal const ushort RI_MOUSE_LEFT_BUTTON_UP = 0x0002;
    internal const ushort RI_MOUSE_RIGHT_BUTTON_DOWN = 0x0004;
    internal const ushort RI_MOUSE_RIGHT_BUTTON_UP = 0x0008;
    internal const ushort RI_MOUSE_MIDDLE_BUTTON_DOWN = 0x0010;
    internal const ushort RI_MOUSE_MIDDLE_BUTTON_UP = 0x0020;
    internal const ushort RI_MOUSE_WHEEL = 0x0400;
    internal const ushort RI_MOUSE_HWHEEL = 0x0800;

    internal const ushort MOUSE_MOVE_ABSOLUTE = 0x01;

    [StructLayout(LayoutKind.Sequential)]
    internal struct RAWINPUTDEVICE
    {
        public ushort usUsagePage;
        public ushort usUsage;
        public uint dwFlags;
        public IntPtr hwndTarget;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RAWINPUTHEADER
    {
        public uint dwType;
        public uint dwSize;
        public IntPtr hDevice;
        public IntPtr wParam;
    }

    /// <summary>
    /// <c>RAWMOUSE</c>. <c>usFlags</c> is followed by two padding bytes because the union that comes next is
    /// ULONG-aligned; the padding is declared rather than left to chance.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct RAWMOUSE
    {
        public ushort usFlags;
        private readonly ushort _reserved;
        public ushort usButtonFlags;
        public ushort usButtonData;
        public uint ulRawButtons;
        public int lLastX;
        public int lLastY;
        public uint ulExtraInformation;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RAWINPUTMOUSE
    {
        public RAWINPUTHEADER header;
        public RAWMOUSE mouse;
    }

    [LibraryImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static partial bool RegisterRawInputDevices(
        [In] RAWINPUTDEVICE[] pRawInputDevices, uint uiNumDevices, uint cbSize);

    [LibraryImport("user32.dll", SetLastError = true)]
    internal static partial uint GetRawInputData(
        IntPtr hRawInput, uint uiCommand, IntPtr pData, ref uint pcbSize, uint cbSizeHeader);

    // Device identity, for the feedback-loop guard (KvmpadUsbIdentity).
    internal const uint RIDEV_DEVNOTIFY = 0x00002000;
    internal const int WM_INPUT_DEVICE_CHANGE = 0x00FE;
    internal const int GIDC_ARRIVAL = 1;
    internal const uint RIDI_DEVICENAME = 0x20000007;

    [StructLayout(LayoutKind.Sequential)]
    internal struct RAWINPUTDEVICELIST
    {
        public IntPtr hDevice;
        public uint dwType;
    }

    [LibraryImport("user32.dll", SetLastError = true)]
    internal static partial uint GetRawInputDeviceList(
        [Out] RAWINPUTDEVICELIST[]? pRawInputDeviceList, ref uint puiNumDevices, uint cbSize);

    [LibraryImport("user32.dll", EntryPoint = "GetRawInputDeviceInfoW", SetLastError = true)]
    internal static partial uint GetRawInputDeviceInfo(IntPtr hDevice, uint uiCommand, IntPtr pData, ref uint pcbSize);

    // ---- session notifications -------------------------------------------------------------------
    internal const int NOTIFY_FOR_THIS_SESSION = 0;

    [LibraryImport("wtsapi32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static partial bool WTSRegisterSessionNotification(IntPtr hWnd, uint dwFlags);

    [LibraryImport("wtsapi32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static partial bool WTSUnRegisterSessionNotification(IntPtr hWnd);

    // ---- credential manager ----------------------------------------------------------------------
    internal const uint CRED_TYPE_GENERIC = 1;
    internal const uint CRED_PERSIST_LOCAL_MACHINE = 2;
    internal const uint CRED_ENUMERATE_ALL_CREDENTIALS = 0x1;

    [StructLayout(LayoutKind.Sequential)]
    internal struct CREDENTIAL
    {
        public uint Flags;
        public uint Type;
        public IntPtr TargetName;
        public IntPtr Comment;
        public long LastWritten;
        public uint CredentialBlobSize;
        public IntPtr CredentialBlob;
        public uint Persist;
        public uint AttributeCount;
        public IntPtr Attributes;
        public IntPtr TargetAlias;
        public IntPtr UserName;
    }

    [LibraryImport("advapi32.dll", EntryPoint = "CredWriteW", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static partial bool CredWrite(ref CREDENTIAL credential, uint flags);

    [LibraryImport("advapi32.dll", EntryPoint = "CredReadW", SetLastError = true, StringMarshalling = StringMarshalling.Utf16)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static partial bool CredRead(string target, uint type, uint reservedFlag, out IntPtr credentialPtr);

    [LibraryImport("advapi32.dll", EntryPoint = "CredDeleteW", SetLastError = true, StringMarshalling = StringMarshalling.Utf16)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static partial bool CredDelete(string target, uint type, uint flags);

    [LibraryImport("advapi32.dll", EntryPoint = "CredEnumerateW", SetLastError = true, StringMarshalling = StringMarshalling.Utf16)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static partial bool CredEnumerate(
        string? filter, uint flag, out uint count, out IntPtr credentialsPtr);

    [LibraryImport("advapi32.dll", EntryPoint = "CredFree")]
    internal static partial void CredFree(IntPtr buffer);
}
