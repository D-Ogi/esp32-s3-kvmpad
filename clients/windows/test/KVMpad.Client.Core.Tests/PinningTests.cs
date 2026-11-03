// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Threading;
using System.Threading.Tasks;
using KVMpad.Client.Core.Session;
using Xunit;

namespace KVMpad.Client.Core.Tests;

/// <summary>
/// Certificate pinning (03 §6.1, ADR-021, ADR-022 SEC-102) and the absence of any downgrade (TECH-041,
/// AT-SEC-01). The loopback test completes a real TLS handshake against a certificate created inside the test,
/// so the pinning decision is exercised through <see cref="SslStream"/> rather than only in isolation.
/// </summary>
public sealed class PinningTests
{
    /// <summary>A self-signed certificate with an exportable key, as the device would present.</summary>
    private static X509Certificate2 NewDeviceCertificate(string commonName = "CN=KVMpad-7F3A")
    {
        using RSA key = RSA.Create(2048);
        var request = new CertificateRequest(commonName, key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
        request.CertificateExtensions.Add(new X509BasicConstraintsExtension(false, false, 0, false));
        request.CertificateExtensions.Add(new X509EnhancedKeyUsageExtension(
            [new Oid("1.3.6.1.5.5.7.3.1")], false)); // serverAuth
        var sanBuilder = new SubjectAlternativeNameBuilder();
        sanBuilder.AddIpAddress(IPAddress.Loopback);
        request.CertificateExtensions.Add(sanBuilder.Build());

        using X509Certificate2 ephemeral = request.CreateSelfSigned(
            DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(365));

        // SChannel needs the key to come from a PKCS#12 blob rather than an ephemeral key handle.
        return X509CertificateLoader.LoadPkcs12(ephemeral.Export(X509ContentType.Pfx), null);
    }

    [Fact]
    public void TheFingerprintIsSha256OverTheWholeDerEncoding()
    {
        using X509Certificate2 certificate = NewDeviceCertificate();

        string computed = CertificateFingerprint.Compute(certificate);
        string direct = Convert.ToHexStringLower(SHA256.HashData(certificate.RawData));

        Assert.Equal(direct, computed);
        Assert.Equal(64, computed.Length);
        Assert.True(CertificateFingerprint.IsWellFormed(computed));
    }

    [Fact]
    public void AMatchingFingerprintIsAccepted()
    {
        using X509Certificate2 certificate = NewDeviceCertificate();
        var pinner = new CertificatePinner(CertificateFingerprint.Compute(certificate));

        PinningVerdict verdict = pinner.Check(certificate);

        Assert.True(verdict.Accepted);
        Assert.Equal(pinner.PinnedFingerprint, verdict.PresentedFingerprint);
    }

    [Fact]
    public void ADifferentCertificateIsRefusedEvenWithTheSameSubject()
    {
        // The attack SEC-102 is aimed at: a man in the middle presenting its own certificate for the same name.
        using X509Certificate2 real = NewDeviceCertificate();
        using X509Certificate2 impostor = NewDeviceCertificate();
        var pinner = new CertificatePinner(CertificateFingerprint.Compute(real));

        PinningVerdict verdict = pinner.Check(impostor);

        Assert.False(verdict.Accepted);
        Assert.Contains("does not match the pin", verdict.Reason);
    }

    [Fact]
    public void NoCertificateIsRefused()
    {
        using X509Certificate2 certificate = NewDeviceCertificate();
        var pinner = new CertificatePinner(CertificateFingerprint.Compute(certificate));

        PinningVerdict verdict = pinner.Check(null);

        Assert.False(verdict.Accepted);
    }

    [Fact]
    public void APinnerCannotBeBuiltWithoutAWellFormedPin()
    {
        // There is no "unpinned" mode to construct by accident.
        Assert.Throws<ArgumentException>(() => new CertificatePinner(""));
        Assert.Throws<ArgumentException>(() => new CertificatePinner("abc"));
        Assert.Throws<ArgumentException>(() => new CertificatePinner(new string('z', 64)));
    }

    [Fact]
    public void FingerprintComparisonIgnoresCaseAndRejectsLengthMismatch()
    {
        string lower = new('a', 64);
        Assert.True(CertificateFingerprint.Matches(lower, lower.ToUpperInvariant()));
        Assert.False(CertificateFingerprint.Matches(lower, new string('a', 63)));
        Assert.False(CertificateFingerprint.Matches(lower, null));
        Assert.False(CertificateFingerprint.Matches(null, lower));
    }

    [Fact]
    public void ASessionEndpointRefusesToExistWithoutAPinOrAToken()
    {
        string pin = new('a', 64);
        Assert.Throws<ArgumentException>(() => new SessionEndpoint("10.0.0.5", 8443, "", "token"));
        Assert.Throws<ArgumentException>(() => new SessionEndpoint("10.0.0.5", 8443, pin, ""));
        Assert.Throws<ArgumentException>(() => new SessionEndpoint("", 8443, pin, "token"));
        Assert.Throws<ArgumentOutOfRangeException>(() => new SessionEndpoint("10.0.0.5", 0, pin, "token"));
    }

    [Fact]
    public void TheSessionUriIsAlwaysWssAndNeverCarriesTheToken()
    {
        // TECH-041 and AT-SEC-01: no downgrade. 03 §6.1: no token in the URL.
        var endpoint = new SessionEndpoint("192.168.1.50", 8443, new string('b', 64), "s3cret-token");

        Assert.Equal("wss://192.168.1.50:8443/v1/ws", endpoint.SessionUri.ToString());
        Assert.Equal("wss://192.168.1.50:8443/v1/pair", endpoint.PairingUri.ToString());
        Assert.DoesNotContain("s3cret-token", endpoint.SessionUri.ToString());
        Assert.DoesNotContain("s3cret-token", endpoint.PairingUri.ToString());
    }

    [Fact]
    public void AnIpv6HostIsBracketedInTheUri()
    {
        var endpoint = new SessionEndpoint("fe80::1", 8443, new string('c', 64), "token");
        Assert.Equal("wss://[fe80::1]:8443/v1/ws", endpoint.SessionUri.ToString());
    }

    [Fact]
    public async Task OverARealHandshakeTheMatchingPinConnectsAndAMismatchDoesNot()
    {
        using X509Certificate2 certificate = NewDeviceCertificate();
        string fingerprint = CertificateFingerprint.Compute(certificate);

        // Matching pin: a self-signed certificate completes the handshake, which is the point — the chain
        // never validates against a public root and the pin is the identity instead.
        Assert.True(await TryHandshakeAsync(certificate, new CertificatePinner(fingerprint)));

        // Mismatched pin: refused, with no option to continue.
        using X509Certificate2 other = NewDeviceCertificate();
        Assert.False(await TryHandshakeAsync(
            certificate, new CertificatePinner(CertificateFingerprint.Compute(other))));
    }

    [Fact]
    public async Task ThePairingCaptureRecordsTheCertificateItSaw()
    {
        // ADR-022 SEC-103: the pairing endpoint is the one place an unverified certificate is allowed, and the
        // fingerprint it captures is what goes into the PAKE transcript.
        using X509Certificate2 certificate = NewDeviceCertificate();
        var capture = new PairingCertificateCapture();

        Assert.False(capture.HasCertificate);
        Assert.True(await TryHandshakeAsync(certificate, capture));

        Assert.True(capture.HasCertificate);
        Assert.Equal(CertificateFingerprint.Compute(certificate), capture.CapturedFingerprint);
    }

    private static Task<bool> TryHandshakeAsync(X509Certificate2 serverCertificate, CertificatePinner pinner)
        => TryHandshakeAsync(serverCertificate, pinner.Callback);

    private static Task<bool> TryHandshakeAsync(X509Certificate2 serverCertificate, PairingCertificateCapture capture)
        => TryHandshakeAsync(serverCertificate, capture.Callback);

    /// <summary>
    /// Completes a TLS handshake on the loopback interface. Nothing leaves the machine and no service outside
    /// the test is contacted.
    /// </summary>
    private static async Task<bool> TryHandshakeAsync(
        X509Certificate2 serverCertificate,
        RemoteCertificateValidationCallback validate)
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        int port = ((IPEndPoint)listener.LocalEndpoint).Port;

        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20));

        Task server = Task.Run(async () =>
        {
            using TcpClient accepted = await listener.AcceptTcpClientAsync(timeout.Token).ConfigureAwait(false);
            await using var stream = new SslStream(accepted.GetStream(), leaveInnerStreamOpen: false);
            try
            {
                await stream
                    .AuthenticateAsServerAsync(serverCertificate, false, checkCertificateRevocation: false)
                    .ConfigureAwait(false);
            }
            catch (Exception)
            {
                // The client refused the certificate; that is the result the test is after.
            }
        }, timeout.Token);

        bool accepted;
        try
        {
            using var client = new TcpClient();
            await client.ConnectAsync(IPAddress.Loopback, port, timeout.Token).ConfigureAwait(false);
            await using var stream = new SslStream(client.GetStream(), leaveInnerStreamOpen: false, validate);
            await stream.AuthenticateAsClientAsync("KVMpad-7F3A").ConfigureAwait(false);
            accepted = true;
        }
        catch (AuthenticationException)
        {
            accepted = false;
        }
        finally
        {
            listener.Stop();
            try
            {
                await server.ConfigureAwait(false);
            }
            catch (Exception)
            {
                // The listener was torn down; nothing to report.
            }
        }

        return accepted;
    }
}
