// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using KVMpad.Client.Core.Pairing;
using KVMpad.Client.Core.Protocol;
using Xunit;

namespace KVMpad.Client.Core.Tests;

/// <summary>
/// The pairing driver: the code the user types, the context, and the <c>pair.*</c> exchange of ADR-029 §4 in
/// the order and with the field names that decision fixes. The EC-JPAKE itself is not exercised here — it is
/// the pairing core's, and <see cref="NativePairingTests"/> drives the real thing against the interop
/// vectors. What these tests pin down is that the driver speaks the right protocol and refuses everything
/// else.
/// </summary>
public sealed class PairingTests
{
    private const string Fingerprint = "9f2c1a04b7e85d3610fa4c92db7e05183c6a4f20e9b1d74c852036af1b9e4d7c";
    private const string ClientId = "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf";

    private static PairingContext NewContext(string code = "482913", string name = "Desk1")
        => new(code, name, ClientId, Fingerprint);

    private static string Frame(string type, string? payloadFields = null, int seq = 1)
        => $@"{{""v"":1,""type"":""{type}"",""seq"":{seq},""payload"":{{{payloadFields ?? string.Empty}}}}}";

    private static string Blob(string type, string text, int seq = 1)
        => Frame(type, $@"""blob"":""{Convert.ToBase64String(Encoding.ASCII.GetBytes(text))}""", seq);

    private static string Mac(string text, int seq = 1)
        => Frame(MessageTypes.PairConfirm, $@"""mac"":""{Convert.ToBase64String(Encoding.ASCII.GetBytes(text))}""", seq);

    // ---- the code -------------------------------------------------------------------------------

    [Theory]
    [InlineData("482913", true)]
    [InlineData("000000", true)]
    [InlineData("999999", true)]
    [InlineData("48291", false)]
    [InlineData("4829134", false)]
    [InlineData("48291a", false)]
    [InlineData("", false)]
    [InlineData(null, false)]
    public void OnlySixDigitsIsAValidCode(string? code, bool expected)
        => Assert.Equal(expected, PairingCode.IsValid(code));

    [Theory]
    [InlineData("482 913", "482913")]
    [InlineData("482-913", "482913")]
    [InlineData(" 482913 ", "482913")]
    [InlineData("abc", "")]
    public void TheCodeTheUserTypesIsNormalizedBeforeValidation(string input, string expected)
        => Assert.Equal(expected, PairingCode.Normalize(input));

    [Fact]
    public void TheCodeIsFormattedTheWayTheDeviceShowsIt()
        => Assert.Equal("482 913", PairingCode.Format("482913"));

    // ---- the context ----------------------------------------------------------------------------

    [Fact]
    public void AContextCannotExistWithoutACodeACertificateHashAndAWellFormedClientId()
    {
        Assert.Throws<ArgumentException>(() => new PairingContext("12345", "Desk1", ClientId, Fingerprint));
        Assert.Throws<ArgumentException>(() => new PairingContext("482913", "Desk1", ClientId, "short"));
        Assert.Throws<ArgumentException>(() => new PairingContext("482913", "", ClientId, Fingerprint));
        // ADR-029 §4 makes the client identifier 16 bytes on the wire, so a free-form string is not one.
        Assert.Throws<ArgumentException>(() => new PairingContext("482913", "Desk1", "client-a", Fingerprint));
        Assert.Throws<ArgumentException>(() => new PairingContext("482913", "Desk1", new string('z', 32), Fingerprint));
    }

    [Fact]
    public void TheContextKeepsTheWireFormsAndTheRawBytesInStep()
    {
        PairingContext context = NewContext();

        Assert.Equal(ClientId, context.ClientId);
        Assert.Equal(Fingerprint, context.DeviceCertificateSha256);
        Assert.Equal(16, context.ClientIdValue.Length);
        Assert.Equal(32, context.CertificateSha256Value.Length);
        Assert.Equal(ClientId, Convert.ToHexStringLower(context.ClientIdValue));
        Assert.Equal(Fingerprint, Convert.ToHexStringLower(context.CertificateSha256Value));
    }

    [Fact]
    public void AnUppercaseFingerprintIsAcceptedAndNormalized()
    {
        var context = new PairingContext("482913", "Desk1", ClientId.ToUpperInvariant(), Fingerprint.ToUpperInvariant());

        Assert.Equal(Fingerprint, context.DeviceCertificateSha256);
        Assert.Equal(ClientId, context.ClientId);
    }

    [Fact]
    public void ClearingTheContextRemovesTheCode()
    {
        PairingContext context = NewContext();
        Assert.Equal("482913", context.Code);

        context.Clear();

        Assert.Equal(string.Empty, context.Code);
    }

    // ---- the exchange ---------------------------------------------------------------------------

