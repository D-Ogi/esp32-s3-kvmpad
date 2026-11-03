// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Buffers;
using System.Text;
using System.Text.Json;

namespace KVMpad.Client.Core.Protocol;

/// <summary>Raised when a frame cannot be built or parsed inside the limits of 03 §6.</summary>
public sealed class ProtocolFrameException : Exception
{
    public ProtocolFrameException(string message) : base(message) { }
}

/// <summary>
/// Builds and parses protocol v1 envelopes (03 §6.2). Field order on the wire is the order the spec
/// prints, so a frame this codec produces matches the spec's example byte for byte.
/// </summary>
public static class ProtocolCodec
{
    private static readonly JsonWriterOptions WriterOptions = new()
    {
        Indented = false,
        SkipValidation = false,
    };

    /// <summary>
    /// Serializes one envelope to UTF-8 JSON.
    /// </summary>
    /// <param name="type">Allowlisted envelope type.</param>
    /// <param name="seq">Monotonic per-session sequence number (03 §6.2).</param>
    /// <param name="sessionId">Non-secret server label; omitted before <c>hello.ack</c> assigns one.</param>
    /// <param name="leaseEpoch">Mandatory for <c>input</c>, omitted otherwise.</param>
    /// <param name="payload">Typed payload, or null for a payload-less envelope.</param>
    /// <exception cref="ProtocolFrameException">The frame would exceed <c>max_frame_size</c>.</exception>
    public static byte[] Encode(string type, ulong seq, string? sessionId, uint? leaseEpoch, IClientPayload? payload)
    {
        if (string.IsNullOrEmpty(type))
            throw new ArgumentException("Envelope type is required.", nameof(type));
        if (seq > (1UL << 53) - 1)
            throw new ArgumentOutOfRangeException(nameof(seq), seq, "seq must stay a safe JSON integer (03 §6.2).");

        var buffer = new ArrayBufferWriter<byte>(256);
        using (var writer = new Utf8JsonWriter(buffer, WriterOptions))
        {
            writer.WriteStartObject();
            writer.WriteNumber("v", ProtocolLimits.Version);
            writer.WriteString("type", type);
            writer.WriteNumber("seq", seq);
            if (sessionId is not null)
                writer.WriteString("session_id", sessionId);
            if (leaseEpoch.HasValue)
                writer.WriteNumber("lease_epoch", leaseEpoch.Value);
            if (payload is not null)
            {
                writer.WritePropertyName("payload");
                payload.WriteTo(writer);
            }
            writer.WriteEndObject();
        }

        byte[] frame = buffer.WrittenSpan.ToArray();
        if (frame.Length > ProtocolLimits.MaxFrameBytes)
            throw new ProtocolFrameException(
                $"Frame of {frame.Length} B exceeds max_frame_size={ProtocolLimits.MaxFrameBytes} B (03 §6.1).");
        return frame;
    }

    /// <summary>Convenience overload for tests and logs; the transport always sends the UTF-8 bytes.</summary>
    public static string EncodeToString(string type, ulong seq, string? sessionId, uint? leaseEpoch, IClientPayload? payload)
        => Encoding.UTF8.GetString(Encode(type, seq, sessionId, leaseEpoch, payload));

    /// <summary>
    /// Parses one inbound frame. The client validates as strictly as the device does: an over-long frame,
    /// malformed JSON, a wrong <c>v</c> or a missing <c>type</c> is rejected rather than guessed at
    /// (03 §6.5, TECH-043 read from the client's side).
    /// </summary>
    public static ServerMessage Decode(ReadOnlySpan<byte> frame)
    {
        if (frame.Length == 0)
            throw new ProtocolFrameException("Empty frame.");
        if (frame.Length > ProtocolLimits.MaxFrameBytes)
            throw new ProtocolFrameException(
                $"Frame of {frame.Length} B exceeds max_frame_size={ProtocolLimits.MaxFrameBytes} B (03 §6.1).");

        JsonDocument document;
        try
        {
            var reader = new Utf8JsonReader(frame, new JsonReaderOptions { MaxDepth = 8 });
            document = JsonDocument.ParseValue(ref reader);
        }
        catch (JsonException ex)
        {
            throw new ProtocolFrameException("Malformed JSON frame: " + ex.Message);
        }

        using (document)
        {
            JsonElement root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
                throw new ProtocolFrameException("Envelope must be a JSON object.");

            if (!root.TryGetProperty("v", out JsonElement versionElement)
                || versionElement.ValueKind != JsonValueKind.Number
                || !versionElement.TryGetInt32(out int version))
                throw new ProtocolFrameException("Envelope has no integer v.");
            if (version != ProtocolLimits.Version)
                throw new ProtocolFrameException($"Unsupported protocol version {version} (expected {ProtocolLimits.Version}).");

            if (!root.TryGetProperty("type", out JsonElement typeElement) || typeElement.ValueKind != JsonValueKind.String)
                throw new ProtocolFrameException("Envelope has no string type.");
            string type = typeElement.GetString()!;

            ulong seq = 0;
            if (root.TryGetProperty("seq", out JsonElement seqElement) && seqElement.ValueKind == JsonValueKind.Number)
                seqElement.TryGetUInt64(out seq);

            string? sessionId = root.TryGetProperty("session_id", out JsonElement sidElement)
                && sidElement.ValueKind == JsonValueKind.String
                    ? sidElement.GetString()
                    : null;

            uint? leaseEpoch = null;
            if (root.TryGetProperty("lease_epoch", out JsonElement epochElement)
                && epochElement.ValueKind == JsonValueKind.Number
                && epochElement.TryGetUInt32(out uint epoch))
                leaseEpoch = epoch;

            ulong? replyToSeq = null;
            if (root.TryGetProperty("reply_to_seq", out JsonElement replyElement)
                && replyElement.ValueKind == JsonValueKind.Number
                && replyElement.TryGetUInt64(out ulong reply))
                replyToSeq = reply;

            long? serverTimestampUs = null;
            if (root.TryGetProperty("server_timestamp_us", out JsonElement tsElement)
                && tsElement.ValueKind == JsonValueKind.Number
                && tsElement.TryGetInt64(out long ts))
                serverTimestampUs = ts;

            JsonElement? payload = root.TryGetProperty("payload", out JsonElement payloadElement)
                ? payloadElement.Clone()
                : null;

            return new ServerMessage(type, seq, sessionId, leaseEpoch, replyToSeq, serverTimestampUs, payload);
        }
    }

    /// <summary>Parses a frame given as text (the transport negotiates UTF-8 text frames, 03 §6.1).</summary>
    public static ServerMessage Decode(string frame) => Decode(Encoding.UTF8.GetBytes(frame));
}
