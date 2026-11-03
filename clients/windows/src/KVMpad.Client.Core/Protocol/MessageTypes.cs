// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

namespace KVMpad.Client.Core.Protocol;

/// <summary>Envelope <c>type</c> values. Both directions are allowlisted; nothing unknown is ignored silently (03 §6.2).</summary>
public static class MessageTypes
{
    // client -> device (03 §6.3)
    public const string Hello = "hello";
    public const string StatusGet = "status.get";
    public const string ControlRequest = "control.request";
    public const string ControlRelease = "control.release";
    public const string Ping = "ping";
    public const string Input = "input";
    public const string SessionClose = "session.close";

    // device -> client (03 §6.4)
    public const string HelloAck = "hello.ack";
    public const string StatusSnapshot = "status.snapshot";
    public const string ControlPending = "control.pending";
    public const string ControlGranted = "control.granted";
    public const string ControlRevoked = "control.revoked";
    public const string Pong = "pong";
    public const string InputAck = "input.ack";
    public const string Error = "error";

    // pairing endpoint (ADR-022 "Protocol" table); the wire payloads land with ADR-029
    public const string PairHello = "pair.hello";
    public const string PairRound1 = "pair.round1";
    public const string PairRound2 = "pair.round2";
    public const string PairConfirm = "pair.confirm";
    public const string PairAccepted = "pair.accepted";
    public const string PairRejected = "pair.rejected";
}

/// <summary><c>input</c> payload <c>kind</c> values (03 §6.3).</summary>
public static class InputKinds
{
    public const string MouseMove = "mouse.move";
    public const string MouseButton = "mouse.button";
    public const string MouseWheel = "mouse.wheel";
    public const string KeyboardKey = "keyboard.key";
    public const string Shortcut = "shortcut";
}

/// <summary>Mouse button names accepted by <c>input</c>/<c>mouse.button</c> (03 §6.3).</summary>
public static class MouseButtons
{
    public const string Left = "left";
    public const string Right = "right";
    public const string Middle = "middle";
}
