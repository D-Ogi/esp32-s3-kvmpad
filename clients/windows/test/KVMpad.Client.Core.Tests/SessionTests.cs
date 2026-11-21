// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using KVMpad.Client.Core.Input;
using KVMpad.Client.Core.Protocol;
using KVMpad.Client.Core.Session;
using Xunit;

namespace KVMpad.Client.Core.Tests;

/// <summary>
/// The session state machine: sequence monotonicity, the ban on replaying a previous session's events, the
/// heartbeat deadline, and the release of every held key when the lease or the link goes away.
/// </summary>
public sealed class SessionTests
{
    [Fact]
    public async Task ConnectedHostAttachesOnceAfterHelloWithoutRequestingHidControl()
    {
        var (logic, _, _) = NewSession();
        var offered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var ready = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        int starts = 0;
        var host = new ConnectedHost(logic, async (attached, token) => {
            Interlocked.Increment(ref starts);
            Task ack = logic.AttachHostAsync(8765, [1, 2, 3], token);
            offered.SetResult();
            await ack;
            attached();
            ready.SetResult();
            await Task.Delay(Timeout.Infinite, token);
        }, () => { });
        logic.OnTransportOpened();
        await host.StartAsync();
        Assert.False(host.Running);
        logic.OnMessage(ProtocolCodec.Decode("""{"v":1,"type":"hello.ack","seq":1,"session_id":"paired","payload":{}}"""));
        await offered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Task lifetime = host.StartAsync(); // A simultaneous manual start reuses the running HOST.
        var frame = ProtocolCodec.Decode(Assert.Single(logic.OnTick()));
        Assert.Equal("host.attach", frame.Type);
        Assert.Null(frame.LeaseEpoch);
        Assert.Equal("Starting HOST", host.Status);
        logic.OnMessage(ProtocolCodec.Decode(JsonSerializer.SerializeToUtf8Bytes(new {
            v = 1, type = "host.attached", seq = 2, session_id = "paired", payload = new { ack_seq = frame.Seq } })));
        await ready.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal("HOST ready", host.Status);
        Assert.False(logic.CanSendInput);
        host.Stop();
        await lifetime.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.False(host.Running);
        Assert.Equal("HOST stopped", host.Status);
        logic.OnMessage(ProtocolCodec.Decode("""{"v":1,"type":"control.granted","seq":3,"session_id":"paired","lease_epoch":7,"payload":{}}"""));
        logic.BuildControlRelease(); // Back to Connected must respect the operator's Stop.
        Assert.Equal(ClientSessionState.Connected, logic.State);
        Assert.False(host.Running);
        Assert.Equal(1, starts);
        await host.CloseAsync();
    }

    [Fact]
    public async Task ConnectedHostFailureKeepsHidAvailableAndRetriedHostClosesWithSession()
    {
        var (logic, _, _) = NewSession();
        var failed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var retry = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var cleanup = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        int starts = 0;
        ConnectedHost? host = null;
        host = new ConnectedHost(logic, async (ready, token) => {
            if (Interlocked.Increment(ref starts) == 1) throw new HostFailure(HostFailureReason.MissingComponent);
            ready();
            retry.SetResult();
            try { await Task.Delay(Timeout.Infinite, token); }
            finally { await cleanup.Task; }
        }, () => { if (host?.Status?.StartsWith("HOST unavailable", StringComparison.Ordinal) == true) failed.TrySetResult(); });
        Handshake(logic);
        await failed.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Contains("component is missing", host.Status);
        Assert.True(logic.CanSendInput);
        logic.BuildControlRelease();
        Assert.False(host.Running);
        Task second = host.StartAsync();
        await retry.Task.WaitAsync(TimeSpan.FromSeconds(5));
        logic.OnTransportClosed();
        Task closing = host.CloseAsync();
        Assert.False(closing.IsCompleted);
        cleanup.SetResult();
        await closing.WaitAsync(TimeSpan.FromSeconds(5));
        await second;
        Assert.False(host.Running);
        Assert.Null(host.Status);
        Assert.Equal(2, starts);
        Handshake(logic, sessionId: "next"); // A retired process cannot attach to a replacement session.
        await host.StartAsync();
        Assert.False(host.Running);
        var fresh = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var (next, _, _) = NewSession();
        var nextHost = new ConnectedHost(next, (_, _) => { fresh.SetResult(); return Task.CompletedTask; }, () => { });
        Handshake(next, sessionId: "new-board");
        await fresh.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await nextHost.CloseAsync();
    }

