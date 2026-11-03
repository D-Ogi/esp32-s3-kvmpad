// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;

namespace KVMpad.Client.Core.Pairing;

/// <summary>
/// The EC-JPAKE half of one pairing run (ADR-029 SEC-101, SEC-102).
/// </summary>
/// <remarks>
/// <para>
/// This interface exists to be implemented once, by <c>NativePakeHandshake</c>, over the pairing core that
/// the firmware also compiles. It is not an extension point and it is not a place to put a second PAKE:
/// ADR-029 chose one implementation compiled twice precisely so that the two ends cannot drift, and SEC-101
/// forbids a construction of our own. The interface is here so the protocol driver in
/// <see cref="PairingClient"/> can be tested without a crypto library, and so the native boundary has one
/// seam instead of being scattered through the run.
/// </para>
/// <para>
/// Every method is called once, in the order below, and every failure throws
/// <see cref="PairingFailedException"/> rather than returning a code — a pairing run has exactly one
/// successful path and no partial outcome worth reporting.
/// </para>
/// <code>
/// Begin           -> pair.hello
/// Round1(device)  -> the CLIENT round-1 blob
/// Round2(device)  -> the CLIENT round-2 blob
/// ConfirmationOut -> pair.confirm, the CLIENT proves first (ADR-029 §3)
/// ConfirmationIn  -> verifies the DEVICE tag; throws on mismatch
/// OpenToken       -> the 32-byte token from pair.accepted
/// </code>
/// </remarks>
public interface IPakeHandshake : IDisposable
{
    /// <summary>The value <c>pair.hello</c> must carry in <c>pake</c>, e.g. <c>ecjpake-p256-sha256</c>.</summary>
    string AlgorithmId { get; }

    /// <summary>The value <c>pair.hello</c> must carry in <c>v</c>.</summary>
    int WireVersion { get; }

    /// <summary>
    /// Starts the run and fixes the channel binding. Sets <see cref="PairingContext.EffectiveClientName"/> to
    /// the possibly truncated name that entered the binding; <c>pair.hello</c> must carry that one.
    /// </summary>
    void Begin(PairingContext context);

    /// <summary>Consumes the device's round-1 blob and returns the client's.</summary>
    byte[] Round1(byte[] deviceRound1);

    /// <summary>Consumes the device's round-2 blob and returns the client's, completing the key exchange.</summary>
    byte[] Round2(byte[] deviceRound2);

    /// <summary>The client's key-confirmation tag, sent before the device reveals its own.</summary>
    byte[] ConfirmationOut();

    /// <summary>
    /// Verifies the device's tag in constant time. Throws with <see cref="PairingRejectionReason.CodeMismatch"/>
    /// on failure — which is the only signal a wrong code, a substituted certificate or a tampered transcript
    /// produces, and deliberately so.
    /// </summary>
    void ConfirmationIn(byte[] deviceConfirmation);

    /// <summary>
    /// Opens the sealed token from <c>pair.accepted</c>. Refuses to run before
    /// <see cref="ConfirmationIn"/> has passed, so material from an unauthenticated peer is never unsealed.
    /// </summary>
    byte[] OpenToken(byte[] sealedToken);

    /// <summary>The transcript hash. Not a secret; used for diagnostics and by the interop vectors.</summary>
    byte[] Transcript();
}
