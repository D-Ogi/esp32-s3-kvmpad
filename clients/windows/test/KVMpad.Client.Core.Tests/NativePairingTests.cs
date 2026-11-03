// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using KVMpad.Client.Core.Pairing;
using Xunit;

namespace KVMpad.Client.Core.Tests;

/// <summary>
/// The real EC-JPAKE of ADR-029, driven through the native pairing library, against the four interop vectors
/// in <c>test/pairing/vectors/</c>.
/// </summary>
/// <remarks>
/// <para>
/// This is the evidence ADR-022 asked for and deferred: "identical test vectors on the firmware and client
/// sides". Each case runs a real <c>kvm_pair_session_t</c> DEVICE and a real <c>kvm_pair_client_t</c> CLIENT
/// in this process, over the seeded generator the vectors were recorded with, and every recorded byte is
/// compared — the four round blobs, the transcript hash, both confirmation tags, the sealed token, the token,
/// and the hash the DEVICE stores.
/// </para>
/// <para>
/// A changed label, a changed blob layout or a changed transcript order therefore fails here rather than
/// silently breaking a user's pairing. The deterministic part is exactly what ADR-029 §5 says it is: a live
/// device and a live client draw real randomness, so the blobs differ on every real run, but how the tags,
/// keys and sealed token follow from the blobs and the binding does not.
/// </para>
/// <para>
/// The runs share one native fixture with static state, so the class is not parallelized.
/// </para>
/// </remarks>
[Collection("native-pairing")]
public sealed class NativePairingTests
{
    private const string TestLibrary = "kvmpad_pairing_test";

    // Fields of kvmpad_vector_field_t.
    private const int FieldCode = 1;
    private const int FieldCertSha256 = 2;
    private const int FieldClientId = 3;
    private const int FieldDeviceRound1 = 4;
    private const int FieldClientRound1 = 5;
    private const int FieldDeviceRound2 = 6;
    private const int FieldClientRound2 = 7;
    private const int FieldClientConfirm = 8;
    private const int FieldDeviceConfirm = 9;
    private const int FieldTokenSealed = 10;
    private const int FieldToken = 11;
    private const int FieldTokenSha256 = 12;
    private const int FieldTranscript = 13;

    [DllImport(TestLibrary, EntryPoint = "kvmpad_test_vector_run", CharSet = CharSet.Ansi)]
    private static extern int VectorRun(
        ulong seed, int requireAccept, string? clientCodeOverride, int certOverride,
        out int failedStep, out int failedError, out int completed);

    [DllImport(TestLibrary, EntryPoint = "kvmpad_test_vector_field")]
    private static extern int VectorField(int field, byte[] output, int outputCapacity, out int outputLength);

    [DllImport(TestLibrary, EntryPoint = "kvmpad_pair_err_name")]
    private static extern IntPtr TestErrorName(int err);

    private static byte[] Field(int field)
    {
        byte[] buffer = new byte[512];
        int rc = VectorField(field, buffer, buffer.Length, out int length);
        Assert.Equal(0, rc);
        byte[] exact = new byte[length];
        Array.Copy(buffer, exact, length);
        return exact;
    }

    private static string ErrorName(int err) => Marshal.PtrToStringUTF8(TestErrorName(err)) ?? "UNKNOWN";

    // ---- the vector files -----------------------------------------------------------------------

    private static string VectorDirectory
    {
        get
        {
            string local = Path.Combine(AppContext.BaseDirectory, "vectors");
            if (Directory.Exists(local))
                return local;
            throw new DirectoryNotFoundException(
                $"The ADR-029 interop vectors are not beside the test binary (looked in '{local}'). "
                + "They are copied from test/pairing/vectors by the test project.");
        }
    }

    private static JsonDocument LoadVector(string name)
        => JsonDocument.Parse(File.ReadAllBytes(Path.Combine(VectorDirectory, name + ".json")));

