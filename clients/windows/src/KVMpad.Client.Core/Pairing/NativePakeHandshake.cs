// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Security.Cryptography;
using System.Text;

namespace KVMpad.Client.Core.Pairing;

/// <summary>
/// The one implementation of <see cref="IPakeHandshake"/>: EC-JPAKE on P-256 through the pairing core that
/// the firmware compiles from the same sources (ADR-029 SEC-101).
/// </summary>
/// <remarks>
/// <para>
/// This type owns a native run and nothing else. It holds no key material of its own — the code, the derived
/// keys and the shared point stay inside the core, which zeroizes them on <see cref="Dispose"/> — and it adds
/// no stage checks of its own, because the core already enforces the call order and a second, slightly
/// different gate here would be a way for the two to disagree.
/// </para>
/// <para>
/// The native library is resolved by name, so <c>kvmpad_pairing.dll</c> has to sit beside the application.
/// A missing or wrong-architecture library surfaces as a <see cref="PairingUnavailableException"/> with the
/// build command in the message, rather than a bare <c>DllNotFoundException</c> from the first call.
/// </para>
/// </remarks>
public sealed class NativePakeHandshake : IPakeHandshake
{
    private IntPtr _handle;
    private bool _disposed;

    public string AlgorithmId { get; }

    public int WireVersion { get; }

    public NativePakeHandshake()
    {
        try
        {
            PairingNative.Check("startup", PairingNative.Startup());
            AlgorithmId = PairingNative.PakeName;
            WireVersion = PairingNative.WireVersion();
            Sizes = PairingNative.Sizes();
        }
        catch (DllNotFoundException ex)
        {
            throw new PairingUnavailableException(ex);
        }
        catch (BadImageFormatException ex)
        {
            throw new PairingUnavailableException(ex);
        }
        catch (EntryPointNotFoundException ex)
        {
            throw new PairingUnavailableException(ex);
        }

        if (AlgorithmId.Length == 0)
            throw new PairingUnavailableException(null);
    }

    /// <summary>The lengths the core works in, for the protocol driver's frame checks.</summary>
    internal NativeSizes Sizes { get; }

    /// <summary>True when the native library loads and answers. Used to decide what the Pair dialog offers.</summary>
    public static bool IsAvailable
    {
        get
        {
            try
            {
                using var probe = new NativePakeHandshake();
                return true;
            }
            catch (PairingUnavailableException)
            {
                return false;
            }
        }
    }

    public void Begin(PairingContext context)
    {
        if (context is null)
            throw new ArgumentNullException(nameof(context));
        ThrowIfDisposed();
        if (_handle != IntPtr.Zero)
            throw new InvalidOperationException("This handshake has already been started.");

        byte[] effectiveName = new byte[Sizes.NameCapacity];
        int rc = PairingNative.ClientCreate(
            context.Code, context.ClientName,
            context.ClientIdValue, context.ClientIdValue.Length,
            context.CertificateSha256Value, context.CertificateSha256Value.Length,
            effectiveName, effectiveName.Length,
            out IntPtr handle);
        PairingNative.Check("client_create", rc);

        _handle = handle;
        context.EffectiveClientName = ReadCString(effectiveName);
    }

    public byte[] Round1(byte[] deviceRound1) => Round(1, deviceRound1, Sizes.Round1Max);

    public byte[] Round2(byte[] deviceRound2) => Round(2, deviceRound2, Sizes.Round2Max);

    private byte[] Round(int round, byte[] deviceBlob, int capacity)
    {
        if (deviceBlob is null)
            throw new ArgumentNullException(nameof(deviceBlob));
        IntPtr handle = Live();

        byte[] buffer = new byte[capacity];
        int rc = round == 1
            ? PairingNative.ClientRound1(handle, deviceBlob, deviceBlob.Length, buffer, buffer.Length, out int produced)
            : PairingNative.ClientRound2(handle, deviceBlob, deviceBlob.Length, buffer, buffer.Length, out produced);
        PairingNative.Check($"round{round}", rc);

        return Trim(buffer, produced);
    }

    public byte[] ConfirmationOut()
    {
        IntPtr handle = Live();
        byte[] tag = new byte[Sizes.ConfirmLength];
        PairingNative.Check("confirm_out", PairingNative.ClientConfirmOut(handle, tag, tag.Length));
        return tag;
    }

    public void ConfirmationIn(byte[] deviceConfirmation)
    {
        if (deviceConfirmation is null)
            throw new ArgumentNullException(nameof(deviceConfirmation));
        IntPtr handle = Live();
        PairingNative.Check("confirm_in",
            PairingNative.ClientConfirmIn(handle, deviceConfirmation, deviceConfirmation.Length));
    }

    public byte[] OpenToken(byte[] sealedToken)
    {
        if (sealedToken is null)
            throw new ArgumentNullException(nameof(sealedToken));
        IntPtr handle = Live();
        byte[] token = new byte[Sizes.TokenLength];
        int rc = PairingNative.ClientOpenToken(handle, sealedToken, sealedToken.Length, token, token.Length);
        if (rc != PairingNative.Ok)
        {
            CryptographicOperations.ZeroMemory(token);
            PairingNative.Check("open_token", rc);
        }
        return token;
    }

    public byte[] Transcript()
    {
        IntPtr handle = Live();
        byte[] hash = new byte[Sizes.Sha256Length];
        PairingNative.Check("transcript", PairingNative.ClientTranscript(handle, hash, hash.Length));
        return hash;
    }

    private IntPtr Live()
    {
        ThrowIfDisposed();
        if (_handle == IntPtr.Zero)
            throw new InvalidOperationException("The handshake has not been started.");
        return _handle;
    }

    private void ThrowIfDisposed()
    {
        if (_disposed)
            throw new ObjectDisposedException(nameof(NativePakeHandshake));
    }

    private static byte[] Trim(byte[] buffer, int length)
    {
        byte[] exact = new byte[length];
        Array.Copy(buffer, exact, length);
        // The tail of the scratch buffer held part of a blob; blobs are public, but the buffer is wiped
        // anyway so that nothing from a run is left on a reused array.
        CryptographicOperations.ZeroMemory(buffer);
        return exact;
    }

    private static string ReadCString(byte[] buffer)
    {
        int end = Array.IndexOf(buffer, (byte)0);
        if (end < 0)
            end = buffer.Length;
        return Encoding.UTF8.GetString(buffer, 0, end);
    }

    public void Dispose()
    {
        if (_disposed)
            return;
        _disposed = true;
        if (_handle != IntPtr.Zero)
        {
            PairingNative.ClientDestroy(_handle); // destroys the PSA keys and zeroizes the run
            _handle = IntPtr.Zero;
        }
    }
}

/// <summary>
/// Raised when the native pairing library cannot be loaded, so pairing is unavailable in this build.
/// </summary>
/// <remarks>
/// It is a distinct type because the remedy is distinct: not "retry" and not "check the code", but build or
/// ship the library. The Pair dialog shows this message instead of offering a flow that cannot finish.
/// </remarks>
public sealed class PairingUnavailableException : Exception
{
    internal PairingUnavailableException(Exception? inner)
        : base(Text, inner)
    {
    }

    internal const string Text =
        "The pairing library (kvmpad_pairing.dll) is not available next to this application, so pairing "
        + "cannot run. Build it with: cmake -S clients/windows/native -B ../build-win-pairing -G Ninja "
        + "-DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang, then cmake --build ../build-win-pairing. "
        + "See clients/windows/README.md.";
}