    [Fact]
    public async Task ClosingBeforeAuthenticationOrBeforeHostReadyDiscardsLateCallbacks()
    {
        var (abandoned, _, _) = NewSession();
        int starts = 0;
        var early = new ConnectedHost(abandoned, (_, _) => { ++starts; return Task.CompletedTask; }, () => { });
        abandoned.OnTransportOpened();
        await early.CloseAsync();
        abandoned.OnMessage(ProtocolCodec.Decode("""{"v":1,"type":"hello.ack","seq":1,"session_id":"old","payload":{}}"""));
        Assert.False(early.Running);
        Assert.Null(early.Status);
        Assert.Equal(0, starts);

        var (logic, _, _) = NewSession();
        var launching = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var reported = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        ConnectedHost? host = null;
        bool showedReady = false, cancellationSeen = false;
        host = new ConnectedHost(logic, async (ready, token) => {
            launching.SetResult();
            await reported.Task;
            cancellationSeen = token.IsCancellationRequested;
            ready(); // The helper's notification was already in flight as the GUI disconnected.
        }, () => { if (host?.Status == "HOST ready") showedReady = true; });
        Handshake(logic);
        await launching.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Task closed = host.CloseAsync();
        Assert.False(closed.IsCompleted);
        reported.SetResult();
        await closed.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(cancellationSeen);
        Assert.False(showedReady);
        Assert.Null(host.Status);
    }

    [Fact]
    public async Task DisplayProfileUsesPairedUsbIdentityAndItsOwnAcknowledgment()
    {
        const string serial = "441BF6C9229C";
        var (logic, _, _) = NewSession();
        logic.OnTransportOpened();
        logic.OnMessage(ProtocolCodec.Decode("""{"v":1,"type":"hello.ack","seq":1,"session_id":"paired","payload":{"usb_serial":"441BF6C9229C","capabilities":["usb_display_profile"]}}"""));
        await Assert.ThrowsAsync<InvalidOperationException>(() => logic.RequestDisplayProfileAsync("441BF6C92290", true, default));
        var update = logic.RequestDisplayProfileAsync(serial, true, default);
        var frame = ProtocolCodec.Decode(Assert.Single(logic.OnTick()));
        Assert.Equal("display.profile.request",frame.Type);
        Assert.Equal(serial,frame.PayloadString("usb_serial"));
        Assert.Equal("1024x600",frame.PayloadString("mode"));
        void Reply(string type, string session, ulong seq) => logic.OnMessage(ProtocolCodec.Decode(
            JsonSerializer.SerializeToUtf8Bytes(new {v=1,type,seq=5,session_id=session,payload=new {ack_seq=seq}})));
        Reply("host.attached","paired",frame.Seq);
        Reply("display.profile.queued","other",frame.Seq);
        Reply("display.profile.queued","paired",frame.Seq+1);
        Assert.False(update.IsCompleted);
        Reply("display.profile.queued","paired",frame.Seq);
        await update;
    }

