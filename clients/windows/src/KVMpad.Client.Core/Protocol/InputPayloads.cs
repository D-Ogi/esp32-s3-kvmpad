// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Text.Json;
using KVMpad.Client.Core.Input;

namespace KVMpad.Client.Core.Protocol;

/// <summary>A payload this client can serialize. Payloads are flat: nothing nests recursively (03 §6.2).</summary>
public interface IClientPayload
{
    void WriteTo(Utf8JsonWriter writer);
}

/// <summary>
/// An <c>input</c> payload. <see cref="IsCoalescable"/> marks the one kind the rate limiter may merge
/// (<c>mouse.move</c>); everything else is an edge the host must see exactly once (03 §6.5).
/// </summary>
public interface IInputPayload : IClientPayload
{
    string Kind { get; }
    bool IsCoalescable { get; }
}

/// <summary><c>input</c>/<c>mouse.move</c>: relative motion, each axis within ±4096 (03 §6.5).</summary>
public readonly struct MouseMovePayload : IInputPayload
{
    public MouseMovePayload(int dx, int dy)
    {
        if (dx < -ProtocolLimits.MouseDeltaLimit || dx > ProtocolLimits.MouseDeltaLimit)
            throw new ArgumentOutOfRangeException(nameof(dx), dx, "dx outside the protocol range; split instead of clipping.");
        if (dy < -ProtocolLimits.MouseDeltaLimit || dy > ProtocolLimits.MouseDeltaLimit)
            throw new ArgumentOutOfRangeException(nameof(dy), dy, "dy outside the protocol range; split instead of clipping.");
        Dx = dx;
        Dy = dy;
    }

    public int Dx { get; }
    public int Dy { get; }

    public string Kind => InputKinds.MouseMove;
    public bool IsCoalescable => true;

    public void WriteTo(Utf8JsonWriter writer)
    {
        writer.WriteStartObject();
        writer.WriteString("kind", InputKinds.MouseMove);
        writer.WriteNumber("dx", Dx);
        writer.WriteNumber("dy", Dy);
        writer.WriteEndObject();
    }
}

/// <summary><c>input</c>/<c>mouse.button</c>. The device keeps the bitmask; a move never clears it (03 §5.3).</summary>
public readonly struct MouseButtonPayload : IInputPayload
{
    public MouseButtonPayload(string button, bool down)
    {
        if (button != MouseButtons.Left && button != MouseButtons.Right && button != MouseButtons.Middle)
            throw new ArgumentOutOfRangeException(nameof(button), button, "Unknown mouse button.");
        Button = button;
        Down = down;
    }

    public string Button { get; }
    public bool Down { get; }

    public string Kind => InputKinds.MouseButton;
    public bool IsCoalescable => false;

    public void WriteTo(Utf8JsonWriter writer)
    {
        writer.WriteStartObject();
        writer.WriteString("kind", InputKinds.MouseButton);
        writer.WriteString("button", Button);
        writer.WriteBoolean("down", Down);
        writer.WriteEndObject();
    }
}

/// <summary><c>input</c>/<c>mouse.wheel</c>. Horizontal reaches the host as AC Pan (ADR-018).</summary>
public readonly struct MouseWheelPayload : IInputPayload
{
    public MouseWheelPayload(int vertical, int horizontal)
    {
        Vertical = vertical;
        Horizontal = horizontal;
    }

    public int Vertical { get; }
    public int Horizontal { get; }

    public string Kind => InputKinds.MouseWheel;
    public bool IsCoalescable => false;

    public void WriteTo(Utf8JsonWriter writer)
    {
        writer.WriteStartObject();
        writer.WriteString("kind", InputKinds.MouseWheel);
        writer.WriteNumber("vertical", Vertical);
        writer.WriteNumber("horizontal", Horizontal);
        writer.WriteEndObject();
    }
}

