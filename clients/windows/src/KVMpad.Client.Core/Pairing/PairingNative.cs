// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Runtime.InteropServices;

namespace KVMpad.Client.Core.Pairing;

/// <summary>
/// The P/Invoke surface of <c>kvmpad_pairing.dll</c> — the pairing core of ADR-029 compiled for Windows
/// (see <c>clients/windows/native/</c>).
/// </summary>
/// <remarks>
/// Nothing here knows the protocol. The managed side keeps no copy of the core's structures, no size
/// literals and no key schedule: every length comes from <see cref="Sizes"/> and every step is one call into
/// the library the firmware also builds. That is the whole point of ADR-029's "one implementation compiled
/// twice", and restating any of it in C# would reintroduce the drift the decision removes.
/// </remarks>
internal static partial class PairingNative
{
    internal const string Library = "kvmpad_pairing";

    /// <summary>Success, mirroring <c>KVM_PAIR_OK</c>.</summary>
    internal const int Ok = 0;

    [LibraryImport(Library, EntryPoint = "kvmpad_pair_startup")]
    internal static partial int Startup();

    [LibraryImport(Library, EntryPoint = "kvmpad_pair_wire_version")]
    internal static partial int WireVersion();

    [LibraryImport(Library, EntryPoint = "kvmpad_pair_pake_name")]
    private static partial IntPtr PakeNamePtr();

    [LibraryImport(Library, EntryPoint = "kvmpad_pair_err_name")]
    private static partial IntPtr ErrorNamePtr(int err);

    [LibraryImport(Library, EntryPoint = "kvmpad_pair_sizes")]
    private static partial int SizesRaw(
        out int codeLength, out int sha256Length, out int clientIdLength, out int tokenLength,
        out int tokenSealedLength, out int confirmLength, out int round1Max, out int round2Max,
        out int nameCap);

    [LibraryImport(Library, EntryPoint = "kvmpad_pair_sha256")]
    internal static partial int Sha256(
        ReadOnlySpan<byte> input, int inputLength, Span<byte> output, int outputCapacity);

    [LibraryImport(Library, EntryPoint = "kvmpad_pair_client_create", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int ClientCreate(
        string code, string clientName,
        ReadOnlySpan<byte> clientId, int clientIdLength,
        ReadOnlySpan<byte> certificateSha256, int certificateSha256Length,
        Span<byte> effectiveName, int effectiveNameCapacity,
        out IntPtr handle);

    [LibraryImport(Library, EntryPoint = "kvmpad_pair_client_round1")]
    internal static partial int ClientRound1(
        IntPtr handle, ReadOnlySpan<byte> input, int inputLength,
        Span<byte> output, int outputCapacity, out int outputLength);

    [LibraryImport(Library, EntryPoint = "kvmpad_pair_client_round2")]
    internal static partial int ClientRound2(
        IntPtr handle, ReadOnlySpan<byte> input, int inputLength,
        Span<byte> output, int outputCapacity, out int outputLength);

    [LibraryImport(Library, EntryPoint = "kvmpad_pair_client_confirm_out")]
    internal static partial int ClientConfirmOut(IntPtr handle, Span<byte> output, int outputCapacity);

    [LibraryImport(Library, EntryPoint = "kvmpad_pair_client_confirm_in")]
    internal static partial int ClientConfirmIn(IntPtr handle, ReadOnlySpan<byte> tag, int tagLength);

    [LibraryImport(Library, EntryPoint = "kvmpad_pair_client_open_token")]
    internal static partial int ClientOpenToken(
        IntPtr handle, ReadOnlySpan<byte> input, int inputLength, Span<byte> token, int tokenCapacity);

    [LibraryImport(Library, EntryPoint = "kvmpad_pair_client_transcript")]
    internal static partial int ClientTranscript(IntPtr handle, Span<byte> output, int outputCapacity);

    [LibraryImport(Library, EntryPoint = "kvmpad_pair_client_destroy")]
    internal static partial void ClientDestroy(IntPtr handle);

    /// <summary>The algorithm identifier, read from the library rather than written out here.</summary>
    internal static string PakeName => Marshal.PtrToStringUTF8(PakeNamePtr()) ?? string.Empty;

    /// <summary>
    /// The name of a result code, e.g. <c>BAD_CODE</c>. Safe to show and to log: the core's table carries no
    /// secret and says nothing about which half of the exchange was wrong.
    /// </summary>
    internal static string ErrorName(int err) => Marshal.PtrToStringUTF8(ErrorNamePtr(err)) ?? "UNKNOWN";

    /// <summary>Every length that crosses the boundary, taken from the core's own constants.</summary>
    internal static NativeSizes Sizes()
    {
        int rc = SizesRaw(
            out int codeLength, out int sha256Length, out int clientIdLength, out int tokenLength,
            out int tokenSealedLength, out int confirmLength, out int round1Max, out int round2Max,
            out int nameCap);
        if (rc != Ok)
            throw new PairingFailedException(PairingRejectionReason.Unknown, Describe("sizes", rc));

        return new NativeSizes(
            codeLength, sha256Length, clientIdLength, tokenLength, tokenSealedLength, confirmLength,
            round1Max, round2Max, nameCap);
    }

    /// <summary>
    /// Turns a result code into the reason the user is shown. The mapping is deliberately coarse on the
    /// failure side: everything that could tell an attacker whether the code or the certificate was wrong
    /// collapses onto one outcome (ADR-029 §5).
    /// </summary>
    internal static PairingRejectionReason ReasonFor(int err) => ErrorName(err) switch
    {
        "EXPIRED" => PairingRejectionReason.Expired,
        "LOCKED" => PairingRejectionReason.Locked,
        "REJECTED" => PairingRejectionReason.LocalReject,
        "BAD_CODE" => PairingRejectionReason.CodeMismatch,
        "BINDING" => PairingRejectionReason.ChannelBindingMismatch,
        _ => PairingRejectionReason.Unknown,
    };

    /// <summary>A message naming the step and the code, with nothing derived from a secret in it.</summary>
    internal static string Describe(string step, int err)
    {
        PairingRejectionReason reason = ReasonFor(err);
        return reason == PairingRejectionReason.Unknown
            ? $"The pairing core refused at {step} ({ErrorName(err)})."
            : PairingFailedException.Describe(reason);
    }

    /// <summary>Throws unless the call succeeded.</summary>
    internal static void Check(string step, int err)
    {
        if (err == Ok)
            return;
        throw new PairingFailedException(ReasonFor(err), Describe(step, err));
    }
}

/// <summary>The sizes the pairing core works in. All of them come from the library (ADR-029 §3, §4).</summary>
internal readonly struct NativeSizes
{
    internal NativeSizes(
        int codeLength, int sha256Length, int clientIdLength, int tokenLength, int tokenSealedLength,
        int confirmLength, int round1Max, int round2Max, int nameCap)
    {
        CodeLength = codeLength;
        Sha256Length = sha256Length;
        ClientIdLength = clientIdLength;
        TokenLength = tokenLength;
        TokenSealedLength = tokenSealedLength;
        ConfirmLength = confirmLength;
        Round1Max = round1Max;
        Round2Max = round2Max;
        NameCapacity = nameCap;
    }

    internal int CodeLength { get; }
    internal int Sha256Length { get; }
    internal int ClientIdLength { get; }
    internal int TokenLength { get; }
    internal int TokenSealedLength { get; }
    internal int ConfirmLength { get; }
    internal int Round1Max { get; }
    internal int Round2Max { get; }
    internal int NameCapacity { get; }
}