    [Fact]
    public async Task HostAttachUsesTheCurrentSessionAndRequiresItsOwnAcknowledgment()
    {
        var (logic, _, _) = NewSession();
        byte[] certificate = [1, 2, 3];
        await Assert.ThrowsAsync<InvalidOperationException>(() => logic.AttachHostAsync(8765, certificate, default));
        logic.OnTransportOpened();
        logic.OnMessage(ProtocolCodec.Decode("""{"v":1,"type":"hello.ack","seq":1,"session_id":"paired","payload":{}}"""));
        var attached = logic.AttachHostAsync(8765, certificate, default);
        await Assert.ThrowsAsync<InvalidOperationException>(() => logic.AttachHostAsync(8765, certificate, default));
        var frame = ProtocolCodec.Decode(Assert.Single(logic.OnTick()));
        Assert.Equal("host.attach", frame.Type);
        Assert.Equal("paired", frame.SessionId);
        Assert.Null(frame.LeaseEpoch);
        Assert.Equal(8765L, frame.PayloadInt64("port"));
        Assert.Equal("AQID", frame.PayloadString("leaf"));
        void Confirm(string id, ulong ack)
            => logic.OnMessage(ProtocolCodec.Decode(JsonSerializer.SerializeToUtf8Bytes(
                new { v = 1, type = "host.attached", seq = 3, session_id = id, payload = new { ack_seq = ack } })));
        Confirm("another", frame.Seq);
        Confirm("paired", frame.Seq + 1);
        Assert.False(attached.IsCompleted);
        Confirm("paired", frame.Seq);
        await attached;
        Assert.False(logic.CanSendInput);
        Assert.Equal(ClientSessionState.Connected, logic.State);
        var detached = logic.DetachHostAsync(default);
        var closing = ProtocolCodec.Decode(Assert.Single(logic.OnTick()));
        Assert.Equal("host.detach", closing.Type);
        Assert.Equal("{}", closing.Payload?.GetRawText());
        Confirm("paired", closing.Seq);
        Assert.False(detached.IsCompleted);
        logic.OnMessage(ProtocolCodec.Decode(JsonSerializer.SerializeToUtf8Bytes(new {
            v = 1, type = "host.detached", seq = 6, session_id = "paired", payload = new { ack_seq = closing.Seq } })));
        await detached;
        var next = logic.AttachHostAsync(8766, [7], default);
        var fresh = ProtocolCodec.Decode(Assert.Single(logic.OnTick()));
        Assert.Equal("Bw==", fresh.PayloadString("leaf"));
        Confirm("paired", fresh.Seq);
        await next;
        Assert.Equal(ClientSessionState.Connected, logic.State);
    }

