// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Threading;
using System.Threading.Tasks;

namespace KVMpad.Client.Core.Session;

/// <summary>
/// A frame-oriented, ordered, authenticated link to the device. Abstracted so the session can be exercised
/// against an in-process fake instead of a real socket; the production implementation is
/// <see cref="ClientWebSocketTransport"/> and it is the only place TLS and pinning are configured.
/// </summary>
public interface IMessageTransport : IAsyncDisposable
{
    bool IsOpen { get; }

    /// <summary>The subprotocol the device selected, for the version check the client makes after connecting.</summary>
    string? NegotiatedSubprotocol { get; }

    Task ConnectAsync(CancellationToken cancellationToken);

    Task SendAsync(ReadOnlyMemory<byte> frame, CancellationToken cancellationToken);

    /// <summary>One frame, or null once the peer has closed.</summary>
    Task<byte[]?> ReceiveAsync(CancellationToken cancellationToken);

    Task CloseAsync(CancellationToken cancellationToken);
}