    /// <summary>Queues the device's half of a successful run, in the order of ADR-029 §4.</summary>
    private static void QueueSuccess(FakeTransport transport, string sealedToken)
    {
        transport.Enqueue(Frame(MessageTypes.PairRound1,
            $@"""blob"":""{Convert.ToBase64String(Encoding.ASCII.GetBytes("device-round-1"))}"",""device_name"":""KVMpad-7F3A""", 1));
        transport.Enqueue(Blob(MessageTypes.PairRound2, "device-round-2", 2));
        transport.Enqueue(Mac("device-confirm", 3));
        transport.Enqueue(Frame(MessageTypes.PairAccepted,
            $@"""token_enc"":""{sealedToken}"",""device_name"":""KVMpad-7F3A""", 4));
    }

    [Fact]
    public async Task AFullRunFollowsTheAdr029OrderAndYieldsAPinnedToken()
    {
        var transport = new FakeTransport();
        await transport.ConnectAsync(CancellationToken.None);
        using var pake = new FakePakeHandshake();
        byte[] token = Encoding.ASCII.GetBytes("TTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTT");
        string expectedToken = PairingClient.Base64Url(token);
        pake.TokenToReturn = token;
        string sealedToken = Convert.ToBase64String(new byte[48]);
        QueueSuccess(transport, sealedToken);

        PairingContext context = NewContext();
        var client = new PairingClient(transport, pake);

        PairingResult result = await client.RunAsync(context, CancellationToken.None);

        Assert.Equal("KVMpad-7F3A", result.DeviceName);
        Assert.Equal(Fingerprint, result.PinnedFingerprint);
        Assert.Equal(expectedToken, result.Token);

        // SEC-105: the raw token buffer the core handed over is wiped once it has been encoded, so the only
        // copy that outlives the run is the one on its way to the credential store. `token` is that very
        // buffer, which is why it reads as zeros here.
        Assert.Equal(new byte[32], token);

        // The client sent exactly four frames, in the order the decision fixes, and nothing else.
        IReadOnlyList<ServerMessage> sent = transport.SentDecoded;
        Assert.Equal(4, sent.Count);
        Assert.Equal(MessageTypes.PairHello, sent[0].Type);
        Assert.Equal(MessageTypes.PairRound1, sent[1].Type);
        Assert.Equal(MessageTypes.PairRound2, sent[2].Type);
        Assert.Equal(MessageTypes.PairConfirm, sent[3].Type);

        // The rounds carry `blob` and the confirmation carries `mac` (ADR-029 §4), not one shared field name.
        Assert.NotNull(sent[1].PayloadString("blob"));
        Assert.NotNull(sent[2].PayloadString("blob"));
        Assert.NotNull(sent[3].PayloadString("mac"));
        Assert.Null(sent[3].PayloadString("blob"));

        // The device spoke first after hello: the client consumed the device's round 1 before sending its own.
        Assert.Equal("device-round-1", Encoding.ASCII.GetString(pake.Consumed[0]));
        Assert.Equal("device-round-2", Encoding.ASCII.GetString(pake.Consumed[1]));
        Assert.Equal("device-confirm", Encoding.ASCII.GetString(pake.Consumed[2]));

        // The sealed token reached the core as the 48 raw bytes, not as base64.
        Assert.Equal(48, pake.SealedTokenSeen!.Length);

        // SEC-105: the code never appears on the wire in any form, and is gone from memory afterwards.
        foreach (string text in transport.SentText)
            Assert.DoesNotContain("482913", text);
        Assert.Equal(string.Empty, context.Code);
    }

    [Fact]
    public async Task PairHelloCarriesTheAdr029FieldsAndNoSecret()
    {
        var transport = new FakeTransport();
        await transport.ConnectAsync(CancellationToken.None);
        using var pake = new FakePakeHandshake();
        transport.Enqueue(Frame(MessageTypes.PairRejected, @"""reason"":""timeout"""));

        var client = new PairingClient(transport, pake);
        await Assert.ThrowsAsync<PairingFailedException>(() => client.RunAsync(NewContext(), CancellationToken.None));

        ServerMessage hello = ProtocolCodec.Decode(transport.Sent[0]);
        Assert.Equal(1, hello.PayloadInt64("v"));
        Assert.Equal("test-fake", hello.PayloadString("pake"));
        Assert.Equal("Desk1", hello.PayloadString("client_name"));
        Assert.Equal(ClientId, hello.PayloadString("client_id"));
        Assert.Equal(Fingerprint, hello.PayloadString("cert_sha256"));
        // No token and no code: the code only ever enters the password derivation (SEC-105).
        Assert.Null(hello.PayloadString("token"));
        Assert.Null(hello.PayloadString("code"));
    }

