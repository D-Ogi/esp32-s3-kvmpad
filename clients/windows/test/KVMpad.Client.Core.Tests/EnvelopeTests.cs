// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Text;
using KVMpad.Client.Core.Input;
using KVMpad.Client.Core.Protocol;
using Xunit;

namespace KVMpad.Client.Core.Tests;

/// <summary>
/// Envelope serialization against the literal examples printed in spec 03 §6.2 and §6.4, plus the validation
/// limits of §6.5. The spec's examples are the contract; matching them character for character is the cheapest
/// proof that this client and the firmware will agree.
/// </summary>
public sealed class EnvelopeTests
{
    [Fact]
    public void MouseMoveEnvelopeMatchesTheSpecExample()
    {
        // 03 §6.2, verbatim.
        const string expected =
            """{"v":1,"type":"input","seq":203,"session_id":"c-815a","lease_epoch":7,"payload":{"kind":"mouse.move","dx":14,"dy":-5}}""";

        string actual = ProtocolCodec.EncodeToString(
            MessageTypes.Input, 203, "c-815a", 7, new MouseMovePayload(14, -5));

        Assert.Equal(expected, actual);
    }

    [Fact]
    public void HelloEnvelopeCarriesNoLeaseEpochAndNoToken()
    {
        string actual = ProtocolCodec.EncodeToString(
            MessageTypes.Hello, 1, null, null, new HelloPayload("Desk1", ["hid_v1"]));

        Assert.Equal(
            """{"v":1,"type":"hello","seq":1,"payload":{"client_name":"Desk1","capabilities":["hid_v1"]}}""",
            actual);
        // The token is a handshake credential, never payload (03 §6.1, ADR-028).
        Assert.DoesNotContain("token", actual, StringComparison.OrdinalIgnoreCase);
    }

    [Theory]
    [InlineData(InputKinds.KeyboardKey)]
    [InlineData(InputKinds.MouseButton)]
    [InlineData(InputKinds.MouseWheel)]
    [InlineData(InputKinds.Shortcut)]
    public void EveryInputKindSerializesWithItsKindField(string kind)
    {
        IInputPayload payload = kind switch
        {
            InputKinds.KeyboardKey => new KeyboardKeyPayload(HidUsage.A, true),
            InputKinds.MouseButton => new MouseButtonPayload(MouseButtons.Left, true),
            InputKinds.MouseWheel => new MouseWheelPayload(3, -1),
            _ => new ShortcutPayload(ChordIds.CtrlAltDel),
        };

        string json = ProtocolCodec.EncodeToString(MessageTypes.Input, 5, "c-1", 2, payload);
        Assert.Contains($"\"kind\":\"{kind}\"", json);
    }

    [Fact]
    public void KeyboardKeyCarriesTheUsageAsANumberNotACharacter()
    {
        string json = ProtocolCodec.EncodeToString(
            MessageTypes.Input, 1, "c-1", 1, new KeyboardKeyPayload(HidUsage.A, true));

        // 0x04, the usage for the A key — not 65, the ASCII code point (03 §5.2 "do not assign ASCII('A')").
        Assert.Contains("\"usage_id\":4", json);
        Assert.DoesNotContain("\"usage_id\":65", json);
    }

    [Fact]
    public void ServerMessagesFromTheSpecDecodeIntoTheirFields()
    {
        // 03 §6.4, verbatim.
        ServerMessage helloAck = ProtocolCodec.Decode(
            """{"v":1,"type":"hello.ack","seq":1,"session_id":"c-815a","payload":{"protocol":1,"device_id":"kvm-xxxx","capabilities":["keyboard","mouse","shortcuts"],"host":"USB_MOUNTED"}}""");
        Assert.Equal(MessageTypes.HelloAck, helloAck.Type);
        Assert.Equal("c-815a", helloAck.SessionId);
        Assert.Equal("kvm-xxxx", helloAck.PayloadString("device_id"));

        ServerMessage pending = ProtocolCodec.Decode(
            """{"v":1,"type":"control.pending","seq":2,"session_id":"c-815a","payload":{"approval_required":true,"expires_in_s":30}}""");
        Assert.True(pending.PayloadBool("approval_required"));
        Assert.Equal(30, pending.PayloadInt64("expires_in_s"));

        ServerMessage granted = ProtocolCodec.Decode(
            """{"v":1,"type":"control.granted","seq":3,"session_id":"c-815a","lease_epoch":7,"payload":{"expires_in_ms":4000}}""");
        Assert.Equal(7u, granted.LeaseEpoch);

        ServerMessage revoked = ProtocolCodec.Decode(
            """{"v":1,"type":"control.revoked","seq":4,"session_id":"c-815a","payload":{"reason":"local_override","new_epoch":8}}""");
        Assert.Equal("local_override", revoked.PayloadString("reason"));

        ServerMessage error = ProtocolCodec.Decode(
            """{"v":1,"type":"error","seq":5,"session_id":"c-815a","payload":{"code":"ERR_STALE_EPOCH","retryable":false}}""");
        Assert.Equal(ErrorCodes.StaleEpoch, error.ErrorCode);
        Assert.False(error.ErrorRetryable);
    }

