// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
using System;
using System.ComponentModel;
using System.Net;
using System.Net.Http;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Security.Authentication;
namespace KVMpad.Client.Core.Session;

public static class HostDiagnostics
{
    internal static bool RefusedOnProxyRoute(Exception error, Uri endpoint, IWebProxy proxy)
    {
        for (Exception? cause = error; cause != null; cause = cause.InnerException)
        {
            if (cause is not SocketException { SocketErrorCode: SocketError.ConnectionRefused }) continue;
            // ClientWebSocket performs an HTTPS upgrade for a wss endpoint.
            var target = new UriBuilder(endpoint) { Scheme = Uri.UriSchemeHttps, Port = endpoint.Port }.Uri;
            try
            {
                return !proxy.IsBypassed(target) && proxy.GetProxy(target) is { } via &&
                    (via.IdnHost != target.IdnHost || via.Port != target.Port);
            }
            catch (Exception)
            {
                // A failed proxy lookup must preserve the original connection error.
                return false;
            }
        }
        return false;
    }

    public static HostFailure? HandshakeFailure(ClientWebSocketTransport transport)
    {
        if (transport.CertificateRejected) return new(HostFailureReason.PinRejected);
        if (transport.LastFailureReason != null) return new(HostFailureReason.Protocol);
        return transport.LastUpgradeStatus switch {
            HttpStatusCode.Unauthorized or HttpStatusCode.Forbidden => new(HostFailureReason.Authentication),
            HttpStatusCode.NotFound or HttpStatusCode.ServiceUnavailable or HttpStatusCode.BadGateway => new(HostFailureReason.ServiceUnavailable),
            _ => null
        };
    }

    public static string Describe(Exception error) => Describe(error, "The paired HOST connection could not finish.");

    public static string Describe(Exception error, string fallback)
    {
        string result = fallback;
        for (Exception? e = error; e != null; e = e.InnerException)
        {
            switch (e)
            {
                case HostFailure failure: return failure.Message;
                case TimeoutException: return "The paired device did not respond in time.";
                case OperationCanceledException: return "The paired CLIENT connection closed.";
                case SocketException socket:
                    return socket.SocketErrorCode switch {
                        SocketError.ConnectionRefused => "The TCP connection was refused. Check the device address and its Remote Network setting.",
                        SocketError.HostNotFound or SocketError.NoData => "Windows could not resolve the paired device address.",
                        SocketError.HostUnreachable or SocketError.NetworkUnreachable => "Windows could not reach the paired device on the network.",
                        SocketError.TimedOut => "The TCP connection to the paired device timed out.",
                        SocketError.ConnectionReset or SocketError.ConnectionAborted => "The TCP connection to the paired device closed.",
                        _ => "The TCP connection to the paired device failed."
                    };
                case Win32Exception win32 when (uint)win32.NativeErrorCode == 0x8009030E:
                    return "Windows TLS could not obtain credentials (SEC_E_NO_CREDENTIALS).";
                case AuthenticationException: result = "Windows could not complete the TLS handshake with the paired device."; break;
                case HttpRequestException { StatusCode: HttpStatusCode.Unauthorized or HttpStatusCode.Forbidden }:
                    return "The device rejected CLIENT authentication. Check its paired clients.";
                case HttpRequestException { HttpRequestError: HttpRequestError.SecureConnectionError }:
                    result = "Windows could not complete the TLS handshake with the paired device."; break;
                case WebSocketException: result = "The device could not open the CLIENT WebSocket service."; break;
            }
        }
        return result;
    }
}
