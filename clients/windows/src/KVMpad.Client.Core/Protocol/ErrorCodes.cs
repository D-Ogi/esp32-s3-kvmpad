// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

namespace KVMpad.Client.Core.Protocol;

/// <summary>Error codes from 03 §6.5 and §7, with the client-side reaction each one needs.</summary>
public static class ErrorCodes
{
    public const string Version = "ERR_VERSION";
    public const string BadFrame = "ERR_BAD_FRAME";
    public const string Range = "ERR_RANGE";
    public const string Usage = "ERR_USAGE";
    public const string Replay = "ERR_REPLAY";
    public const string StaleEpoch = "ERR_STALE_EPOCH";
    public const string Session = "ERR_SESSION";
    public const string HostOffline = "ERR_HOST_OFFLINE";
    public const string UsbSuspended = "ERR_USB_SUSPENDED";
    public const string TouchIo = "ERR_TOUCH_IO";
    public const string ClientStale = "ERR_CLIENT_STALE";
    public const string ControlDenied = "ERR_CONTROL_DENIED";
    public const string PairingExpired = "ERR_PAIRING_EXPIRED";
    public const string HidQueueOverflow = "ERR_HID_QUEUE_OVERFLOW";
    public const string OtaInProgress = "ERR_OTA_IN_PROGRESS";

    /// <summary>
    /// True when the code means "this client no longer owns the lease, or never did". The session must
    /// drop its epoch and release every locally held key and button before it sends anything else,
    /// because further input would only earn more rejections (03 §7, AGENTS §2.3).
    /// </summary>
    public static bool RequiresLocalRelease(string code) => code switch
    {
        StaleEpoch => true,
        Session => true,
        ClientStale => true,
        ControlDenied => true,
        HostOffline => true,
        UsbSuspended => true,
        HidQueueOverflow => true,
        OtaInProgress => true,
        _ => false,
    };

    /// <summary>True when the code means the connection itself must be torn down (03 §6.5).</summary>
    public static bool IsFatalForConnection(string code) => code switch
    {
        Version => true,
        BadFrame => true,
        Replay => true,
        Session => true,
        _ => false,
    };
}
