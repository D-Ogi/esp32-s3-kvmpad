// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Channels;
using System.Threading.Tasks;
using KVMpad.Client.Core.Pairing;
using KVMpad.Client.Core.Protocol;
using KVMpad.Client.Core.Session;

namespace KVMpad.Client.Core.Tests;

/// <summary>A clock the test moves by hand, so a heartbeat deadline is checked instead of waited for.</summary>
internal sealed class FakeClock : IClock
{
    public TimeSpan Elapsed { get; private set; } = TimeSpan.Zero;

    public void Advance(TimeSpan delta) => Elapsed += delta;

    public void AdvanceMilliseconds(double milliseconds) => Advance(TimeSpan.FromMilliseconds(milliseconds));
}

/// <summary>An in-process transport that records what was sent and replays what the test queues.</summary>
internal sealed class FakeTransport : IMessageTransport
{
    private readonly Channel<byte[]> _inbound = Channel.CreateUnbounded<byte[]>();

    /// <summary>
    /// Guards <see cref="_sent"/>. The session pump sends from its own task while the test thread polls, so an
    /// unguarded list would hand the test a torn view — which is a flaw in the fake, not in the client.
    /// </summary>
    private readonly object _gate = new();

    private readonly List<byte[]> _sent = new();

    /// <summary>A snapshot of the frames sent so far; safe to index and count while the pump runs.</summary>
    public IReadOnlyList<byte[]> Sent
    {
        get
        {
            lock (_gate)
                return _sent.ToArray();
        }
    }

    public bool IsOpen { get; private set; }

    public string? NegotiatedSubprotocol => ProtocolLimits.SessionSubprotocol;

    public bool CloseRequested { get; private set; }

    public Task ConnectAsync(CancellationToken cancellationToken)
    {
        IsOpen = true;
        return Task.CompletedTask;
    }

    public Task SendAsync(ReadOnlyMemory<byte> frame, CancellationToken cancellationToken)
    {
        byte[] copy = frame.ToArray();
        lock (_gate)
            _sent.Add(copy);
        return Task.CompletedTask;
    }

    public async Task<byte[]?> ReceiveAsync(CancellationToken cancellationToken)
    {
        try
        {
            return await _inbound.Reader.ReadAsync(cancellationToken).ConfigureAwait(false);
        }
        catch (ChannelClosedException)
        {
            return null;
        }
    }

    public Task CloseAsync(CancellationToken cancellationToken)
    {
        CloseRequested = true;
        IsOpen = false;
        return Task.CompletedTask;
    }

    public ValueTask DisposeAsync()
    {
        IsOpen = false;
        return ValueTask.CompletedTask;
    }

    /// <summary>Queues a frame for the client to receive.</summary>
    public void Enqueue(string json) => _inbound.Writer.TryWrite(Encoding.UTF8.GetBytes(json));

    public void EndOfStream() => _inbound.Writer.TryComplete();

    /// <summary>The frames sent so far, as text, for assertions.</summary>
    public IReadOnlyList<string> SentText
    {
        get
        {
            IReadOnlyList<byte[]> frames = Sent;
            var text = new List<string>(frames.Count);
            foreach (byte[] frame in frames)
                text.Add(Encoding.UTF8.GetString(frame));
            return text;
        }
    }

    public IReadOnlyList<ServerMessage> SentDecoded
    {
        get
        {
            IReadOnlyList<byte[]> frames = Sent;
            var decoded = new List<ServerMessage>(frames.Count);
            foreach (byte[] frame in frames)
                decoded.Add(ProtocolCodec.Decode(frame));
            return decoded;
        }
    }
}

/// <summary>
/// A stand-in for the real PAKE, exercising the wire framing and the channel-binding plumbing in
/// <see cref="PairingClient"/> without implying anything about the algorithm ADR-029 will choose. Its
/// "shared secret" is a hash, which is not a PAKE and is only ever used here.
/// </summary>
internal sealed class FakePakeHandshake : IPakeHandshake
{
    public string AlgorithmId => "test-fake";

    public int WireVersion => 1;

    /// <summary>The device tag this stand-in will accept; null accepts anything.</summary>
    public byte[]? DeviceConfirmationToAccept { get; set; }

    /// <summary>The blobs the driver handed over, in order, so a test can check what was consumed.</summary>
    public List<byte[]> Consumed { get; } = new();

    /// <summary>What <see cref="OpenToken"/> returns on success.</summary>
    public byte[] TokenToReturn { get; set; } = Encoding.ASCII.GetBytes("0123456789abcdef0123456789abcdef");

    /// <summary>The sealed bytes the driver passed to <see cref="OpenToken"/>.</summary>
    public byte[]? SealedTokenSeen { get; private set; }

    public bool Disposed { get; private set; }

    public void Begin(PairingContext context)
    {
        // The real core truncates the client name to the wire cap and the binding commits to the truncated
        // form; the stand-in mimics that so the driver's use of EffectiveClientName is exercised.
        context.EffectiveClientName = context.ClientName.Length > 23
            ? context.ClientName[..23]
            : context.ClientName;
    }

    public byte[] Round1(byte[] deviceRound1)
    {
        Consumed.Add(deviceRound1);
        return Encoding.ASCII.GetBytes("client-round-1");
    }

    public byte[] Round2(byte[] deviceRound2)
    {
        Consumed.Add(deviceRound2);
        return Encoding.ASCII.GetBytes("client-round-2");
    }

    public byte[] ConfirmationOut() => Encoding.ASCII.GetBytes("client-confirm");

    public void ConfirmationIn(byte[] deviceConfirmation)
    {
        Consumed.Add(deviceConfirmation);
        if (DeviceConfirmationToAccept is null)
            return;
        if (!CryptographicOperations.FixedTimeEquals(deviceConfirmation, DeviceConfirmationToAccept))
        {
            throw new PairingFailedException(
                PairingRejectionReason.CodeMismatch,
                PairingFailedException.Describe(PairingRejectionReason.CodeMismatch));
        }
    }

    public byte[] OpenToken(byte[] sealedToken)
    {
        SealedTokenSeen = sealedToken;
        return TokenToReturn;
    }

    public byte[] Transcript() => SHA256.HashData(Encoding.ASCII.GetBytes("test-transcript"));

    public void Dispose() => Disposed = true;
}
