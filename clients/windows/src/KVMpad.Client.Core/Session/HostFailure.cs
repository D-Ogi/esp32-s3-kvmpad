// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
using System.IO;
using KVMpad.Client.Core.Protocol;

namespace KVMpad.Client.Core.Session;

public enum HostFailureReason
{
    Unpaired, NoSession, SessionClosed, MissingComponent, StartProcess, Listener, ListenerTimeout,
    IPv4Required, InvalidListener, CertificateSize, RequestTimeout, Rejected, Pending, ProcessClosed,
    PinRejected, Authentication, ServiceUnavailable, Protocol, ProxyRefused
}

/// <summary>Messages safe to present without exposing helper output, credentials or peer-supplied text.</summary>
public sealed class HostFailure : IOException
{
    public HostFailure(HostFailureReason reason, string? deviceCode = null, System.Exception? inner = null) : base(reason switch
    {
        HostFailureReason.Unpaired => "The device is not paired to this CLIENT.",
        HostFailureReason.NoSession => "Connect to the paired device before starting HOST.",
        HostFailureReason.SessionClosed => "The paired CLIENT session closed.",
        HostFailureReason.MissingComponent => "The Windows HOST component is missing.",
        HostFailureReason.StartProcess => "Windows could not start the HOST process. Check the Python installation.",
        HostFailureReason.Listener => "The Windows HOST process closed before its listener was ready. Check that the HOST port is available.",
        HostFailureReason.ListenerTimeout => "The Windows HOST listener did not become ready in time.",
        HostFailureReason.IPv4Required => "HOST requires a device with an IPv4 address.",
        HostFailureReason.InvalidListener => "The Windows HOST listener returned an invalid port or certificate.",
        HostFailureReason.CertificateSize => "The HOST certificate exceeds the device frame limit.",
        HostFailureReason.RequestTimeout => "The paired device did not acknowledge the HOST request in time.",
        HostFailureReason.Rejected when ErrorCodes.IsKnown(deviceCode) => $"The device rejected the HOST request ({deviceCode}).",
        HostFailureReason.Rejected => "The device rejected the HOST request.",
        HostFailureReason.Pending => "Another HOST request is still pending.",
        HostFailureReason.ProcessClosed => "The Windows HOST process closed unexpectedly.",
        HostFailureReason.PinRejected => "The device TLS certificate did not match this pairing.",
        HostFailureReason.Authentication => "The device rejected CLIENT authentication. Check its paired clients.",
        HostFailureReason.ServiceUnavailable => "The device's CLIENT WebSocket service is unavailable.",
        HostFailureReason.Protocol => "The device did not negotiate the supported CLIENT WebSocket protocol.",
        HostFailureReason.ProxyRefused => "Windows selected a proxy for the KVMpad connection. TCP was refused before the device handshake. Check the proxy settings for this Windows session.",
        _ => "The HOST connection could not finish."
    }, inner) { }
}