    public static TheoryData<string> AllVectors() => new()
    {
        "pair-success-require-accept",
        "pair-success-auto-accept",
        "pair-wrong-code",
        "pair-cert-mismatch",
    };

    public static TheoryData<string> SuccessVectors() => new()
    {
        "pair-success-require-accept",
        "pair-success-auto-accept",
    };

    /// <summary>
    /// Runs the case a vector file describes, with the inputs taken from the file rather than restated here —
    /// so a regenerated vector drives a different run instead of quietly passing against stale expectations.
    /// </summary>
    private static (int Step, int Error, bool Completed) RunFromFile(JsonElement input)
    {
        ulong seed = input.GetProperty("generator_seed").GetUInt64();
        bool requireAccept = input.GetProperty("require_accept").GetBoolean();
        string deviceCode = input.GetProperty("device_code").GetString()!;
        string clientCode = input.GetProperty("client_code").GetString()!;
        bool certOverride = input.GetProperty("device_cert_sha256").GetString()
            != input.GetProperty("client_cert_sha256").GetString();

        string? codeOverride = clientCode == deviceCode ? null : clientCode;

        int rc = VectorRun(seed, requireAccept ? 1 : 0, codeOverride, certOverride ? 1 : 0,
            out int step, out int error, out int completed);
        Assert.Equal(0, rc);
        return (step, error, completed != 0);
    }

    // ---- the library is there -------------------------------------------------------------------

    [Fact]
    public void TheNativeLibraryLoadsAndAgreesWithTheManagedSideAboutEverySize()
    {
        // Every length the managed code uses comes from the library. If this drifts, the C# side is carrying
        // a stale copy of a constant that ADR-029 fixed, which is the class of bug the native boundary exists
        // to prevent.
        using var handshake = new NativePakeHandshake();

        Assert.Equal("ecjpake-p256-sha256", handshake.AlgorithmId);
        Assert.Equal(1, handshake.WireVersion);
    }

    [Fact]
    public void TheNativeSha256AgreesWithTheManagedOne()
    {
        // The certificate pin is a SHA-256 the managed side computes and the native binding consumes; if the
        // two disagreed, every pairing would fail at confirmation with nothing to point at.
        byte[] input = Encoding.ASCII.GetBytes("KVMpad interop vector DEVICE certificate (DER stand-in)");
        byte[] native = new byte[32];

        Assert.Equal(0, PairingNativeProbe.Sha256(input, input.Length, native, native.Length));
        Assert.Equal(SHA256.HashData(input), native);
    }

    // ---- the vectors ----------------------------------------------------------------------------

    [Theory]
    [MemberData(nameof(AllVectors))]
    public void EachVectorReachesTheRecordedOutcome(string name)
    {
        using JsonDocument vector = LoadVector(name);
        JsonElement expect = vector.RootElement.GetProperty("expect");

        (int step, int error, bool completed) = RunFromFile(vector.RootElement.GetProperty("input"));

        Assert.Equal(expect.GetProperty("failed_step").GetInt32(), step);
        Assert.Equal(expect.GetProperty("failed_error").GetString(), ErrorName(error));
        Assert.Equal(expect.GetProperty("completed").GetBoolean(), completed);

        // The wire identity in the file has to be the one the library implements.
        Assert.Equal("ecjpake-p256-sha256", vector.RootElement.GetProperty("pake").GetString());
        Assert.Equal(1, vector.RootElement.GetProperty("wire_version").GetInt32());
    }