    [Fact]
    public async Task HostAttachCancellationRejectionAndDisconnectEndOnlyTheirPendingRequest()
    {
        var (logic, _, _) = NewSession();
        Handshake(logic);
        using var cancel = new CancellationTokenSource();
        var abandoned = logic.AttachHostAsync(8765, [3], cancel.Token);
        cancel.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => abandoned);
        Assert.Empty(logic.OnTick());
        var denied = logic.AttachHostAsync(8765, [4], default);
        var offer = ProtocolCodec.Decode(Assert.Single(logic.OnTick()));
        logic.OnMessage(ProtocolCodec.Decode(JsonSerializer.SerializeToUtf8Bytes(new {
            v = 1, type = "error", seq = 5, reply_to_seq = offer.Seq, session_id = offer.SessionId,
            payload = new { code = "ERR_CONTROL_DENIED", retryable = true } })));
        var rejection = await Assert.ThrowsAsync<HostFailure>(() => denied);
        Assert.Contains("ERR_CONTROL_DENIED", rejection.Message);
        var lost = logic.AttachHostAsync(8765, [5], default);
        logic.OnTransportClosed();
        await Assert.ThrowsAsync<HostFailure>(() => lost);
    }

    [Fact]
    public void HostDiagnosticsUseOnlyKnownProtocolAndTransportInformation()
    {
        const string untrusted = "private-test-value";
        var rejected = new HostFailure(HostFailureReason.Rejected, untrusted);
        Assert.DoesNotContain(untrusted, HostDiagnostics.Describe(rejected));
        Assert.DoesNotContain(untrusted, HostDiagnostics.Describe(new IOException(untrusted)));
        Assert.Equal("Transfer interrupted.", HostDiagnostics.Describe(new Exception(untrusted), "Transfer interrupted."));
        Assert.Contains("ERR_SESSION", HostDiagnostics.Describe(new HostFailure(HostFailureReason.Rejected, "ERR_SESSION")));
        Assert.Contains("TCP", HostDiagnostics.Describe(new IOException(untrusted,
            new System.Net.Sockets.SocketException((int)System.Net.Sockets.SocketError.ConnectionRefused))));
        Assert.Contains("TLS", HostDiagnostics.Describe(new IOException(untrusted,
            new System.Security.Authentication.AuthenticationException(untrusted))));
        var schannel = new System.ComponentModel.Win32Exception(unchecked((int)0x8009030E), untrusted);
        Assert.Contains("SEC_E_NO_CREDENTIALS", HostDiagnostics.Describe(new System.Security.Authentication.AuthenticationException(untrusted, schannel)));
        Assert.Contains("authentication", HostDiagnostics.Describe(new System.Net.Http.HttpRequestException(untrusted, null, System.Net.HttpStatusCode.Unauthorized)));
        Assert.Contains("in time", HostDiagnostics.Describe(new TimeoutException(untrusted)));
    }

    [Fact]
    public void RefusedProxyRouteIsDistinctFromDirectOrUnrelatedFailures()
    {
        var endpoint = new Uri("wss://192.0.2.42:8443/v1/ws");
        var refused = new IOException("test", new System.Net.Sockets.SocketException(
            (int)System.Net.Sockets.SocketError.ConnectionRefused));
        var proxy = new System.Net.WebProxy("http://proxy.example:8080");
        Assert.True(HostDiagnostics.RefusedOnProxyRoute(refused, endpoint, proxy));
        Assert.False(HostDiagnostics.RefusedOnProxyRoute(refused, endpoint, new System.Net.WebProxy()));
        Assert.False(HostDiagnostics.RefusedOnProxyRoute(new IOException(), endpoint, proxy));
        proxy.BypassList = [@"192\.0\.2\.42"];
        Assert.False(HostDiagnostics.RefusedOnProxyRoute(refused, endpoint, proxy));
        Assert.Contains("proxy", HostDiagnostics.Describe(new HostFailure(HostFailureReason.ProxyRefused, inner: refused)));
    }

    private static (SessionLogic Logic, FakeClock Clock, CaptureLedger Ledger) NewSession(int budget = 100)
    {
        var clock = new FakeClock();
        var ledger = new CaptureLedger();
        var logic = new SessionLogic(
            clock,
            new SessionOptions { ClientName = "Desk1", InputMessagesPerSecond = budget },
            ledger);
        return (logic, clock, ledger);
    }

    private static void Handshake(SessionLogic logic, uint epoch = 7, string sessionId = "c-815a")
    {
        logic.OnTransportOpened();
        logic.OnMessage(ProtocolCodec.Decode(
            $@"{{""v"":1,""type"":""hello.ack"",""seq"":1,""session_id"":""{sessionId}"",""payload"":{{""protocol"":1,""device_id"":""kvm-1"",""capabilities"":[""keyboard"",""mouse""]}}}}"));
        logic.OnMessage(ProtocolCodec.Decode(
            $@"{{""v"":1,""type"":""control.granted"",""seq"":2,""session_id"":""{sessionId}"",""lease_epoch"":{epoch},""payload"":{{""expires_in_ms"":4000}}}}"));
    }

    private static ulong SeqOf(byte[] frame) => ProtocolCodec.Decode(frame).Seq;

    [Fact]
    public void TheHandshakeOpensWithHelloThenStatusGet()
    {
        (SessionLogic logic, FakeClock clock, _) = NewSession();
        IReadOnlyList<byte[]> frames = logic.OnTransportOpened();

        Assert.Single(frames);
        Assert.Equal(MessageTypes.Hello, ProtocolCodec.Decode(frames[0]).Type);
        Assert.Null(logic.BuildStatusGet());
        clock.Advance(ProtocolLimits.PingInterval);
        Assert.Empty(logic.OnTick());
        Assert.Equal(ClientSessionState.Handshaking, logic.State);
        // Not "Connected" until the device says so (AT-CONN-04).
        Assert.False(logic.CanSendInput);
        var ack = ProtocolCodec.Decode("""{"v":1,"type":"hello.ack","seq":1,"session_id":"from-board","payload":{}}""");
        var status = ProtocolCodec.Decode(Assert.Single(logic.OnMessage(ack)));
        Assert.Equal(MessageTypes.StatusGet, status.Type);
        Assert.Equal("from-board", status.SessionId);
    }

    [Fact]
    public void ASocketWithoutAnAuthenticatedSessionIsNotConnected()
    {
        (SessionLogic logic, _, _) = NewSession();
        logic.OnTransportOpened();
        Assert.NotEqual(ClientSessionState.Connected, logic.State);

        logic.OnMessage(ProtocolCodec.Decode(
            """{"v":1,"type":"hello.ack","seq":1,"session_id":"c-1","payload":{"protocol":1}}"""));
        Assert.Equal(ClientSessionState.Connected, logic.State);
        // AT-CONN-05: an authenticated session without a lease still sends no input.
        Assert.False(logic.CanSendInput);
        Assert.False(logic.QueueInput(new KeyboardKeyPayload(HidUsage.A, true)));
    }

    [Fact]
    public void SequenceNumbersIncreaseAcrossEveryFrameOfASession()
    {
        (SessionLogic logic, _, _) = NewSession();
        Handshake(logic);

        var seqs = new List<ulong>();
        foreach (byte[] frame in logic.OnTransportOpened())
            seqs.Add(SeqOf(frame));

        Handshake(logic);
        logic.QueueInput(new KeyboardKeyPayload(HidUsage.A, true));
        logic.QueueInput(new KeyboardKeyPayload(HidUsage.A, false));
        foreach (byte[] frame in logic.DrainInput())
            seqs.Add(SeqOf(frame));
        byte[]? status = logic.BuildStatusGet();
        Assert.NotNull(status);
        seqs.Add(SeqOf(status!));

        for (int i = 1; i < seqs.Count; i++)
            Assert.True(seqs[i] > seqs[i - 1], $"seq went backwards: {seqs[i - 1]} then {seqs[i]}");
    }

    [Fact]
    public void ReconnectingRestartsTheSequenceAndDiscardsTheOldQueue()
    {
        // 03 §6.4: "the application can resend an old event after a reconnect, which is forbidden."
        (SessionLogic logic, _, _) = NewSession(budget: 2);
        Handshake(logic);

        for (int i = 0; i < 10; i++)
            logic.QueueInput(new KeyboardKeyPayload(HidUsage.B, i % 2 == 0));
        IReadOnlyList<byte[]> firstBatch = logic.DrainInput();
        Assert.Equal(2, firstBatch.Count);
        Assert.True(logic.RateLimiter.PendingCount > 0);

        logic.OnTransportClosed();
        IReadOnlyList<byte[]> reopened = logic.OnTransportOpened();

        // The sequence starts again at 1 for the new session.
        Assert.Equal(1u, SeqOf(reopened[0]));
        Assert.Equal(1u, logic.Sequence.Last);

        // And the events queued under the old session are gone, not replayed.
        Assert.Equal(0, logic.RateLimiter.PendingCount);
        Handshake(logic, epoch: 9, sessionId: "c-second");
        Assert.Empty(logic.DrainInput());
    }

    [Fact]
    public void InputCarriesTheCurrentEpochAndTheDeviceSessionId()
    {
        (SessionLogic logic, _, _) = NewSession();
        Handshake(logic, epoch: 11, sessionId: "c-abc");

        logic.QueueInput(new KeyboardKeyPayload(HidUsage.Z, true));
        ServerMessage sent = ProtocolCodec.Decode(logic.DrainInput()[0]);

        Assert.Equal(MessageTypes.Input, sent.Type);
        Assert.Equal(11u, sent.LeaseEpoch);
        Assert.Equal("c-abc", sent.SessionId);
    }

    [Fact]
    public void TheHeartbeatGoesOutOnScheduleAndWellInsideTheStaleLimit()
    {
        (SessionLogic logic, FakeClock clock, _) = NewSession();
        Handshake(logic);

        Assert.Empty(logic.OnTick());

        clock.Advance(ProtocolLimits.PingInterval);
        IReadOnlyList<byte[]> frames = logic.OnTick();
        Assert.Single(frames);
        ServerMessage ping = ProtocolCodec.Decode(frames[0]);
        Assert.Equal(MessageTypes.Ping, ping.Type);
        Assert.Equal(1, ping.PayloadInt64("nonce"));

        // The device revokes a stale session after 3 s (AT-CONN-08); pinging at most every second leaves room
        // for a loss before that.
        Assert.True(ProtocolLimits.PingInterval <= TimeSpan.FromSeconds(1));
        Assert.True(ProtocolLimits.PongTimeout < TimeSpan.FromSeconds(3));
    }

    [Fact]
    public void AMissingPongEndsTheSessionBeforeTheDeviceWouldAndReleasesEverything()
    {
        (SessionLogic logic, FakeClock clock, CaptureLedger ledger) = NewSession();
        Handshake(logic);

        logic.QueueInput(new KeyboardKeyPayload(HidUsage.LeftCtrl, true));
        Assert.Single(ledger.PressedKeys);

        ReleaseAllResult? released = null;
        logic.Released += result => released = result;

        clock.Advance(ProtocolLimits.PingInterval);
        logic.OnTick(); // ping goes out
        clock.Advance(ProtocolLimits.PongTimeout);
        logic.OnTick(); // no pong ever came

        Assert.Equal(ClientSessionState.Disconnected, logic.State);
        Assert.Equal(SessionEndReason.HeartbeatStale, logic.EndReason);
        Assert.Empty(ledger.PressedKeys);
        Assert.NotNull(released);
        Assert.Equal(ReleaseReason.ConnectionLost, released!.Reason);
        // Nothing can be sent on a dead link, so the UP events are not attempted (03 §4.2).
        Assert.False(released.ShouldTransmit);
    }

    [Fact]
    public void APongKeepsTheSessionAlive()
    {
        (SessionLogic logic, FakeClock clock, _) = NewSession();
        Handshake(logic);

        for (int i = 0; i < 10; i++)
        {
            clock.Advance(ProtocolLimits.PingInterval);
            IReadOnlyList<byte[]> frames = logic.OnTick();
            if (frames.Count == 1)
            {
                long nonce = ProtocolCodec.Decode(frames[0]).PayloadInt64("nonce") ?? 0;
                logic.OnMessage(ProtocolCodec.Decode(
                    $@"{{""v"":1,""type"":""pong"",""seq"":{i + 10},""session_id"":""c-815a"",""payload"":{{""nonce"":{nonce}}}}}"));
            }
        }

        Assert.Equal(ClientSessionState.Controlling, logic.State);
    }

    [Fact]
    public void ControlRevokedDropsTheEpochAndReleasesWithoutSendingInput()
    {
        // VEC-009 / AT-CONN-07: a local takeover on the device revokes the remote lease mid-drag.
        (SessionLogic logic, _, CaptureLedger ledger) = NewSession();
        Handshake(logic);

        logic.QueueInput(new KeyboardKeyPayload(HidUsage.LeftCtrl, true));
        logic.QueueInput(new MouseButtonPayload(MouseButtons.Left, true));
        Assert.Single(ledger.PressedKeys);
        Assert.Single(ledger.PressedButtons);

        ReleaseAllResult? released = null;
        logic.Released += result => released = result;

        logic.OnMessage(ProtocolCodec.Decode(
            """{"v":1,"type":"control.revoked","seq":4,"session_id":"c-815a","payload":{"reason":"local_override","new_epoch":8}}"""));

        Assert.Equal(ClientSessionState.Connected, logic.State);
        Assert.Null(logic.LeaseEpoch);
        Assert.False(logic.CanSendInput);
        Assert.Empty(ledger.PressedKeys);
        Assert.Empty(ledger.PressedButtons);
        Assert.Equal(ReleaseReason.ControlRevoked, released!.Reason);
        Assert.False(released.ShouldTransmit);

        // And no further input is accepted with a stale epoch.
        Assert.False(logic.QueueInput(new KeyboardKeyPayload(HidUsage.A, true)));
        Assert.Empty(logic.DrainInput());
    }

    [Fact]
    public void ErrStaleEpochStopsInputAndClearsTheLedger()
    {
        // AT-SEC-03 from the client's side: after a rejection the client must stop sending, not keep retrying.
        (SessionLogic logic, _, CaptureLedger ledger) = NewSession();
        Handshake(logic);
        logic.QueueInput(new KeyboardKeyPayload(HidUsage.LeftShift, true));

        logic.OnMessage(ProtocolCodec.Decode(
            """{"v":1,"type":"error","seq":5,"session_id":"c-815a","payload":{"code":"ERR_STALE_EPOCH","retryable":false}}"""));

        Assert.Equal(ErrorCodes.StaleEpoch, logic.LastErrorCode);
        Assert.Null(logic.LeaseEpoch);
        Assert.Empty(ledger.PressedKeys);
        Assert.False(logic.CanSendInput);
    }

    [Theory]
    [InlineData(ErrorCodes.HostOffline)]
    [InlineData(ErrorCodes.UsbSuspended)]
    [InlineData(ErrorCodes.ClientStale)]
    [InlineData(ErrorCodes.HidQueueOverflow)]
    [InlineData(ErrorCodes.OtaInProgress)]
    public void EveryErrorThatMeansNoHidAlsoMeansReleaseEverything(string code)
    {
        (SessionLogic logic, _, CaptureLedger ledger) = NewSession();
        Handshake(logic);
        logic.QueueInput(new KeyboardKeyPayload(HidUsage.LeftAlt, true));

        logic.OnMessage(ProtocolCodec.Decode(
            $@"{{""v"":1,""type"":""error"",""seq"":9,""session_id"":""c-815a"",""payload"":{{""code"":""{code}"",""retryable"":false}}}}"));

        Assert.Empty(ledger.PressedKeys);
        Assert.False(logic.CanSendInput);
    }

    [Fact]
    public void AFatalProtocolErrorEndsTheConnection()
    {
        (SessionLogic logic, _, _) = NewSession();
        Handshake(logic);

        logic.OnMessage(ProtocolCodec.Decode(
            """{"v":1,"type":"error","seq":6,"session_id":"c-815a","payload":{"code":"ERR_VERSION","retryable":false}}"""));

        Assert.Equal(ClientSessionState.Disconnected, logic.State);
        Assert.Equal(SessionEndReason.ProtocolError, logic.EndReason);
    }

    [Fact]
    public void AnUnknownEnvelopeTypeIsRecordedNotIgnored()
    {
        // 03 §6.2: "no silent ignoring of unknown names."
        (SessionLogic logic, _, _) = NewSession();
        Handshake(logic);

        string? reported = null;
        logic.ErrorReceived += (code, _) => reported = code;

        logic.OnMessage(ProtocolCodec.Decode(
            """{"v":1,"type":"device.firmware.push","seq":7,"session_id":"c-815a","payload":{}}"""));

        Assert.Contains("ERR_UNKNOWN_TYPE", reported);
        Assert.Contains("device.firmware.push", logic.LastErrorCode);
    }

    [Fact]
    public void ReleasingControlSendsTheKeyUpsBeforeGivingUpTheLease()
    {
        (SessionLogic logic, _, CaptureLedger ledger) = NewSession();
        Handshake(logic);
        logic.QueueInput(new KeyboardKeyPayload(HidUsage.LeftCtrl, true));
        logic.QueueInput(new KeyboardKeyPayload(HidUsage.C, true));

        IReadOnlyList<byte[]> frames = logic.BuildControlRelease();

        // Two key UPs, then control.release — in that order, so the host never holds a key past the lease.
        Assert.Equal(3, frames.Count);
        ServerMessage first = ProtocolCodec.Decode(frames[0]);
        ServerMessage second = ProtocolCodec.Decode(frames[1]);
        ServerMessage third = ProtocolCodec.Decode(frames[2]);
        Assert.Equal(MessageTypes.Input, first.Type);
        Assert.Equal(MessageTypes.Input, second.Type);
        Assert.Equal(MessageTypes.ControlRelease, third.Type);
        Assert.False(first.PayloadBool("down"));
        Assert.False(second.PayloadBool("down"));
        // Reverse press order: the C key goes up before the modifier (02 §7.3).
        Assert.Equal(HidUsage.C, (byte)first.PayloadInt64("usage_id")!.Value);
        Assert.Equal(HidUsage.LeftCtrl, (byte)second.PayloadInt64("usage_id")!.Value);
        Assert.Empty(ledger.PressedKeys);
        Assert.Null(logic.LeaseEpoch);
    }

    [Fact]
    public void ControlApprovalTimesOutAfterThirtySeconds()
    {
        // 03 §6.5: control.request may wait 30 s for physical confirmation; timeout means reject.
        (SessionLogic logic, FakeClock clock, _) = NewSession();
        logic.OnTransportOpened();
        logic.OnMessage(ProtocolCodec.Decode(
            """{"v":1,"type":"hello.ack","seq":1,"session_id":"c-1","payload":{"protocol":1}}"""));
        Assert.NotNull(logic.BuildControlRequest());
        logic.OnMessage(ProtocolCodec.Decode(
            """{"v":1,"type":"control.pending","seq":2,"session_id":"c-1","payload":{"approval_required":true,"expires_in_s":30}}"""));
        Assert.Equal(ClientSessionState.ControlPending, logic.State);

        clock.Advance(ProtocolLimits.ControlApprovalTimeout + TimeSpan.FromSeconds(1));
        logic.OnTick();

        Assert.Equal(ClientSessionState.Connected, logic.State);
        Assert.Equal(ErrorCodes.ControlDenied, logic.LastErrorCode);
    }

    [Fact]
    public void SessionCloseReleasesFirst()
    {
        (SessionLogic logic, _, _) = NewSession();
        Handshake(logic);
        logic.QueueInput(new MouseButtonPayload(MouseButtons.Left, true));

        IReadOnlyList<byte[]> frames = logic.BuildSessionClose();

        Assert.Equal(2, frames.Count);
        Assert.Equal(MessageTypes.Input, ProtocolCodec.Decode(frames[0]).Type);
        Assert.Equal(MessageTypes.SessionClose, ProtocolCodec.Decode(frames[1]).Type);
    }

    [Fact]
    public void TheSnapshotDrivesWhetherCaptureMayStart()
    {
        (SessionLogic logic, _, _) = NewSession();
        Handshake(logic);
        logic.OnMessage(ProtocolCodec.Decode(
            """{"v":1,"type":"status.snapshot","seq":8,"session_id":"c-815a","payload":{"host":{"state":"USB_UNMOUNTED"},"client":{"state":"CLIENT_CONNECTED"},"control":{"owner":"REMOTE","lease_epoch":7,"approval":"auto"},"device":{"wifi":"CONNECTED"}}}"""));

        Assert.NotNull(logic.Snapshot);
        Assert.False(logic.Snapshot!.IsHostMounted);
        // ADR-023 FR-735: the relaxed policy is visible to the user.
        Assert.Equal("auto", logic.Snapshot.ControlApproval);
    }
}
