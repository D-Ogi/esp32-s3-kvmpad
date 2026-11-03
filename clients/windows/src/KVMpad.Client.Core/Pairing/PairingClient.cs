// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Security.Cryptography;
using System.Threading;
using System.Threading.Tasks;
using KVMpad.Client.Core.Protocol;
using KVMpad.Client.Core.Session;

namespace KVMpad.Client.Core.Pairing;

/// <summary>
/// Runs one pairing attempt: the <c>pair.*</c> exchange of ADR-029 §4 and the token hand-off.
/// </summary>
/// <remarks>
/// <para>
/// This class is the transport driver and nothing more. The protocol — the key schedule, the channel
/// binding, the blob layout and every constant-time comparison — lives in the pairing core behind
/// <see cref="IPakeHandshake"/>, which the firmware compiles from the same sources. What is enforced here is
/// what belongs to the client's side of the conversation: one attempt per run, the whole window bounded by
/// 180 s as a real deadline, <c>pair.*</c> frames only, in the order ADR-029 fixes, and a pin taken from the
/// certificate that was in the transcript rather than from whatever the next handshake presents (FR-722,
/// SEC-102, SEC-103, SEC-104).
/// </para>
/// <para>
/// Message order (ADR-029 §4). The device speaks first after <c>pair.hello</c>: its <c>pair.round1</c>
/// doubles as the acknowledgment, and the client answers each round rather than leading it.
/// </para>
/// <code>
/// C: pair.hello     D: pair.round1 + device_name
/// C: pair.round1    D: pair.round2
/// C: pair.round2
/// C: pair.confirm   D: pair.confirm
///                   D: pair.accepted | pair.rejected
/// </code>
/// </remarks>
public sealed class PairingClient
{
    private readonly IMessageTransport _transport;
    private readonly IPakeHandshake _pake;
    private readonly SequenceCounter _seq = new();

    /// <param name="window">
    /// How long the whole attempt may take. The default is the 180 s of FR-722 and is what production uses; a
    /// shorter value exists so a test can watch the window close without waiting three minutes for it.
    /// </param>
    public PairingClient(IMessageTransport transport, IPakeHandshake pake, TimeSpan? window = null)
    {
        _transport = transport ?? throw new ArgumentNullException(nameof(transport));
        _pake = pake ?? throw new ArgumentNullException(nameof(pake));
        Window = window ?? ProtocolLimits.PairingWindow;
        if (Window <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(window), "The pairing window must be positive.");
    }

    /// <summary>The bound on the whole attempt, counted from the first frame (FR-722).</summary>
    public TimeSpan Window { get; }

    /// <summary>
    /// Pairs once. Succeeds only when the device's key confirmation verifies and the token opens; any other
    /// outcome throws and stores nothing.
    /// </summary>
    /// <remarks>
    /// The window is enforced as a real deadline over the entire run, not merely checked between frames: a
    /// device that goes quiet mid-handshake — or an attacker holding the socket open to keep the Accept
    /// screen up — must not be able to stall the attempt indefinitely (FR-722, SEC-104).
    /// </remarks>
    public async Task<PairingResult> RunAsync(PairingContext context, CancellationToken cancellationToken)
    {
        if (context is null)
            throw new ArgumentNullException(nameof(context));

        using var window = new CancellationTokenSource(Window);
        using CancellationTokenSource linked =
            CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, window.Token);
        CancellationToken attempt = linked.Token;