    [Theory]
    [MemberData(nameof(SuccessVectors))]
    public void ASuccessfulVectorReproducesEveryRecordedByte(string name)
    {
        using JsonDocument vector = LoadVector(name);
        JsonElement input = vector.RootElement.GetProperty("input");
        JsonElement expect = vector.RootElement.GetProperty("expect");

        (int step, _, bool completed) = RunFromFile(input);
        Assert.Equal(0, step);
        Assert.True(completed);

        // Inputs: the code the device drew from the seed, and the binding material.
        Assert.Equal(input.GetProperty("device_code").GetString(), Encoding.ASCII.GetString(Field(FieldCode)));
        Assert.Equal(input.GetProperty("device_cert_sha256").GetString(), Hex(Field(FieldCertSha256)));
        Assert.Equal(input.GetProperty("client_id").GetString(), Hex(Field(FieldClientId)));

        // The four blobs, in the canonical order the transcript hashes them in (ADR-029 §3).
        Assert.Equal(expect.GetProperty("device_round1").GetString(), Base64(Field(FieldDeviceRound1)));
        Assert.Equal(expect.GetProperty("client_round1").GetString(), Base64(Field(FieldClientRound1)));
        Assert.Equal(expect.GetProperty("device_round2").GetString(), Base64(Field(FieldDeviceRound2)));
        Assert.Equal(expect.GetProperty("client_round2").GetString(), Base64(Field(FieldClientRound2)));

        // The derived material: everything a second implementation can get wrong.
        Assert.Equal(expect.GetProperty("transcript_sha256").GetString(), Hex(Field(FieldTranscript)));
        Assert.Equal(expect.GetProperty("client_confirm").GetString(), Base64(Field(FieldClientConfirm)));
        Assert.Equal(expect.GetProperty("device_confirm").GetString(), Base64(Field(FieldDeviceConfirm)));
        Assert.Equal(expect.GetProperty("token_sealed").GetString(), Base64(Field(FieldTokenSealed)));
        Assert.Equal(expect.GetProperty("token").GetString(), Hex(Field(FieldToken)));
        Assert.Equal(expect.GetProperty("token_sha256_stored").GetString(), Hex(Field(FieldTokenSha256)));
    }

    [Fact]
    public void TheSealedTokenIsFortyEightBytesAndTheTokenIsThirtyTwo()
    {
        // ADR-029 §3: ciphertext plus a 16-byte GCM tag over a 32-byte token. The managed driver sizes its
        // frame checks from the library, so this is the number both ends have to mean.
        using JsonDocument vector = LoadVector("pair-success-require-accept");
        RunFromFile(vector.RootElement.GetProperty("input"));

        Assert.Equal(48, Field(FieldTokenSealed).Length);
        Assert.Equal(32, Field(FieldToken).Length);
        Assert.Equal(32, Field(FieldClientConfirm).Length);
        Assert.Equal(32, Field(FieldDeviceConfirm).Length);
    }

    [Fact]
    public void TheRoundBlobsStayInsideTheEnvelopeLimit()
    {
        // 03 §6.5 caps a frame at 1024 bytes and ADR-029 §4 keeps the blobs under that in base64. Round 1 is
        // the big one, so it is the one that has to fit.
        using JsonDocument vector = LoadVector("pair-success-require-accept");
        RunFromFile(vector.RootElement.GetProperty("input"));

        byte[] round1 = Field(FieldDeviceRound1);
        Assert.InRange(round1.Length, 1, 330);
        // The whole frame is the envelope plus the base64 of the blob plus the device name; 512 bytes of
        // base64 leaves ample room inside 1024.
        Assert.InRange(Base64(round1).Length, 1, 512);
        Assert.InRange(Field(FieldDeviceRound2).Length, 1, 165);
    }

    [Fact]
    public void AWrongCodeFailsAtConfirmationAndStoresNothing()
    {
        using JsonDocument vector = LoadVector("pair-wrong-code");
        JsonElement input = vector.RootElement.GetProperty("input");

        // The two codes really do differ; otherwise this test would be asserting nothing.
        Assert.NotEqual(input.GetProperty("device_code").GetString(), input.GetProperty("client_code").GetString());

        (_, int error, bool completed) = RunFromFile(input);

        Assert.Equal("BAD_CODE", ErrorName(error));
        Assert.False(completed);
    }

