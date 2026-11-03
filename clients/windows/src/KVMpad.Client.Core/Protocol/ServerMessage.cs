// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Text.Json;

namespace KVMpad.Client.Core.Protocol;

/// <summary>
/// A parsed device → client envelope (03 §6.4). <see cref="Seq"/> is the device's own counter, not a copy
/// of the request's; correlation, when it matters, uses <see cref="ReplyToSeq"/>.
/// </summary>
public sealed class ServerMessage
{
    public ServerMessage(
        string type,
        ulong seq,
        string? sessionId,
        uint? leaseEpoch,
        ulong? replyToSeq,
        long? serverTimestampUs,
        JsonElement? payload)
    {
        Type = type;
        Seq = seq;
        SessionId = sessionId;
        LeaseEpoch = leaseEpoch;
        ReplyToSeq = replyToSeq;
        ServerTimestampUs = serverTimestampUs;
        Payload = payload;
    }

    public string Type { get; }
    public ulong Seq { get; }
    public string? SessionId { get; }
    public uint? LeaseEpoch { get; }
    public ulong? ReplyToSeq { get; }
    public long? ServerTimestampUs { get; }
    public JsonElement? Payload { get; }

    public string? PayloadString(string name)
        => Payload.HasValue
            && Payload.Value.ValueKind == JsonValueKind.Object
            && Payload.Value.TryGetProperty(name, out JsonElement element)
            && element.ValueKind == JsonValueKind.String
                ? element.GetString()
                : null;

    public long? PayloadInt64(string name)
        => Payload.HasValue
            && Payload.Value.ValueKind == JsonValueKind.Object
            && Payload.Value.TryGetProperty(name, out JsonElement element)
            && element.ValueKind == JsonValueKind.Number
            && element.TryGetInt64(out long value)
                ? value
                : null;

    public bool? PayloadBool(string name)
    {
        if (!Payload.HasValue
            || Payload.Value.ValueKind != JsonValueKind.Object
            || !Payload.Value.TryGetProperty(name, out JsonElement element))
            return null;
        return element.ValueKind switch
        {
            JsonValueKind.True => true,
            JsonValueKind.False => false,
            _ => null,
        };
    }

    /// <summary>The <c>error</c> payload's <c>code</c>, or null when this is not an error.</summary>
    public string? ErrorCode => Type == MessageTypes.Error ? PayloadString("code") : null;

    /// <summary>The <c>error</c> payload's <c>retryable</c> flag; absent counts as not retryable.</summary>
    public bool ErrorRetryable => PayloadBool("retryable") ?? false;
}

/// <summary>
/// The device's view of the world, from <c>status.snapshot</c> (03 §6.6). The client renders this and
/// nothing of its own invention: the UI is not the source of truth for host, ownership or Wi-Fi state.
/// </summary>
public sealed class StatusSnapshot
{
    public string HostState { get; init; } = "UNKNOWN";
    public string HostTransport { get; init; } = string.Empty;
    public string ClientState { get; init; } = "UNKNOWN";
    public string ClientRole { get; init; } = string.Empty;
    public string ControlOwner { get; init; } = "NONE";
    public uint? ControlLeaseEpoch { get; init; }
    public string DeviceWifi { get; init; } = string.Empty;
    public bool DeviceMaintenance { get; init; }
    public string DeviceFirmware { get; init; } = string.Empty;

    /// <summary>Active host layout id, when the device publishes one (FR-745). Null until TASK-071 lands.</summary>
    public string? HostLayout { get; init; }

    /// <summary>Active control-approval policy, for the status detail line (ADR-023 FR-735).</summary>
    public string? ControlApproval { get; init; }

    /// <summary>True only when the device itself reports an authenticated client session (AT-CONN-04).</summary>
    public bool IsClientConnected => ClientState == "CLIENT_CONNECTED";

    /// <summary>True when the device reports this client as the control owner.</summary>
    public bool IsRemoteOwner => ControlOwner == "REMOTE";

    /// <summary>True when the host is mounted; without it the device refuses input with <c>ERR_HOST_OFFLINE</c>.</summary>
    public bool IsHostMounted => HostState == "USB_MOUNTED";

    public static StatusSnapshot Parse(ServerMessage message)
    {
        if (message.Type != MessageTypes.StatusSnapshot)
            throw new ArgumentException("Not a status.snapshot envelope.", nameof(message));
        if (!message.Payload.HasValue || message.Payload.Value.ValueKind != JsonValueKind.Object)
            throw new ProtocolFrameException("status.snapshot without a payload object.");

        JsonElement payload = message.Payload.Value;
        JsonElement host = Section(payload, "host");
        JsonElement client = Section(payload, "client");
        JsonElement control = Section(payload, "control");
        JsonElement device = Section(payload, "device");

        uint? epoch = null;
        if (control.ValueKind == JsonValueKind.Object
            && control.TryGetProperty("lease_epoch", out JsonElement epochElement)
            && epochElement.ValueKind == JsonValueKind.Number
            && epochElement.TryGetUInt32(out uint parsedEpoch))
            epoch = parsedEpoch;

        return new StatusSnapshot
        {
            HostState = Text(host, "state") ?? "UNKNOWN",
            HostTransport = Text(host, "transport") ?? string.Empty,
            ClientState = Text(client, "state") ?? "UNKNOWN",
            ClientRole = Text(client, "role") ?? string.Empty,
            ControlOwner = Text(control, "owner") ?? "NONE",
            ControlLeaseEpoch = epoch,
            ControlApproval = Text(control, "approval"),
            DeviceWifi = Text(device, "wifi") ?? string.Empty,
            DeviceMaintenance = Flag(device, "maintenance"),
            DeviceFirmware = Text(device, "fw") ?? string.Empty,
            HostLayout = Text(device, "host_layout"),
        };
    }

    private static JsonElement Section(JsonElement payload, string name)
        => payload.TryGetProperty(name, out JsonElement section) ? section : default;

    private static string? Text(JsonElement section, string name)
        => section.ValueKind == JsonValueKind.Object
            && section.TryGetProperty(name, out JsonElement element)
            && element.ValueKind == JsonValueKind.String
                ? element.GetString()
                : null;

    private static bool Flag(JsonElement section, string name)
        => section.ValueKind == JsonValueKind.Object
            && section.TryGetProperty(name, out JsonElement element)
            && element.ValueKind == JsonValueKind.True;
}
