// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;

namespace KVMpad.Client.Core.Protocol;

/// <summary>
/// Hard numbers from spec 03 §6 plus the client-side budgets this implementation keeps below them.
/// The spec values are the server's rejection thresholds; the client values are deliberately lower so
/// a correct client never trips a limit in normal use (03 §6.5, ADR-019 FR-704).
/// </summary>
public static class ProtocolLimits
{
    /// <summary>Envelope <c>v</c>. An unknown value is <c>ERR_VERSION</c> (03 §6.2).</summary>
    public const int Version = 1;

    /// <summary>WebSocket subprotocol offered on the control-session handshake (ADR-028).</summary>
    public const string SessionSubprotocol = "kvmpad.v1";

    /// <summary>WebSocket subprotocol offered on the pairing handshake (ADR-028, ADR-022 SEC-103).</summary>
    public const string PairingSubprotocol = "kvmpad.pair.v1";

    /// <summary>Request path of the authenticated control session (ADR-028).</summary>
    public const string SessionPath = "/v1/ws";

    /// <summary>Request path of the pairing endpoint, open only inside the pairing window (ADR-028).</summary>
    public const string PairingPath = "/v1/pair";

    /// <summary>mDNS/DNS-SD service type the device advertises (ADR-022 FR-723).</summary>
    public const string ServiceType = "_kvmpad._tcp.local";

    /// <summary><c>max_frame_size</c> from 03 §6.1. Longer frames are <c>ERR_BAD_FRAME</c>.</summary>
    public const int MaxFrameBytes = 1024;

    /// <summary>Server throttle threshold: 120 input msg/s in a rolling window (03 §6.5).</summary>
    public const int ServerInputMessagesPerSecond = 120;

    /// <summary>
    /// Ceiling this client enforces on itself for all input traffic. Below the server's 120/s so that
    /// clock skew between the two rolling windows cannot push us over (03 §6.5).
    /// </summary>
    public const int ClientInputMessagesPerSecond = 100;

    /// <summary>Default "type text" rate, in messages per second (ADR-019 FR-704: default ≤ 60).</summary>
    public const int DefaultTypeTextMessagesPerSecond = 60;

    /// <summary>Hard ceiling for "type text" (ADR-019 FR-704: never more than 100 msg/s).</summary>
    public const int MaxTypeTextMessagesPerSecond = 100;

    /// <summary>Default maximum length of one "type text" run, in characters (ADR-019 FR-704).</summary>
    public const int DefaultTypeTextMaxCharacters = 4096;

    /// <summary>Inclusive bound on a single <c>mouse.move</c> axis. Outside is <c>ERR_RANGE</c> (03 §6.5).</summary>
    public const int MouseDeltaLimit = 4096;

    /// <summary>Inclusive bound on one <c>mouse.wheel</c> axis step. Kept conservative; the spec says "integer".</summary>
    public const int WheelStepLimit = 127;

    /// <summary>Lowest keyboard usage the device accepts on page 0x07 (hid_keymap.h).</summary>
    public const byte MinKeyUsage = 0x04;

    /// <summary>Highest non-modifier keyboard usage the device accepts (hid_keymap.h KVM_KEY_LAST_REGULAR).</summary>
    public const byte MaxRegularKeyUsage = 0xA4;

    /// <summary>
    /// Heartbeat period. The device revokes a stale session after 3 s (AT-CONN-08), so a ping every
    /// 800 ms leaves room for two losses before the limit is anywhere near.
    /// </summary>
    public static readonly TimeSpan PingInterval = TimeSpan.FromMilliseconds(800);

    /// <summary>How long a missing <c>pong</c> is tolerated before the session counts as stale.</summary>
    public static readonly TimeSpan PongTimeout = TimeSpan.FromMilliseconds(2400);

    /// <summary><c>control.request</c> may wait this long for physical approval on the device (03 §6.5).</summary>
    public static readonly TimeSpan ControlApprovalTimeout = TimeSpan.FromSeconds(30);

    /// <summary>Whole pairing window, from the tap on <em>Pair client</em> to the endpoint closing (ADR-022 FR-722).</summary>
    public static readonly TimeSpan PairingWindow = TimeSpan.FromSeconds(180);
}