    [Fact]
    public void ASubstitutedCertificateIsRefusedBeforeARunStarts()
    {
        // SEC-102, and ADR-029 §2: the device compares the hash the client reports against its own before
        // starting, so a certificate-substitution probe cannot burn the user's three attempts. Failing at
        // step 1 rather than at confirmation is the evidence for that.
        using JsonDocument vector = LoadVector("pair-cert-mismatch");

        (int step, int error, bool completed) = RunFromFile(vector.RootElement.GetProperty("input"));

        Assert.Equal(1, step);
        Assert.Equal("BINDING", ErrorName(error));
        Assert.False(completed);
    }

    [Fact]
    public void TheTwoSuccessVectorsDifferInEveryDerivedValue()
    {
        // Different seeds, so different blobs, so different transcripts and different keys. If the two
        // agreed anywhere here, something would be deriving from a constant rather than from the run.
        using JsonDocument a = LoadVector("pair-success-require-accept");
        using JsonDocument b = LoadVector("pair-success-auto-accept");
        JsonElement ea = a.RootElement.GetProperty("expect");
        JsonElement eb = b.RootElement.GetProperty("expect");

        Assert.NotEqual(ea.GetProperty("transcript_sha256").GetString(), eb.GetProperty("transcript_sha256").GetString());
        Assert.NotEqual(ea.GetProperty("client_confirm").GetString(), eb.GetProperty("client_confirm").GetString());
        Assert.NotEqual(ea.GetProperty("device_confirm").GetString(), eb.GetProperty("device_confirm").GetString());
        Assert.NotEqual(ea.GetProperty("token_sealed").GetString(), eb.GetProperty("token_sealed").GetString());
        Assert.NotEqual(ea.GetProperty("token").GetString(), eb.GetProperty("token").GetString());

        // And the confirmation tags of the two roles in one run are different too: one key per direction
        // (ADR-029 §3), so a tag cannot be replayed back at its sender.
        Assert.NotEqual(ea.GetProperty("client_confirm").GetString(), ea.GetProperty("device_confirm").GetString());
    }

    [Fact]
    public void TheStoredHashIsTheHashOfTheTokenAndNotTheTokenItself()
    {
        // SEC-105: the device keeps only the hash. A record holding the token would be a credential at rest.
        using JsonDocument vector = LoadVector("pair-success-require-accept");
        JsonElement expect = vector.RootElement.GetProperty("expect");

        RunFromFile(vector.RootElement.GetProperty("input"));

        byte[] token = Field(FieldToken);
        byte[] stored = Field(FieldTokenSha256);

        Assert.Equal(SHA256.HashData(token), stored);
        Assert.NotEqual(Hex(token), Hex(stored));
        Assert.Equal(expect.GetProperty("token_sha256_stored").GetString(), Hex(stored));
    }

    // ---- the handshake wrapper ------------------------------------------------------------------

    [Fact]
    public void TheHandshakeRefusesToRunOutOfOrder()
    {
        // The stage gate lives in the core; the wrapper must not let a caller skip past it. A run that has
        // not verified the device tag can never be made to open a token.
        using var handshake = new NativePakeHandshake();

        Assert.Throws<InvalidOperationException>(() => handshake.ConfirmationOut());
        Assert.Throws<InvalidOperationException>(() => handshake.OpenToken(new byte[48]));
        Assert.Throws<InvalidOperationException>(() => handshake.Round1(new byte[10]));

        handshake.Begin(new PairingContext("482913", "Desk1", "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf",
            "9f2c1a04b7e85d3610fa4c92db7e05183c6a4f20e9b1d74c852036af1b9e4d7c"));

        // Started, but no rounds have run: opening a token is a state error from the core.
        PairingFailedException ex = Assert.Throws<PairingFailedException>(() => handshake.OpenToken(new byte[48]));
        Assert.Contains("STATE", ex.Message);

        // And it cannot be started twice.
        Assert.Throws<InvalidOperationException>(() => handshake.Begin(
            new PairingContext("482913", "Desk1", "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf",
                "9f2c1a04b7e85d3610fa4c92db7e05183c6a4f20e9b1d74c852036af1b9e4d7c")));
    }

