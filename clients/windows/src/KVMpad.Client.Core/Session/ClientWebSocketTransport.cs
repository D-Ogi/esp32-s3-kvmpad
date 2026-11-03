// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Net.WebSockets;
using System.Threading;
using System.Threading.Tasks;
using KVMpad.Client.Core.Protocol;

namespace KVMpad.Client.Core.Session;

/// <summary>Everything needed to open one authenticated control session.</summary>
public sealed class SessionEndpoint
{
    public SessionEndpoint(string host, int port, string pinnedFingerprint, string token)
    {
        if (string.IsNullOrWhiteSpace(host))
            throw new ArgumentException("Host is required.", nameof(host));
        if (port is < 1 or > 65535)
            throw new ArgumentOutOfRangeException(nameof(port), port, "Port out of range.");
        if (!CertificateFingerprint.IsWellFormed(pinnedFingerprint))
            throw new ArgumentException("A pinned fingerprint is required; there is no unpinned session.", nameof(pinnedFingerprint));
        if (string.IsNullOrEmpty(token))
            throw new ArgumentException("A pairing token is required.", nameof(token));

        Host = host;
        Port = port;
        PinnedFingerprint = pinnedFingerprint;
        Token = token;
    }

    public string Host { get; }
    public int Port { get; }

    /// <summary>SHA-256 of the device certificate, captured at pairing (ADR-022 SEC-102).</summary>
    public string PinnedFingerprint { get; }

    /// <summary>
    /// The 32-byte pairing token, base64url without padding. Sent as a handshake credential and never
    /// written to a log, a file or the URL (03 §6.1, SEC-105).
    /// </summary>
    public string Token { get; }

    /// <summary>
    /// <c>wss://host:port/v1/ws</c>. The scheme is fixed: there is no <c>ws://</c> variant to fall back to
    /// (TECH-041, AT-SEC-01), and no credential ever appears in the URI.
    /// </summary>
    public Uri SessionUri => new($"wss://{FormatHost(Host)}:{Port}{ProtocolLimits.SessionPath}");

    /// <summary><c>wss://host:port/v1/pair</c>, reachable only inside an open pairing window (SEC-103, SEC-106).</summary>
    public Uri PairingUri => new($"wss://{FormatHost(Host)}:{Port}{ProtocolLimits.PairingPath}");

    private static string FormatHost(string host) => host.Contains(':') ? "[" + host + "]" : host;
}

/// <summary>
/// The production transport: a <see cref="ClientWebSocket"/> over TLS with certificate pinning and the
/// pairing token presented in the upgrade request.
/// </summary>
/// <remarks>
/// <para>
/// <b>Trust.</b> <see cref="ClientWebSocketOptions.RemoteCertificateValidationCallback"/> is set to the
/// pinner, which accepts exactly one certificate and nothing else. There is no option, setting or code path
/// that opens a session over <c>ws://</c> or over TLS whose certificate does not match the pin
/// (TECH-041, 03 §6.1, AT-SEC-01).
/// </para>
/// <para>
/// <b>Authentication.</b> The token travels as <c>Authorization: Bearer &lt;token&gt;</c> on the HTTP upgrade
/// request, so the device can refuse an unauthenticated peer with a 401 before any WebSocket state exists.
/// The spec forbids a token in the query string (03 §6.1); a header also keeps it out of request logs and
/// out of the <c>hello</c> payload, which is plain protocol data the device echoes into diagnostics.
/// </para>
/// </remarks>
public sealed class ClientWebSocketTransport : IMessageTransport
{
    private readonly SessionEndpoint _endpoint;
    private readonly CertificatePinner _pinner;
    private readonly byte[] _receiveBuffer = new byte[ProtocolLimits.MaxFrameBytes + 1];
    private ClientWebSocket? _socket;

    public ClientWebSocketTransport(SessionEndpoint endpoint)
    {
        _endpoint = endpoint ?? throw new ArgumentNullException(nameof(endpoint));
        _pinner = new CertificatePinner(endpoint.PinnedFingerprint);
    }