        try
        {
            // Starting the handshake fixes the channel binding, including the truncation of the client name
            // that pair.hello then has to report (ADR-029 §2).
            _pake.Begin(context);

            await SendAsync(MessageTypes.PairHello,
                new PairHelloPayload(_pake.WireVersion, _pake.AlgorithmId, context.EffectiveClientName,
                    context.ClientId, context.DeviceCertificateSha256),
                attempt).ConfigureAwait(false);

            ServerMessage deviceRound1 = await ExpectAsync(MessageTypes.PairRound1, attempt).ConfigureAwait(false);
            string deviceName = deviceRound1.PayloadString("device_name") ?? "KVMpad";
            byte[] clientRound1 = _pake.Round1(RequireBlob(deviceRound1, "blob", MessageTypes.PairRound1));
            await SendAsync(MessageTypes.PairRound1, new PairBlobPayload(clientRound1), attempt)
                .ConfigureAwait(false);

            ServerMessage deviceRound2 = await ExpectAsync(MessageTypes.PairRound2, attempt).ConfigureAwait(false);
            byte[] clientRound2 = _pake.Round2(RequireBlob(deviceRound2, "blob", MessageTypes.PairRound2));
            await SendAsync(MessageTypes.PairRound2, new PairBlobPayload(clientRound2), attempt)
                .ConfigureAwait(false);

            // ADR-029 §4: the client holds everything it needs after the device's round 2, so round 2 and the
            // confirmation go out back to back. The client proves first.
            await SendAsync(MessageTypes.PairConfirm, new PairMacPayload(_pake.ConfirmationOut()), attempt)
                .ConfigureAwait(false);

            ServerMessage deviceConfirm = await ExpectAsync(MessageTypes.PairConfirm, attempt).ConfigureAwait(false);
            _pake.ConfirmationIn(RequireBlob(deviceConfirm, "mac", MessageTypes.PairConfirm));

            ServerMessage accepted = await ReceiveAsync(attempt).ConfigureAwait(false);
            if (accepted.Type == MessageTypes.PairRejected)
            {
                PairingRejectionReason reason = PairingFailedException.ParseReason(accepted.PayloadString("reason"));
                throw new PairingFailedException(reason, PairingFailedException.Describe(reason));
            }
            if (accepted.Type != MessageTypes.PairAccepted)
            {
                throw new PairingFailedException(
                    PairingRejectionReason.Unknown,
                    $"The device answered '{accepted.Type}' where pair.accepted was expected.");
            }

            byte[] sealedToken = RequireBlob(accepted, "token_enc", MessageTypes.PairAccepted);
            // The device's name in pair.accepted is authoritative over the one in pair.round1.
            deviceName = accepted.PayloadString("device_name") ?? deviceName;

            byte[] token = _pake.OpenToken(sealedToken);
            string tokenText;
            try
            {
                tokenText = Base64Url(token);
            }
            finally
            {
                CryptographicOperations.ZeroMemory(token);
            }

            return new PairingResult(deviceName, context.DeviceCertificateSha256, tokenText);
        }
        catch (OperationCanceledException) when (window.IsCancellationRequested && !cancellationToken.IsCancellationRequested)
        {
            // The window closed, not the caller's token: that is an expiry, and the one reason the user is
            // shown is "the window closed, start again" — never which half of the exchange stalled.
            throw new PairingFailedException(
                PairingRejectionReason.Expired, PairingFailedException.Describe(PairingRejectionReason.Expired));
        }
        finally
        {
            // The code is of no further use and must not survive the run in memory (SEC-105).
            context.Clear();
        }
    }

    /// <summary>Base64url without padding, the form the token is stored in.</summary>
    internal static string Base64Url(byte[] value)
        => Convert.ToBase64String(value).TrimEnd('=').Replace('+', '-').Replace('/', '_');

    private async Task SendAsync(string type, IClientPayload payload, CancellationToken cancellationToken)
    {
        byte[] frame = ProtocolCodec.Encode(type, _seq.Next(), null, null, payload);
        await _transport.SendAsync(frame, cancellationToken).ConfigureAwait(false);
    }

    /// <summary>
    /// Reads the next frame and insists it is the expected <c>pair.*</c> message. A frame of the right shape
    /// but the wrong turn is refused rather than interpreted: out-of-sequence pairing frames are
    /// <c>ERR_PAIRING_EXPIRED</c> territory on the device side too (ADR-029 §5).
    /// </summary>
    private async Task<ServerMessage> ExpectAsync(string type, CancellationToken cancellationToken)
    {
        ServerMessage message = await ReceiveAsync(cancellationToken).ConfigureAwait(false);
        if (message.Type == MessageTypes.PairRejected)
        {
            PairingRejectionReason reason = PairingFailedException.ParseReason(message.PayloadString("reason"));
            throw new PairingFailedException(reason, PairingFailedException.Describe(reason));
        }
        if (message.Type != type)
        {
            throw new PairingFailedException(
                PairingRejectionReason.Unknown,
                $"The device answered '{message.Type}' where {type} was expected.");
        }
        return message;
    }

    /// <summary>Decodes one base64 field, refusing a missing or malformed one (ADR-029 §5, ERR_BAD_FRAME).</summary>
    private static byte[] RequireBlob(ServerMessage message, string field, string type)
    {
        string? encoded = message.PayloadString(field);
        if (encoded is null)
        {
            throw new PairingFailedException(
                PairingRejectionReason.Unknown, $"{type} carried no '{field}'.");
        }
        try
        {
            return Convert.FromBase64String(encoded);
        }
        catch (FormatException)
        {
            throw new PairingFailedException(
                PairingRejectionReason.Unknown, $"{type} carried a malformed '{field}'.");
        }
    }

    private async Task<ServerMessage> ReceiveAsync(CancellationToken cancellationToken)
    {
        byte[]? frame = await _transport.ReceiveAsync(cancellationToken).ConfigureAwait(false);
        if (frame is null)
        {
            throw new PairingFailedException(
                PairingRejectionReason.Timeout, PairingFailedException.Describe(PairingRejectionReason.Timeout));
        }

        ServerMessage message = ProtocolCodec.Decode(frame);
        if (message.Type == MessageTypes.Error && message.ErrorCode == ErrorCodes.PairingExpired)
        {
            throw new PairingFailedException(
                PairingRejectionReason.Expired, PairingFailedException.Describe(PairingRejectionReason.Expired));
        }
        return message;
    }
}