/// <summary>
/// <c>input</c>/<c>keyboard.key</c>: a usage from page 0x07, never a character code (03 §5.2, FR-743).
/// This is also the only channel "type text" uses (ADR-019 FR-701).
/// </summary>
public readonly struct KeyboardKeyPayload : IInputPayload
{
    public KeyboardKeyPayload(byte usageId, bool down)
    {
        if (!HidUsage.IsValidKey(usageId))
            throw new ArgumentOutOfRangeException(nameof(usageId), usageId, "Usage outside the device's accepted range.");
        UsageId = usageId;
        Down = down;
    }

    public byte UsageId { get; }
    public bool Down { get; }

    public string Kind => InputKinds.KeyboardKey;
    public bool IsCoalescable => false;

    public void WriteTo(Utf8JsonWriter writer)
    {
        writer.WriteStartObject();
        writer.WriteString("kind", InputKinds.KeyboardKey);
        writer.WriteNumber("usage_id", UsageId);
        writer.WriteBoolean("down", Down);
        writer.WriteEndObject();
    }
}

/// <summary>
/// <c>input</c>/<c>shortcut</c>: an allowlisted <c>command_id</c> only. The client never sends a key array
/// for a chord, so it cannot invent a combination the device has not approved (03 §3 rule 2, TECH-045).
/// </summary>
public readonly struct ShortcutPayload : IInputPayload
{
    public ShortcutPayload(string commandId)
    {
        if (!ChordIds.IsAllowlisted(commandId))
            throw new ArgumentOutOfRangeException(nameof(commandId), commandId, "command_id is not on the chord allowlist.");
        CommandId = commandId;
    }

    public string CommandId { get; }

    public string Kind => InputKinds.Shortcut;
    public bool IsCoalescable => false;

    public void WriteTo(Utf8JsonWriter writer)
    {
        writer.WriteStartObject();
        writer.WriteString("kind", InputKinds.Shortcut);
        writer.WriteString("command_id", CommandId);
        writer.WriteEndObject();
    }
}

/// <summary><c>hello</c> payload (03 §6.3). It carries no token: authentication is the handshake (ADR-028).</summary>
public readonly struct HelloPayload : IClientPayload
{
    public HelloPayload(string clientName, string[] capabilities)
    {
        ClientName = clientName ?? throw new ArgumentNullException(nameof(clientName));
        Capabilities = capabilities ?? throw new ArgumentNullException(nameof(capabilities));
    }

    public string ClientName { get; }
    public string[] Capabilities { get; }

    public void WriteTo(Utf8JsonWriter writer)
    {
        writer.WriteStartObject();
        writer.WriteString("client_name", ClientName);
        writer.WriteStartArray("capabilities");
        foreach (string capability in Capabilities)
            writer.WriteStringValue(capability);
        writer.WriteEndArray();
        writer.WriteEndObject();
    }
}

/// <summary><c>control.request</c> payload (03 §6.3).</summary>
public readonly struct ControlRequestPayload : IClientPayload
{
    public ControlRequestPayload(string reason) => Reason = reason ?? throw new ArgumentNullException(nameof(reason));

    public string Reason { get; }

    public void WriteTo(Utf8JsonWriter writer)
    {
        writer.WriteStartObject();
        writer.WriteString("reason", Reason);
        writer.WriteEndObject();
    }
}

/// <summary><c>ping</c> payload (03 §6.3). The nonce lets the client match the <c>pong</c>.</summary>
public readonly struct PingPayload : IClientPayload
{
    public PingPayload(long nonce) => Nonce = nonce;

    public long Nonce { get; }

    public void WriteTo(Utf8JsonWriter writer)
    {
        writer.WriteStartObject();
        writer.WriteNumber("nonce", Nonce);
        writer.WriteEndObject();
    }
}

/// <summary>An empty <c>{}</c> payload, for <c>status.get</c>, <c>control.release</c> and <c>session.close</c>.</summary>
public readonly struct EmptyPayload : IClientPayload
{
    public void WriteTo(Utf8JsonWriter writer)
    {
        writer.WriteStartObject();
        writer.WriteEndObject();
    }
}