    public bool IsOpen => _socket?.State == WebSocketState.Open;

    public string? NegotiatedSubprotocol => _socket?.SubProtocol;

    /// <summary>The reason the last handshake or pinning check failed, for the status line.</summary>
    public string? LastFailureReason { get; private set; }

    public async Task ConnectAsync(CancellationToken cancellationToken)
    {
        var socket = new ClientWebSocket();
        socket.Options.RemoteCertificateValidationCallback = (_, certificate, _, _) =>
        {
            PinningVerdict verdict = _pinner.Check(certificate);
            if (!verdict.Accepted)
                LastFailureReason = verdict.Reason;
            return verdict.Accepted;
        };
        socket.Options.AddSubProtocol(ProtocolLimits.SessionSubprotocol);
        socket.Options.SetRequestHeader("Authorization", "Bearer " + _endpoint.Token);
        socket.Options.KeepAliveInterval = TimeSpan.Zero; // the protocol's own ping is the heartbeat

        try
        {
            await socket.ConnectAsync(_endpoint.SessionUri, cancellationToken).ConfigureAwait(false);
        }
        catch
        {
            socket.Dispose();
            throw;
        }

        if (socket.SubProtocol != ProtocolLimits.SessionSubprotocol)
        {
            LastFailureReason =
                $"The device did not select the '{ProtocolLimits.SessionSubprotocol}' subprotocol; refusing an unversioned session.";
            socket.Dispose();
            throw new InvalidOperationException(LastFailureReason);
        }

        _socket = socket;
    }

    public async Task SendAsync(ReadOnlyMemory<byte> frame, CancellationToken cancellationToken)
    {
        ClientWebSocket socket = _socket ?? throw new InvalidOperationException("The transport is not connected.");
        if (frame.Length > ProtocolLimits.MaxFrameBytes)
            throw new ProtocolFrameException($"Refusing to send {frame.Length} B; max_frame_size is {ProtocolLimits.MaxFrameBytes} B.");
        await socket.SendAsync(frame, WebSocketMessageType.Text, endOfMessage: true, cancellationToken).ConfigureAwait(false);
    }

    public async Task<byte[]?> ReceiveAsync(CancellationToken cancellationToken)
    {
        ClientWebSocket socket = _socket ?? throw new InvalidOperationException("The transport is not connected.");
        int offset = 0;

        while (true)
        {
            WebSocketReceiveResult result;
            try
            {
                result = await socket
                    .ReceiveAsync(new ArraySegment<byte>(_receiveBuffer, offset, _receiveBuffer.Length - offset), cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (WebSocketException)
            {
                return null;
            }

            if (result.MessageType == WebSocketMessageType.Close)
                return null;

            offset += result.Count;
            if (offset > ProtocolLimits.MaxFrameBytes)
                throw new ProtocolFrameException(
                    $"Inbound frame exceeds max_frame_size={ProtocolLimits.MaxFrameBytes} B; closing (03 §6.5).");

            if (result.EndOfMessage)
                break;

            if (offset == _receiveBuffer.Length)
                throw new ProtocolFrameException("Inbound frame does not fit the protocol limit.");
        }

        return _receiveBuffer.AsSpan(0, offset).ToArray();
    }

    public async Task CloseAsync(CancellationToken cancellationToken)
    {
        ClientWebSocket? socket = _socket;
        if (socket is null || socket.State != WebSocketState.Open)
            return;
        try
        {
            await socket.CloseAsync(WebSocketCloseStatus.NormalClosure, "session.close", cancellationToken).ConfigureAwait(false);
        }
        catch (WebSocketException)
        {
            // The peer may already be gone; the caller's release path does not depend on a clean close.
        }
    }

    public ValueTask DisposeAsync()
    {
        _socket?.Dispose();
        _socket = null;
        return ValueTask.CompletedTask;
    }
}
