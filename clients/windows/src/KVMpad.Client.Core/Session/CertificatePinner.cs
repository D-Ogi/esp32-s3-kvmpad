// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Net.Security;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

namespace KVMpad.Client.Core.Session;

/// <summary>SHA-256 over a certificate's DER encoding — the value pinned at pairing (ADR-022 SEC-102).</summary>
public static class CertificateFingerprint
{
    /// <summary>Lowercase hex SHA-256 of the whole DER-encoded certificate.</summary>
    public static string Compute(X509Certificate certificate)
    {
        if (certificate is null)
            throw new ArgumentNullException(nameof(certificate));
        byte[] der = certificate.GetRawCertData();
        return Convert.ToHexStringLower(SHA256.HashData(der));
    }

    /// <summary>Lowercase hex SHA-256 of a DER blob, for test vectors and for the pairing transcript.</summary>
    public static string Compute(ReadOnlySpan<byte> der) => Convert.ToHexStringLower(SHA256.HashData(der));

    /// <summary>True when the text is 64 hexadecimal characters.</summary>
    public static bool IsWellFormed(string? fingerprint)
    {
        if (fingerprint is null || fingerprint.Length != 64)
            return false;
        foreach (char c in fingerprint)
        {
            bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            if (!hex)
                return false;
        }
        return true;
    }

    /// <summary>
    /// Case-insensitive comparison in time independent of where the first difference falls. The fingerprint is
    /// not a secret, but comparing it in constant time costs nothing and keeps the habit.
    /// </summary>
    public static bool Matches(string? a, string? b)
    {
        if (a is null || b is null || a.Length != b.Length)
            return false;
        int difference = 0;
        for (int i = 0; i < a.Length; i++)
            difference |= ToLower(a[i]) ^ ToLower(b[i]);
        return difference == 0;
    }

    private static int ToLower(char c) => c is >= 'A' and <= 'Z' ? c + 32 : c;
}

/// <summary>The verdict of a pinning check, with a reason the UI can show and a log can keep.</summary>
public readonly struct PinningVerdict
{
    private PinningVerdict(bool accepted, string reason, string? presentedFingerprint)
    {
        Accepted = accepted;
        Reason = reason;
        PresentedFingerprint = presentedFingerprint;
    }

    public bool Accepted { get; }
    public string Reason { get; }
    public string? PresentedFingerprint { get; }

    public static PinningVerdict Accept(string fingerprint) => new(true, "Certificate matches the pin stored at pairing.", fingerprint);

    public static PinningVerdict Reject(string reason, string? fingerprint = null) => new(false, reason, fingerprint);
}

/// <summary>
/// Certificate pinning for the control session (03 §6.1, ADR-021, ADR-022 SEC-102).
/// </summary>
/// <remarks>
/// <para>
/// The device presents a self-signed certificate, so the chain never validates against a public root and
/// <see cref="SslPolicyErrors"/> is expected to be non-zero. The pin is the identity: the client accepts a
/// handshake if and only if SHA-256 over the presented DER equals the fingerprint captured during pairing,
/// and it refuses otherwise. There is no "continue anyway", no fallback to chain validation and no plaintext
/// downgrade (TECH-041, AT-SEC-01).
/// </para>
/// <para>
/// Hostname and validity are deliberately not checked separately. The pin covers the entire DER, including
/// the subject and the validity dates, so a certificate that matches the pin is byte for byte the one the
/// user approved on the device's screen. Checking its <c>notAfter</c> again would only add a date on which
/// a working pairing stops working, with no attacker it excludes. If the device ever regenerates its key,
/// the pin stops matching and the recovery is to pair again — which is the intended behavior.
/// </para>
/// </remarks>
public sealed class CertificatePinner
{
    private readonly string _pinnedFingerprint;

    public CertificatePinner(string pinnedFingerprint)
    {
        if (!CertificateFingerprint.IsWellFormed(pinnedFingerprint))
            throw new ArgumentException("The pinned fingerprint must be 64 hexadecimal characters.", nameof(pinnedFingerprint));
        _pinnedFingerprint = pinnedFingerprint;
    }

    /// <summary>The pin this instance enforces, in lowercase hex.</summary>
    public string PinnedFingerprint => _pinnedFingerprint.ToLowerInvariant();

    /// <summary>Checks one presented certificate against the pin.</summary>
    public PinningVerdict Check(X509Certificate? certificate)
    {
        if (certificate is null)
            return PinningVerdict.Reject("The device presented no certificate.");

        string presented;
        try
        {
            presented = CertificateFingerprint.Compute(certificate);
        }
        catch (CryptographicException ex)
        {
            return PinningVerdict.Reject("The presented certificate could not be read: " + ex.Message);
        }

        return CertificateFingerprint.Matches(presented, _pinnedFingerprint)
            ? PinningVerdict.Accept(presented)
            : PinningVerdict.Reject(
                "Certificate fingerprint does not match the pin stored at pairing. Refusing the connection; pair again if the device was reset.",
                presented);
    }

    /// <summary>
    /// The callback to hand to <c>ClientWebSocket.Options.RemoteCertificateValidationCallback</c>. The
    /// <see cref="SslPolicyErrors"/> argument is ignored on purpose, as explained on the class.
    /// </summary>
    public RemoteCertificateValidationCallback Callback
        => (_, certificate, _, _) => Check(certificate).Accepted;
}

/// <summary>
/// Captures the certificate the device presents on the <em>pairing</em> endpoint, where no pin exists yet.
/// </summary>
/// <remarks>
/// This is the single exception to verified TLS that ADR-022 SEC-103 allows, and it is narrow by
/// construction: the captured fingerprint goes into the PAKE transcript as channel binding, and the pairing
/// engine stores it as the pin only after key confirmation succeeds. The connection carries <c>pair.*</c>
/// frames and nothing else — no status, no HID, no token, no snapshot — and it still runs over TLS, so the
/// <c>ws://</c> prohibition is untouched. Never use this type for a control session.
/// </remarks>
public sealed class PairingCertificateCapture
{
    /// <summary>The fingerprint of the certificate seen during the handshake, or null if none was seen yet.</summary>
    public string? CapturedFingerprint { get; private set; }

    /// <summary>True once a certificate has been captured.</summary>
    public bool HasCertificate => CapturedFingerprint is not null;

    /// <summary>
    /// A validation callback that accepts any certificate but records it. It accepts nothing on the wire by
    /// itself: the pairing run aborts unless the PAKE transcript confirms this exact fingerprint (SEC-102).
    /// </summary>
    public RemoteCertificateValidationCallback Callback => (_, certificate, _, _) =>
    {
        if (certificate is null)
            return false;
        CapturedFingerprint = CertificateFingerprint.Compute(certificate);
        return true;
    };
}