    [Fact]
    public async Task PairHelloReportsTheTruncatedNameThatEnteredTheBinding()
    {
        // The wire caps the client name, and the binding commits to the truncated form. Sending the original
        // would make the device derive a different password and fail at confirmation for no visible reason.
        var transport = new FakeTransport();
        await transport.ConnectAsync(CancellationToken.None);
        using var pake = new FakePakeHandshake();
        transport.Enqueue(Frame(MessageTypes.PairRejected, @"""reason"":""timeout"""));

        var context = new PairingContext("482913", new string('W', 40), ClientId, Fingerprint);
        var client = new PairingClient(transport, pake);
        await Assert.ThrowsAsync<PairingFailedException>(() => client.RunAsync(context, CancellationToken.None));

        string? name = ProtocolCodec.Decode(transport.Sent[0]).PayloadString("client_name");
        Assert.Equal(new string('W', 23), name);
        Assert.Equal(name, context.EffectiveClientName);
    }

    [Theory]
    [InlineData("timeout", PairingRejectionReason.Timeout)]
    [InlineData("local_reject", PairingRejectionReason.LocalReject)]
    [InlineData("locked", PairingRejectionReason.Locked)]
    [InlineData("something_else", PairingRejectionReason.Unknown)]
    public async Task ARejectionIsReportedWithoutStoringAnything(string reason, PairingRejectionReason expected)
    {
        var transport = new FakeTransport();
        await transport.ConnectAsync(CancellationToken.None);
        using var pake = new FakePakeHandshake();
        transport.Enqueue(Frame(MessageTypes.PairRejected, $@"""reason"":""{reason}"""));

        var client = new PairingClient(transport, pake);

        PairingFailedException ex = await Assert.ThrowsAsync<PairingFailedException>(
            () => client.RunAsync(NewContext(), CancellationToken.None));

        Assert.Equal(expected, ex.Reason);
    }

    [Fact]
    public async Task ARejectionAtAnyStepEndsTheRun()
    {
        // ADR-029 §4 allows pair.rejected in place of any device message, so every wait has to accept it.
        var transport = new FakeTransport();
        await transport.ConnectAsync(CancellationToken.None);
        using var pake = new FakePakeHandshake();
        transport.Enqueue(Blob(MessageTypes.PairRound1, "device-round-1", 1));
        transport.Enqueue(Frame(MessageTypes.PairRejected, @"""reason"":""local_reject""", 2));

        var client = new PairingClient(transport, pake);

        PairingFailedException ex = await Assert.ThrowsAsync<PairingFailedException>(
            () => client.RunAsync(NewContext(), CancellationToken.None));

        Assert.Equal(PairingRejectionReason.LocalReject, ex.Reason);
    }

