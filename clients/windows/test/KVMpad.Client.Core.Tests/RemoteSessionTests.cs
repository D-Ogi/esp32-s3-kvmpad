// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using KVMpad.Client.Core.Input;
using KVMpad.Client.Core.Protocol;
using KVMpad.Client.Core.Session;
using Xunit;

namespace KVMpad.Client.Core.Tests;

/// <summary>
/// The async host around <see cref="SessionLogic"/>, driven against an in-process transport: the handshake
/// goes out, inbound frames are applied, posted release frames are sent ahead of queued input, and a frame
/// the client cannot parse closes the connection instead of being skipped.
/// </summary>
public sealed class RemoteSessionTests
{
    private static (RemoteSession Session, SessionLogic Logic, FakeTransport Transport) NewSession()
    {
        var transport = new FakeTransport();
        var logic = new SessionLogic(SystemClock.Instance, new SessionOptions { ClientName = "Desk1" }, new CaptureLedger());
        var session = new RemoteSession(transport, logic, TimeSpan.FromMilliseconds(10));
        return (session, logic, transport);
    }

    private static async Task<bool> WaitForAsync(Func<bool> condition, TimeSpan timeout)
    {
        DateTime deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            if (condition())
                return true;
            await Task.Delay(10);
        }
        return condition();
    }

    [Fact]
    public async Task TheHandshakeGoesOutAndTheAckIsApplied()
    {
        (RemoteSession session, SessionLogic logic, FakeTransport transport) = NewSession();
        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));

        Task run = session.RunAsync(cts.Token);

        Assert.True(await WaitForAsync(() => transport.Sent.Count >= 2, TimeSpan.FromSeconds(5)));
        Assert.Equal(MessageTypes.Hello, ProtocolCodec.Decode(transport.Sent[0]).Type);
        Assert.Equal(MessageTypes.StatusGet, ProtocolCodec.Decode(transport.Sent[1]).Type);

        transport.Enqueue(
            """{"v":1,"type":"hello.ack","seq":1,"session_id":"c-xyz","payload":{"protocol":1,"device_id":"kvm-9","capabilities":["keyboard","mouse"]}}""");

        Assert.True(await WaitForAsync(() => logic.State == ClientSessionState.Connected, TimeSpan.FromSeconds(5)));
        Assert.Equal("c-xyz", logic.SessionId);
        Assert.Equal("kvm-9", logic.DeviceId);
        Assert.Contains("keyboard", logic.DeviceCapabilities);

        cts.Cancel();
        await run;
        await session.DisposeAsync();
    }

    [Fact]
    public async Task TheHeartbeatReachesTheDeviceWithoutAnyoneAskingForIt()
    {
        (RemoteSession session, SessionLogic logic, FakeTransport transport) = NewSession();
        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(15));

        Task run = session.RunAsync(cts.Token);
        transport.Enqueue("""{"v":1,"type":"hello.ack","seq":1,"session_id":"c-1","payload":{"protocol":1}}""");
        Assert.True(await WaitForAsync(() => logic.State == ClientSessionState.Connected, TimeSpan.FromSeconds(5)));

        Assert.True(await WaitForAsync(
            () => transport.SentDecoded.Any(m => m.Type == MessageTypes.Ping),
            ProtocolLimits.PingInterval + TimeSpan.FromSeconds(3)));

        cts.Cancel();
        await run;
        await session.DisposeAsync();
    }

    [Fact]
    public async Task PostedReleaseFramesAreSentAheadOfQueuedInput()
    {
        (RemoteSession session, SessionLogic logic, FakeTransport transport) = NewSession();
        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));

        Task run = session.RunAsync(cts.Token);
        transport.Enqueue("""{"v":1,"type":"hello.ack","seq":1,"session_id":"c-1","payload":{"protocol":1}}""");
        transport.Enqueue("""{"v":1,"type":"control.granted","seq":2,"session_id":"c-1","lease_epoch":5,"payload":{}}""");
        Assert.True(await WaitForAsync(() => logic.CanSendInput, TimeSpan.FromSeconds(5)));

        int before = transport.Sent.Count;
        logic.QueueInput(new KeyboardKeyPayload(HidUsage.LeftCtrl, true));
        session.Post(logic.BuildReleaseAllFrames(ReleaseReason.CaptureToggledOff));

        Assert.True(await WaitForAsync(() => transport.Sent.Count > before, TimeSpan.FromSeconds(5)));
        ServerMessage first = ProtocolCodec.Decode(transport.Sent[before]);
        Assert.Equal(MessageTypes.Input, first.Type);
        Assert.False(first.PayloadBool("down"));

        cts.Cancel();
        await run;
        await session.DisposeAsync();
    }

    [Fact]
    public async Task AFrameTheClientCannotParseClosesTheConnection()
    {
        // AT-SEC-04 read from this side: fail closed, do not skip the frame and carry on.
        (RemoteSession session, SessionLogic logic, FakeTransport transport) = NewSession();
        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));

        string? rejected = null;
        session.FrameRejected += message => rejected = message;

        Task run = session.RunAsync(cts.Token);
        transport.Enqueue("""{"v":1,"type":"hello.ack","seq":1,"session_id":"c-1","payload":{"protocol":1}}""");
        Assert.True(await WaitForAsync(() => logic.State == ClientSessionState.Connected, TimeSpan.FromSeconds(5)));

        transport.Enqueue("""{"v":1,"type":"status.snapshot",""");

        await run;

        Assert.NotNull(rejected);
        Assert.Equal(ClientSessionState.Disconnected, logic.State);
        Assert.Equal(SessionEndReason.ProtocolError, logic.EndReason);
        Assert.True(transport.CloseRequested);

        await session.DisposeAsync();
    }

    [Fact]
    public async Task TheStreamEndingReleasesEverythingAndReportsTheReason()
    {
        (RemoteSession session, SessionLogic logic, FakeTransport transport) = NewSession();
        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));

        SessionEndReason? ended = null;
        session.Ended += reason => ended = reason;

        Task run = session.RunAsync(cts.Token);
        transport.Enqueue("""{"v":1,"type":"hello.ack","seq":1,"session_id":"c-1","payload":{"protocol":1}}""");
        transport.Enqueue("""{"v":1,"type":"control.granted","seq":2,"session_id":"c-1","lease_epoch":5,"payload":{}}""");
        Assert.True(await WaitForAsync(() => logic.CanSendInput, TimeSpan.FromSeconds(5)));
        logic.QueueInput(new KeyboardKeyPayload(HidUsage.LeftAlt, true));
        Assert.Single(logic.Ledger.PressedKeys);

        transport.EndOfStream();

        await run;

        Assert.Equal(SessionEndReason.TransportClosed, ended);
        Assert.Empty(logic.Ledger.PressedKeys);

        await session.DisposeAsync();
    }

    [Fact]
    public async Task ClosingSendsTheReleaseThenSessionClose()
    {
        (RemoteSession session, SessionLogic logic, FakeTransport transport) = NewSession();
        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));

        Task run = session.RunAsync(cts.Token);
        transport.Enqueue("""{"v":1,"type":"hello.ack","seq":1,"session_id":"c-1","payload":{"protocol":1}}""");
        transport.Enqueue("""{"v":1,"type":"control.granted","seq":2,"session_id":"c-1","lease_epoch":5,"payload":{}}""");
        Assert.True(await WaitForAsync(() => logic.CanSendInput, TimeSpan.FromSeconds(5)));
        logic.QueueInput(new MouseButtonPayload(MouseButtons.Left, true));

        await session.CloseAsync(CancellationToken.None);

        IReadOnlyList<ServerMessage> sent = transport.SentDecoded;
        int closeIndex = -1;
        for (int i = 0; i < sent.Count; i++)
        {
            if (sent[i].Type == MessageTypes.SessionClose)
                closeIndex = i;
        }
        Assert.True(closeIndex > 0);
        ServerMessage release = sent[closeIndex - 1];
        Assert.Equal(MessageTypes.Input, release.Type);
        Assert.False(release.PayloadBool("down"));
        Assert.True(transport.CloseRequested);

        cts.Cancel();
        await run;
        await session.DisposeAsync();
    }
}
