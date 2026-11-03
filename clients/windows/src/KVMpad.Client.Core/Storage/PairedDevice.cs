// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Text;
using System.Text.Json;
using KVMpad.Client.Core.Session;

namespace KVMpad.Client.Core.Storage;

/// <summary>
/// What pairing leaves on this machine: the device's name, where to reach it, the pinned certificate
/// fingerprint and the token.
/// </summary>
/// <remarks>
/// The token and the pin belong in the Windows credential store, never in a configuration file
/// (ADR-022 SEC-105). <see cref="ToJson"/> therefore produces the blob that goes into a credential, and the
/// non-secret fields are carried along inside it rather than being duplicated somewhere readable.
/// </remarks>
public sealed class PairedDevice
{
    public PairedDevice(
        string deviceId,
        string deviceName,
        string host,
        int port,
        string pinnedFingerprint,
        string token,
        DateTimeOffset pairedAt)
    {
        if (string.IsNullOrWhiteSpace(deviceId))
            throw new ArgumentException("A device id is required.", nameof(deviceId));
        if (!CertificateFingerprint.IsWellFormed(pinnedFingerprint))
            throw new ArgumentException("The pinned fingerprint must be 64 hex characters.", nameof(pinnedFingerprint));
        if (string.IsNullOrEmpty(token))
            throw new ArgumentException("A token is required.", nameof(token));

        DeviceId = deviceId;
        DeviceName = deviceName;
        Host = host;
        Port = port;
        PinnedFingerprint = pinnedFingerprint.ToLowerInvariant();
        Token = token;
        PairedAt = pairedAt;
    }

    /// <summary>A stable key for the credential entry: the mDNS instance label, e.g. <c>KVMpad-7F3A</c>.</summary>
    public string DeviceId { get; }

    public string DeviceName { get; }

    /// <summary>Last known address or host name. Re-discovered over mDNS when it changes.</summary>
    public string Host { get; }

    public int Port { get; }

    /// <summary>SHA-256 of the certificate approved at pairing. The session accepts no other (SEC-102).</summary>
    public string PinnedFingerprint { get; }

    /// <summary>The pairing token, base64url. A secret.</summary>
    public string Token { get; }

    public DateTimeOffset PairedAt { get; }

    /// <summary>A copy with a new address, for when mDNS reports the device has moved.</summary>
    public PairedDevice WithEndpoint(string host, int port)
        => new(DeviceId, DeviceName, host, port, PinnedFingerprint, Token, PairedAt);

    public SessionEndpoint ToEndpoint() => new(Host, Port, PinnedFingerprint, Token);

    /// <summary>Serializes the record for the credential blob.</summary>
    public byte[] ToJson()
    {
        var buffer = new System.Buffers.ArrayBufferWriter<byte>(256);
        using (var writer = new Utf8JsonWriter(buffer))
        {
            writer.WriteStartObject();
            writer.WriteNumber("schema", 1);
            writer.WriteString("device_id", DeviceId);
            writer.WriteString("device_name", DeviceName);
            writer.WriteString("host", Host);
            writer.WriteNumber("port", Port);
            writer.WriteString("pin_sha256", PinnedFingerprint);
            writer.WriteString("token", Token);
            writer.WriteString("paired_at", PairedAt.ToString("O"));
            writer.WriteEndObject();
        }
        return buffer.WrittenSpan.ToArray();
    }

    /// <summary>Reads a record back. A blob that does not parse is treated as absent, never as partially valid.</summary>
    public static PairedDevice? FromJson(ReadOnlySpan<byte> json)
    {
        try
        {
            using JsonDocument document = JsonDocument.Parse(Encoding.UTF8.GetString(json));
            JsonElement root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
                return null;

            string? deviceId = Text(root, "device_id");
            string? pin = Text(root, "pin_sha256");
            string? token = Text(root, "token");
            if (deviceId is null || pin is null || token is null)
                return null;

            int port = root.TryGetProperty("port", out JsonElement portElement)
                && portElement.ValueKind == JsonValueKind.Number
                && portElement.TryGetInt32(out int parsedPort)
                    ? parsedPort
                    : 0;

            DateTimeOffset pairedAt = DateTimeOffset.TryParse(Text(root, "paired_at"), out DateTimeOffset parsed)
                ? parsed
                : DateTimeOffset.MinValue;

            return new PairedDevice(
                deviceId,
                Text(root, "device_name") ?? deviceId,
                Text(root, "host") ?? string.Empty,
                port,
                pin,
                token,
                pairedAt);
        }
        catch (JsonException)
        {
            return null;
        }
        catch (ArgumentException)
        {
            return null;
        }
    }

    private static string? Text(JsonElement root, string name)
        => root.TryGetProperty(name, out JsonElement element) && element.ValueKind == JsonValueKind.String
            ? element.GetString()
            : null;

    /// <summary>A line for the UI and the logs: name, address and when it was paired, never the token or the pin.</summary>
    public override string ToString() => $"{DeviceName} ({Host}:{Port}), paired {PairedAt:yyyy-MM-dd}";
}