    [Fact]
    public void SnapshotDecodesIntoTheStateTheUiShows()
    {
        // 03 §6.6, verbatim.
        ServerMessage message = ProtocolCodec.Decode(
            """{"v":1,"type":"status.snapshot","seq":16,"session_id":"c-815a","payload":{"host":{"state":"USB_MOUNTED","transport":"usb_hid"},"client":{"state":"CLIENT_CONNECTED","role":"operator"},"control":{"owner":"REMOTE","lease_epoch":7},"device":{"wifi":"CONNECTED","maintenance":false,"fw":"1.0.0"}}}""");

        StatusSnapshot snapshot = StatusSnapshot.Parse(message);
        Assert.True(snapshot.IsHostMounted);
        Assert.True(snapshot.IsClientConnected);
        Assert.True(snapshot.IsRemoteOwner);
        Assert.Equal(7u, snapshot.ControlLeaseEpoch);
        Assert.Equal("CONNECTED", snapshot.DeviceWifi);
        Assert.Equal("1.0.0", snapshot.DeviceFirmware);
        Assert.False(snapshot.DeviceMaintenance);
    }

    [Fact]
    public void WifiConnectedDoesNotMakeTheClientConnected()
    {
        // AT-CONN-04: wifi:CONNECTED is a separate state and must never read as CLIENT_CONNECTED (03 §6.6).
        ServerMessage message = ProtocolCodec.Decode(
            """{"v":1,"type":"status.snapshot","seq":2,"session_id":"c-1","payload":{"host":{"state":"USB_MOUNTED"},"client":{"state":"CLIENT_DISCONNECTED"},"control":{"owner":"NONE"},"device":{"wifi":"CONNECTED"}}}""");

        StatusSnapshot snapshot = StatusSnapshot.Parse(message);
        Assert.Equal("CONNECTED", snapshot.DeviceWifi);
        Assert.False(snapshot.IsClientConnected);
        Assert.False(snapshot.IsRemoteOwner);
    }

    [Fact]
    public void AFrameOverTheLimitIsRefusedRatherThanTruncated()
    {
        // 03 §6.1: max_frame_size = 1024 B. The client refuses to build one instead of being disconnected
        // with ERR_BAD_FRAME for it.
        var longName = new string('n', 1100);
        ProtocolFrameException ex = Assert.Throws<ProtocolFrameException>(() =>
            ProtocolCodec.Encode(MessageTypes.Hello, 1, null, null, new HelloPayload(longName, ["hid_v1"])));
        Assert.Contains("max_frame_size", ex.Message);
    }

    [Fact]
    public void AnInboundFrameOverTheLimitIsRejected()
    {
        byte[] oversized = Encoding.UTF8.GetBytes(
            """{"v":1,"type":"status.snapshot","seq":1,"payload":{"pad":" """ + new string('x', 1100) + """ "}}""");
        Assert.Throws<ProtocolFrameException>(() => ProtocolCodec.Decode(oversized));
    }

    [Theory]
    [InlineData("""{"v":2,"type":"hello.ack","seq":1}""")]
    [InlineData("""{"type":"hello.ack","seq":1}""")]
    [InlineData("""{"v":1,"seq":1}""")]
    [InlineData("""{"v":1,"type":"hello.ack",""")]
    [InlineData("[]")]
    [InlineData("")]
    public void AMalformedOrWrongVersionFrameFailsClosed(string json)
    {
        // 03 §6.5 and AT-SEC-04: the parser fails closed; nothing is guessed at and nothing crashes.
        Assert.Throws<ProtocolFrameException>(() => ProtocolCodec.Decode(json));
    }

    [Fact]
    public void DeeplyNestedJsonIsRejected()
    {
        // AT-SEC-04 names "large depth" explicitly.
        var json = new StringBuilder("""{"v":1,"type":"status.snapshot","seq":1,"payload":""");
        for (int i = 0; i < 30; i++)
            json.Append("{\"a\":");
        json.Append("1");
        for (int i = 0; i < 30; i++)
            json.Append('}');
        json.Append('}');

        Assert.Throws<ProtocolFrameException>(() => ProtocolCodec.Decode(json.ToString()));
    }

    [Fact]
    public void AnOutOfRangeMouseDeltaIsRefusedAtConstruction()
    {
        // 03 §6.5 forbids hidden clipping; the payload type refuses the value so the caller must split it.
        Assert.Throws<ArgumentOutOfRangeException>(() => new MouseMovePayload(4097, 0));
        Assert.Throws<ArgumentOutOfRangeException>(() => new MouseMovePayload(0, -4097));
        _ = new MouseMovePayload(4096, -4096);
    }

    [Fact]
    public void AnUnknownUsageOrChordIsRefusedAtConstruction()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => new KeyboardKeyPayload(0x00, true));
        Assert.Throws<ArgumentOutOfRangeException>(() => new KeyboardKeyPayload(0xB0, true));
        Assert.Throws<ArgumentOutOfRangeException>(() => new ShortcutPayload("rm_minus_rf"));
        Assert.Throws<ArgumentOutOfRangeException>(() => new MouseButtonPayload("x1", true));
    }
}
