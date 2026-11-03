// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Net.WebSockets;
using System.Threading;
using System.Threading.Tasks;
using KVMpad.Client.Core.Protocol;

namespace KVMpad.Client.Core.Session;

/// <summary>
/// The transport for one pairing attempt: <c>wss://host:port/v1/pair</c>, subprotocol
/// <c>kvmpad.pair.v1</c>, no <c>Authorization</c> header (ADR-028, ADR-029 §4).
/// </summary>
/// <remarks>
/// <para>
/// This is the only place in the client where a certificate is accepted without being pinned, and it is the
/// one place SEC-103 permits it: there is no pin yet, the endpoint exists only while the device's pairing
/// window is open, and it carries <c>pair.*</c> frames and nothing else. The certificate is recorded rather
/// than trusted — the attempt only produces a token if the PAKE transcript confirms that exact fingerprint,
/// and the fingerprint that gets pinned is the captured one, never whatever a later handshake presents
/// (SEC-102).
/// </para>
/// <para>
/// It is still <c>wss</c>. Pairing over plain <c>ws://</c> does not exist here either (TECH-041).
/// </para>
/// </remarks>
public sealed class PairingWebSocketTransport : IMessageTransport
{
    private readonly Uri _uri;
    private readonly PairingCertificateCapture _capture = new();
    private readonly byte[] _receiveBuffer = new byte[ProtocolLimits.MaxFrameBytes + 1];
    private ClientWebSocket? _socket;

    public PairingWebSocketTransport(string host, int port)
    {
        if (string.IsNullOrWhiteSpace(host))
            throw new ArgumentException("A host is required.", nameof(host));
        if (port is <= 0 or > 65535)
            throw new ArgumentOutOfRangeException(nameof(port), "The port must be between 1 and 65535.");

        Host = host;
        Port = port;
        _uri = new Uri($"wss://{FormatHost(host)}:{port}{ProtocolLimits.PairingPath}");
    }

    public string Host { get; }

    public int Port { get; }

    public Uri Uri => _uri;

    /// <summary>
    /// The fingerprint of the certificate this handshake presented — what the channel binding commits to and
    /// what gets pinned if the attempt succeeds. Null until the handshake has run.
    /// </summary>
    public string? CapturedFingerprint => _capture.CapturedFingerprint;

    public bool IsOpen => _socket?.State == WebSocketState.Open;

    public string? NegotiatedSubprotocol => _socket?.SubProtocol;

    public async Task ConnectAsync(CancellationToken cancellationToken)
    {
        var socket = new ClientWebSocket();
        socket.Options.RemoteCertificateValidationCallback =
            (sender, certificate, chain, errors) => _capture.Callback(sender, certificate, chain, errors);
        socket.Options.AddSubProtocol(ProtocolLimits.PairingSubprotocol);
        socket.Options.KeepAliveInterval = TimeSpan.Zero;

        try
        {
            await socket.ConnectAsync(_uri, cancellationToken).ConfigureAwait(false);
        }
        catch
        {
            socket.Dispose();
            throw;
        }

        if (socket.SubProtocol != ProtocolLimits.PairingSubprotocol)
        {
            socket.Dispose();
            throw new InvalidOperationException(
                $"The device did not select the '{ProtocolLimits.PairingSubprotocol}' subprotocol; refusing to pair over an unversioned channel.");
        }
        if (_capture.CapturedFingerprint is null)
        {
            // Without a captured certificate there is nothing to bind the exchange to, so there is no safe
            // way to continue (SEC-102).
            socket.Dispose();
            throw new InvalidOperationException("The pairing handshake presented no certificate.");
        }

        _socket = socket;
    }

    public async Task SendAsync(ReadOnlyMemory<byte> frame, CancellationToken cancellationToken)
    {
        ClientWebSocket socket = _socket ?? throw new InvalidOperationException("The pairing socket is not open.");
        await socket.SendAsync(frame, WebSocketMessageType.Text, true, cancellationToken).ConfigureAwait(false);
    }

    public async Task<byte[]?> ReceiveAsync(CancellationToken cancellationToken)
    {
        ClientWebSocket socket = _socket ?? throw new InvalidOperationException("The pairing socket is not open.");

        int offset = 0;
        while (true)
        {
            WebSocketReceiveResult result;
            try
            {
                result = await socket
                    .ReceiveAsync(new ArraySegment<byte>(_receiveBuffer, offset, _receiveBuffer.Length - offset),
                        cancellationToken)
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
            {
                // Over the frame limit: fail closed rather than grow a buffer for an untrusted peer.
                return null;
            }
            if (result.EndOfMessage)
                break;
        }

        byte[] frame = new byte[offset];
        Array.Copy(_receiveBuffer, frame, offset);
        return frame;
    }

    public async Task CloseAsync(CancellationToken cancellationToken)
    {
        ClientWebSocket? socket = _socket;
        if (socket is null)
            return;
        if (socket.State == WebSocketState.Open)
        {
            try
            {
                await socket
                    .CloseAsync(WebSocketCloseStatus.NormalClosure, "pairing finished", cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (WebSocketException)
            {
                // The device closed first; nothing to report.
            }
        }
    }

    public ValueTask DisposeAsync()
    {
        _socket?.Dispose();
        _socket = null;
        return ValueTask.CompletedTask;
    }

    private static string FormatHost(string host) => host.Contains(':') ? $"[{host}]" : host;
}