    [Fact]
    public async Task AFailedKeyConfirmationAbortsWithoutRevealingWhichHalfWasWrong()
    {
        // SEC-104 counts this as one of three attempts; the message must not say whether the code or the
        // certificate was the problem.
        var transport = new FakeTransport();
        await transport.ConnectAsync(CancellationToken.None);
        using var pake = new FakePakeHandshake
        {
            DeviceConfirmationToAccept = Encoding.ASCII.GetBytes("right"),
        };
        transport.Enqueue(Blob(MessageTypes.PairRound1, "device-round-1", 1));
        transport.Enqueue(Blob(MessageTypes.PairRound2, "device-round-2", 2));
        transport.Enqueue(Mac("wrong", 3));

        var client = new PairingClient(transport, pake);

        PairingFailedException ex = await Assert.ThrowsAsync<PairingFailedException>(
            () => client.RunAsync(NewContext(), CancellationToken.None));

        Assert.Equal(PairingRejectionReason.CodeMismatch, ex.Reason);
        Assert.DoesNotContain("certificate", ex.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task AnExpiredPairingWindowIsReportedAsSuch()
    {
        var transport = new FakeTransport();
        await transport.ConnectAsync(CancellationToken.None);
        using var pake = new FakePakeHandshake();
        transport.Enqueue(
            """{"v":1,"type":"error","seq":2,"payload":{"code":"ERR_PAIRING_EXPIRED","retryable":false}}""");

        var client = new PairingClient(transport, pake);

        PairingFailedException ex = await Assert.ThrowsAsync<PairingFailedException>(
            () => client.RunAsync(NewContext(), CancellationToken.None));

        Assert.Equal(PairingRejectionReason.Expired, ex.Reason);
    }

    [Fact]
    public async Task ARunThatOutlivesTheWindowStops()
    {
        // FR-722: the whole window, including the Accept screen, is at most 180 s. A device that simply stops
        // answering must not leave the attempt open — which is what a window checked only between frames
        // would do. The window here is shortened to keep the test quick; the default is asserted below.
        var transport = new FakeTransport();
        await transport.ConnectAsync(CancellationToken.None);
        using var pake = new FakePakeHandshake();
        var client = new PairingClient(transport, pake, TimeSpan.FromMilliseconds(150));

        // Nothing is queued: the device answers pair.hello with silence.
        PairingFailedException ex = await Assert.ThrowsAsync<PairingFailedException>(
            () => client.RunAsync(NewContext(), CancellationToken.None));

        Assert.Equal(PairingRejectionReason.Expired, ex.Reason);
        Assert.DoesNotContain("482913", ex.Message);

        Assert.Equal(TimeSpan.FromSeconds(180), ProtocolLimits.PairingWindow);
        Assert.Equal(TimeSpan.FromSeconds(180), new PairingClient(transport, pake).Window);
        Assert.Throws<ArgumentOutOfRangeException>(() => new PairingClient(transport, pake, TimeSpan.Zero));
    }

    [Fact]
    public async Task CancellingTheAttemptIsNotReportedAsAnExpiry()
    {
        // Closing the dialog is the user's doing, not a timeout, and the two must not be confused in the UI.
        var transport = new FakeTransport();
        await transport.ConnectAsync(CancellationToken.None);
        using var pake = new FakePakeHandshake();
        var client = new PairingClient(transport, pake, TimeSpan.FromSeconds(30));
        using var cts = new CancellationTokenSource();

        Task<PairingResult> run = client.RunAsync(NewContext(), cts.Token);
        cts.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => run);
    }

    [Fact]
    public async Task AFrameOutOfOrderIsRefusedRatherThanInterpreted()
    {
        // The device answers hello with round 2. A driver that shrugged and carried on would be hashing the
        // wrong blob into the transcript.
        var transport = new FakeTransport();
        await transport.ConnectAsync(CancellationToken.None);
        using var pake = new FakePakeHandshake();
        transport.Enqueue(Blob(MessageTypes.PairRound2, "device-round-2", 1));

        var client = new PairingClient(transport, pake);

        PairingFailedException ex = await Assert.ThrowsAsync<PairingFailedException>(
            () => client.RunAsync(NewContext(), CancellationToken.None));

        Assert.Contains("pair.round1", ex.Message);
        Assert.Empty(pake.Consumed);
    }

    [Theory]
    [InlineData(MessageTypes.PairRound1, "", "no 'blob'")]
    [InlineData(MessageTypes.PairRound1, @"""blob"":""not base64 !""", "malformed 'blob'")]
    public async Task AMissingOrMalformedBlobFailsClosed(string type, string payload, string expected)
    {
        var transport = new FakeTransport();
        await transport.ConnectAsync(CancellationToken.None);
        using var pake = new FakePakeHandshake();
        transport.Enqueue(Frame(type, payload));

        var client = new PairingClient(transport, pake);

        PairingFailedException ex = await Assert.ThrowsAsync<PairingFailedException>(
            () => client.RunAsync(NewContext(), CancellationToken.None));

        Assert.Contains(expected, ex.Message);
        Assert.Empty(pake.Consumed);
    }

    [Fact]
    public async Task AnAcceptedFrameWithoutATokenIsRefused()
    {
        var transport = new FakeTransport();
        await transport.ConnectAsync(CancellationToken.None);
        using var pake = new FakePakeHandshake();
        transport.Enqueue(Blob(MessageTypes.PairRound1, "device-round-1", 1));
        transport.Enqueue(Blob(MessageTypes.PairRound2, "device-round-2", 2));
        transport.Enqueue(Mac("device-confirm", 3));
        transport.Enqueue(Frame(MessageTypes.PairAccepted, @"""device_name"":""KVMpad-7F3A""", 4));

        var client = new PairingClient(transport, pake);

        PairingFailedException ex = await Assert.ThrowsAsync<PairingFailedException>(
            () => client.RunAsync(NewContext(), CancellationToken.None));

        Assert.Contains("token_enc", ex.Message);
    }

    [Fact]
    public async Task TheStreamEndingMidRunIsATimeoutAndStoresNothing()
    {
        var transport = new FakeTransport();
        await transport.ConnectAsync(CancellationToken.None);
        using var pake = new FakePakeHandshake();
        transport.EndOfStream();

        var client = new PairingClient(transport, pake);

        PairingFailedException ex = await Assert.ThrowsAsync<PairingFailedException>(
            () => client.RunAsync(NewContext(), CancellationToken.None));

        Assert.Equal(PairingRejectionReason.Timeout, ex.Reason);
    }

    [Fact]
    public void EveryRejectionReasonHasASentenceThatNamesNoSecret()
    {
        foreach (PairingRejectionReason reason in Enum.GetValues<PairingRejectionReason>())
        {
            string text = PairingFailedException.Describe(reason);
            Assert.False(string.IsNullOrWhiteSpace(text));
            Assert.DoesNotContain("482913", text);
            Assert.DoesNotContain(Fingerprint, text);
        }
    }
}
