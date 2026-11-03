// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Text;

namespace KVMpad.Client.Core.Pairing;

/// <summary>The 6-digit pairing code: the only thing the user types in the whole flow (ADR-022 FR-721).</summary>
public static class PairingCode
{
    public const int DigitCount = 6;

    /// <summary>
    /// True for exactly six decimal digits. The full range <c>000000</c>–<c>999999</c> is valid: excluding
    /// "ugly" patterns would only lower the entropy the whole scheme depends on (FR-722).
    /// </summary>
    public static bool IsValid(string? code)
    {
        if (code is null || code.Length != DigitCount)
            return false;
        foreach (char c in code)
        {
            if (c is < '0' or > '9')
                return false;
        }
        return true;
    }

    /// <summary>Strips spaces and separators the user may have copied from the device's <c>ddd ddd</c> display.</summary>
    public static string Normalize(string? input)
    {
        if (string.IsNullOrEmpty(input))
            return string.Empty;
        var builder = new StringBuilder(DigitCount);
        foreach (char c in input)
        {
            if (c is >= '0' and <= '9')
                builder.Append(c);
        }
        return builder.ToString();
    }

    /// <summary>Formats a code as the device shows it, <c>ddd ddd</c>.</summary>
    public static string Format(string code)
        => IsValid(code) ? code[..3] + " " + code[3..] : code;
}

/// <summary>
/// Everything one pairing run needs. The code lives here only for the duration of the run and is cleared by
/// <see cref="Clear"/> afterwards; it is never logged, stored or sent outside the PAKE (SEC-105).
/// </summary>
public sealed class PairingContext
{
    /// <summary>The client identifier is a fixed-width value on the wire: 16 bytes, 32 hex characters.</summary>
    public const int ClientIdBytes = 16;

    /// <param name="clientId">
    /// 32 hex characters — the stable per-installation identifier of ADR-029 §4. It is not a free-form
    /// string: the device parses it as exactly <see cref="ClientIdBytes"/> bytes and folds those bytes into
    /// the channel binding, so anything else would derive a different password.
    /// </param>
    public PairingContext(string code, string clientName, string clientId, string deviceCertificateSha256)
    {
        if (!PairingCode.IsValid(code))
            throw new ArgumentException("The pairing code must be exactly 6 digits.", nameof(code));
        if (string.IsNullOrWhiteSpace(clientName))
            throw new ArgumentException("A client name is required.", nameof(clientName));

        ClientIdValue = ParseHex(clientId, ClientIdBytes, nameof(clientId),
            "The client identifier must be 32 hex characters (16 bytes).");
        CertificateSha256Value = ParseHex(deviceCertificateSha256, 32, nameof(deviceCertificateSha256),
            "The device certificate fingerprint must be 64 hex characters; without it there is no channel binding.");

        Code = code;
        ClientName = clientName;
        ClientId = Convert.ToHexStringLower(ClientIdValue);
        DeviceCertificateSha256 = Convert.ToHexStringLower(CertificateSha256Value);
    }

    private static byte[] ParseHex(string? value, int expectedBytes, string parameterName, string message)
    {
        if (value is null || value.Length != expectedBytes * 2)
            throw new ArgumentException(message, parameterName);
        try
        {
            return Convert.FromHexString(value);
        }
        catch (FormatException)
        {
            throw new ArgumentException(message, parameterName);
        }
    }

    /// <summary>The 6 digits the user typed. Cleared after the run.</summary>
    public string Code { get; private set; }

    /// <summary>The name the device shows on its Accept screen: the Windows host name (FR-724).</summary>
    public string ClientName { get; }

    /// <summary>
    /// A stable, non-secret identifier for this installation, bound into the PAKE transcript alongside the
    /// certificate hash (SEC-102). Lowercase hex, as <c>pair.hello</c> carries it.
    /// </summary>
    public string ClientId { get; }

    /// <summary>
    /// SHA-256 of the certificate the device presented on <em>this</em> handshake, captured by
    /// <c>PairingCertificateCapture</c>. On success the client pins exactly this value (SEC-102).
    /// </summary>
    public string DeviceCertificateSha256 { get; }

    /// <summary>The raw 16 bytes behind <see cref="ClientId"/>, as the handshake consumes them.</summary>
    public byte[] ClientIdValue { get; }

    /// <summary>The raw 32 bytes behind <see cref="DeviceCertificateSha256"/>.</summary>
    public byte[] CertificateSha256Value { get; }

    /// <summary>
    /// The name that actually entered the channel binding, filled in by the handshake once it starts.
    /// </summary>
    /// <remarks>
    /// The wire caps the client name at 23 bytes and the pairing core truncates anything longer. The binding
    /// commits to the truncated form, so <c>pair.hello</c> has to carry that and not the original, or the
    /// device derives a different password and the exchange fails at key confirmation for no visible reason.
    /// </remarks>
    public string EffectiveClientName { get; internal set; } = string.Empty;

    /// <summary>Overwrites the code in memory once the window is over (SEC-105).</summary>
    public void Clear() => Code = string.Empty;
}

/// <summary>What a successful pairing leaves behind.</summary>
public sealed class PairingResult
{
    public PairingResult(string deviceName, string pinnedFingerprint, string token)
    {
        DeviceName = deviceName;
        PinnedFingerprint = pinnedFingerprint;
        Token = token;
    }

    /// <summary>The device name from <c>pair.accepted</c>.</summary>
    public string DeviceName { get; }

    /// <summary>The certificate fingerprint to pin — the one that was in the transcript (SEC-102).</summary>
    public string PinnedFingerprint { get; }

    /// <summary>The token, base64url. Goes straight to the credential store, never to a file (SEC-105).</summary>
    public string Token { get; }
}

/// <summary>Why the device refused, as <c>pair.rejected</c> reports it — without detail that would help an attacker.</summary>
public enum PairingRejectionReason
{
    Unknown,
    Timeout,
    LocalReject,
    Locked,
    Expired,
    CodeMismatch,
    ChannelBindingMismatch,
}

/// <summary>Raised when a pairing run does not end in a token.</summary>
public sealed class PairingFailedException : Exception
{
    public PairingFailedException(PairingRejectionReason reason, string message) : base(message)
        => Reason = reason;

    public PairingRejectionReason Reason { get; }

    /// <summary>Maps a <c>pair.rejected</c> reason string to the enum (ADR-022 protocol table).</summary>
    public static PairingRejectionReason ParseReason(string? reason) => reason switch
    {
        "timeout" => PairingRejectionReason.Timeout,
        "local_reject" => PairingRejectionReason.LocalReject,
        "locked" => PairingRejectionReason.Locked,
        _ => PairingRejectionReason.Unknown,
    };

    /// <summary>An English sentence for the UI, with no hint about which part of the secret was wrong.</summary>
    public static string Describe(PairingRejectionReason reason) => reason switch
    {
        PairingRejectionReason.Timeout => "The device's pairing window closed before pairing finished. Open it again and retry.",
        PairingRejectionReason.LocalReject => "Pairing was rejected on the device.",
        PairingRejectionReason.Locked => "The device has locked pairing after repeated failures. Wait ten minutes and retry.",
        PairingRejectionReason.Expired => "The pairing code has expired. Open the pairing screen on the device again.",
        PairingRejectionReason.CodeMismatch => "Pairing failed. Check the code on the device's screen and retry.",
        PairingRejectionReason.ChannelBindingMismatch =>
            "The device's certificate does not match the one in the pairing exchange. Nothing was stored. "
            + "Pairing was abandoned because another device may be impersonating this one on the network.",
        _ => "Pairing failed.",
    };
}