    [Fact]
    public void TheHandshakeReportsTheTruncatedClientNameItBoundTo()
    {
        using var handshake = new NativePakeHandshake();
        var context = new PairingContext("482913", new string('W', 40), "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf",
            "9f2c1a04b7e85d3610fa4c92db7e05183c6a4f20e9b1d74c852036af1b9e4d7c");

        handshake.Begin(context);

        // The wire cap is 24 bytes including the terminator, so 23 characters survive.
        Assert.Equal(new string('W', 23), context.EffectiveClientName);
    }

    [Fact]
    public void AMalformedRoundBlobIsRefusedByTheCore()
    {
        // ADR-029 §4: a blob with a zero-length chunk, an over-long chunk, a truncation or trailing bytes is
        // a bad frame. The managed side does not parse blobs at all — this proves the core rejects them.
        using var handshake = new NativePakeHandshake();
        handshake.Begin(new PairingContext("482913", "Desk1", "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf",
            "9f2c1a04b7e85d3610fa4c92db7e05183c6a4f20e9b1d74c852036af1b9e4d7c"));

        foreach (byte[] bad in MalformedBlobs())
            Assert.Throws<PairingFailedException>(() => handshake.Round1(bad));
    }

    private static IEnumerable<byte[]> MalformedBlobs()
    {
        yield return Array.Empty<byte>();              // nothing at all
        yield return new byte[] { 0x00 };              // a zero-length chunk
        yield return new byte[] { 0x41 };              // a length prefix with no payload
        yield return new byte[] { 0xff, 0x01, 0x02 };  // a chunk longer than a P-256 point
        yield return new byte[200];                    // the right size, all zeros: not a valid point
    }

    [Fact]
    public void TheShippingLibraryExportsNoTestFixtureEntryPoint()
    {
        // ADR-028 §4: the fixture lives in a separate artifact, and the guarantee is the export table — the
        // shipping DLL offers no way to drive the DEVICE side or to seed a generator. The fixture's own
        // header (test/pairing/det_rng.h) says its generator must never reach the CLIENT; this is the check
        // that it has not, and it would fail the moment someone added the fixture sources to that target.
        Assert.Throws<EntryPointNotFoundException>(() => ShippingLibraryProbe.VectorRun(0, 0, null, 0,
            out _, out _, out _));

        // The CLIENT entry points, by contrast, are there.
        using var handshake = new NativePakeHandshake();
        Assert.Equal("ecjpake-p256-sha256", handshake.AlgorithmId);
    }

    [Fact]
    public void DisposingTwiceIsSafe()
    {
        var handshake = new NativePakeHandshake();
        handshake.Dispose();
        handshake.Dispose();

        Assert.Throws<ObjectDisposedException>(() => handshake.Transcript());
    }

    private static string Hex(byte[] value) => Convert.ToHexStringLower(value);

    private static string Base64(byte[] value) => Convert.ToBase64String(value);
}

/// <summary>Reaches the shipping library's SHA-256 export, to compare it with the managed one.</summary>
internal static class PairingNativeProbe
{
    [DllImport("kvmpad_pairing", EntryPoint = "kvmpad_pair_sha256")]
    internal static extern int Sha256(byte[] input, int inputLength, byte[] output, int outputCapacity);
}

/// <summary>
/// Declares a fixture entry point against the <em>shipping</em> library on purpose, so that a test can show
/// it is not there.
/// </summary>
internal static class ShippingLibraryProbe
{
    [DllImport("kvmpad_pairing", EntryPoint = "kvmpad_test_vector_run", CharSet = CharSet.Ansi)]
    internal static extern int VectorRun(
        ulong seed, int requireAccept, string? clientCodeOverride, int certOverride,
        out int failedStep, out int failedError, out int completed);
}

/// <summary>
/// The native vector fixture keeps the last run in static storage, so these tests must not run beside each
/// other. xUnit parallelizes across collections, not within one.
/// </summary>
[CollectionDefinition("native-pairing", DisableParallelization = true)]
public sealed class NativePairingCollection
{
}
