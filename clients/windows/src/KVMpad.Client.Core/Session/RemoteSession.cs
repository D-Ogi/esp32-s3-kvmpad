// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using KVMpad.Client.Core.Input;
using KVMpad.Client.Core.Protocol;

namespace KVMpad.Client.Core.Session;

/// <summary>
/// Drives a <see cref="SessionLogic"/> over an <see cref="IMessageTransport"/>: one receive loop, one timer
/// loop, and a single place where frames are written. All protocol decisions stay in the logic object; this
/// class only moves bytes and ticks the clock.
/// </summary>
public sealed class RemoteSession : IAsyncDisposable
{
    private readonly IMessageTransport _transport;
    private readonly SessionLogic _logic;
    private readonly SemaphoreSlim _sendLock = new(1, 1);
    private readonly TimeSpan _tickInterval;
    private readonly System.Collections.Concurrent.ConcurrentQueue<byte[]> _outbound = new();

    public RemoteSession(IMessageTransport transport, SessionLogic logic, TimeSpan? tickInterval = null)
    {
        _transport = transport ?? throw new ArgumentNullException(nameof(transport));
        _logic = logic ?? throw new ArgumentNullException(nameof(logic));
        _tickInterval = tickInterval ?? TimeSpan.FromMilliseconds(50);
    }

    public SessionLogic Logic => _logic;

    /// <summary>Raised when the loop ends, with the reason the logic recorded.</summary>
    public event Action<SessionEndReason>? Ended;

    /// <summary>Raised for a frame that could not be parsed. The connection is closed; nothing is guessed at.</summary>
    public event Action<string>? FrameRejected;

    /// <summary>
    /// Connects, performs the handshake and pumps until the transport closes or the token is cancelled. On
    /// every exit path the logic runs its release, so no key is left believed to be held.
    /// </summary>
    public async Task RunAsync(CancellationToken cancellationToken)
    {
        await _transport.ConnectAsync(cancellationToken).ConfigureAwait(false);
        await SendAllAsync(_logic.OnTransportOpened(), cancellationToken).ConfigureAwait(false);

        using var linked = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        Task pump = PumpAsync(linked.Token);
        Task tick = TickAsync(linked.Token);

        try
        {
            await Task.WhenAny(pump, tick).ConfigureAwait(false);
        }
        finally
        {
            linked.Cancel();
            try
            {
                await Task.WhenAll(pump, tick).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                // Expected: cancelling the sibling loop is how this method returns.
            }
            catch (Exception)
            {
                // The reason is already recorded on the logic; the release below is what matters.
            }

            if (_logic.State != ClientSessionState.Disconnected)
                _logic.OnTransportClosed();
            Ended?.Invoke(_logic.EndReason);
        }
    }

    /// <summary>Asks the device for the lease (03 §6.3).</summary>
    public async Task RequestControlAsync(CancellationToken cancellationToken)
    {
        byte[]? frame = _logic.BuildControlRequest();
        if (frame is not null)
            await SendAsync(frame, cancellationToken).ConfigureAwait(false);
    }

    /// <summary>Gives the lease back, releasing everything held first.</summary>
    public Task ReleaseControlAsync(CancellationToken cancellationToken)
        => SendAllAsync(_logic.BuildControlRelease(), cancellationToken);

    public async Task RequestStatusAsync(CancellationToken cancellationToken)
    {
        byte[]? frame = _logic.BuildStatusGet();
        if (frame is not null)
            await SendAsync(frame, cancellationToken).ConfigureAwait(false);
    }

    /// <summary>Releases every held key and button and sends the UP events if the lease still allows it.</summary>
    public Task ReleaseAllAsync(ReleaseReason reason, CancellationToken cancellationToken)
        => SendAllAsync(_logic.BuildReleaseAllFrames(reason), cancellationToken);

    /// <summary>
    /// Hands frames to the send loop without blocking the caller. The keyboard hook runs on the UI thread and
    /// must return in microseconds, so it posts here instead of awaiting a socket write.
    /// </summary>
    public void Post(IReadOnlyList<byte[]> frames)
    {
        for (int i = 0; i < frames.Count; i++)
            _outbound.Enqueue(frames[i]);
    }

    /// <summary>Ends the session politely: release, <c>session.close</c>, then a clean WebSocket close.</summary>
    public async Task CloseAsync(CancellationToken cancellationToken)
    {
        await SendAllAsync(_logic.BuildSessionClose(), cancellationToken).ConfigureAwait(false);
        await _transport.CloseAsync(cancellationToken).ConfigureAwait(false);
    }

    private async Task PumpAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            byte[]? frame;
            try
            {
                frame = await _transport.ReceiveAsync(cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                return;
            }

            if (frame is null)
            {
                _logic.OnTransportClosed();
                return;
            }

            ServerMessage message;
            try
            {
                message = ProtocolCodec.Decode(frame);
            }
            catch (ProtocolFrameException ex)
            {
                FrameRejected?.Invoke(ex.Message);
                _logic.OnTransportClosed(SessionEndReason.ProtocolError);
                await _transport.CloseAsync(CancellationToken.None).ConfigureAwait(false);
                return;
            }

            await SendAllAsync(_logic.OnMessage(message), cancellationToken).ConfigureAwait(false);

            if (_logic.State == ClientSessionState.Disconnected)
                return;
        }
    }

    private async Task TickAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(_tickInterval, cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                return;
            }

            // Posted frames go first: they are the release events the hook produced, and a key UP has
            // priority over anything queued behind it (03 §5.3, AGENTS §2.3).
            while (_outbound.TryDequeue(out byte[]? posted))
                await SendAsync(posted, cancellationToken).ConfigureAwait(false);

            await SendAllAsync(_logic.OnTick(), cancellationToken).ConfigureAwait(false);
            await SendAllAsync(_logic.DrainInput(), cancellationToken).ConfigureAwait(false);

            if (_logic.State == ClientSessionState.Disconnected)
                return;
        }
    }

    private async Task SendAllAsync(IReadOnlyList<byte[]> frames, CancellationToken cancellationToken)
    {
        if (frames.Count == 0)
            return;
        foreach (byte[] frame in frames)
            await SendAsync(frame, cancellationToken).ConfigureAwait(false);
    }

    private async Task SendAsync(byte[] frame, CancellationToken cancellationToken)
    {
        if (!_transport.IsOpen)
            return;
        await _sendLock.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            await _transport.SendAsync(frame, cancellationToken).ConfigureAwait(false);
        }
        catch (Exception) when (!cancellationToken.IsCancellationRequested)
        {
            _logic.OnTransportClosed();
        }
        finally
        {
            _sendLock.Release();
        }
    }

    public async ValueTask DisposeAsync()
    {
        _sendLock.Dispose();
        await _transport.DisposeAsync().ConfigureAwait(false);
    }
}
