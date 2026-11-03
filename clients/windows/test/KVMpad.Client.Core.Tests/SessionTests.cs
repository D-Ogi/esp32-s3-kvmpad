// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.Text;
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
        (SessionLogic logic, _, _) = NewSession();
        IReadOnlyList<byte[]> frames = logic.OnTransportOpened();

        Assert.Equal(2, frames.Count);
        Assert.Equal(MessageTypes.Hello, ProtocolCodec.Decode(frames[0]).Type);
        Assert.Equal(MessageTypes.StatusGet, ProtocolCodec.Decode(frames[1]).Type);
        Assert.Equal(ClientSessionState.Handshaking, logic.State);
        // Not "Connected" until the device says so (AT-CONN-04).
        Assert.False(logic.CanSendInput);
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
        Assert.Equal(0u, logic.Sequence.Last - 2);

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
