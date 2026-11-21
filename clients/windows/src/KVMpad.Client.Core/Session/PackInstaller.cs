// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
using System;
using System.IO;
using System.Security.Cryptography;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using KVMpad.Client.Core.Protocol;

namespace KVMpad.Client.Core.Session;

/// <summary>One archive and one physically armed device over an existing pinned CLIENT transport.</summary>
public static class PackInstaller
{
    public const int MaxArchive = 256 * 1024;
    private sealed record Fields(Action<Utf8JsonWriter>? Write = null) : IClientPayload
    {
        public void WriteTo(Utf8JsonWriter writer)
        {
            writer.WriteStartObject();
            Write?.Invoke(writer);
            writer.WriteEndObject();
        }
    }

    public static Task InstallAsync(IMessageTransport transport, byte[] archive, CancellationToken cancellationToken)
        => InstallAsync(transport, archive, cancellationToken, null);

    /// <param name="onPhase">Receives transfer, review or committing when the device confirms that phase.</param>
    public static async Task InstallAsync(IMessageTransport transport, byte[] archive,
                                          CancellationToken cancellationToken, Action<string>? onPhase)
    {
        if (archive.Length is < 1 or > MaxArchive) throw new InvalidDataException("Invalid archive size.");
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        deadline.CancelAfter(TimeSpan.FromMinutes(8));
        CancellationToken ct = deadline.Token;
        ulong seq = 0, peerSeq = 0;
        string? label = null;
        async Task<ServerMessage> Exchange(string type, Fields fields)
        {
            // The board allows 160 pack requests/s per session. One request is outstanding at a time.
            if (seq != 0) await Task.Delay(8, ct).ConfigureAwait(false);
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
            timeout.CancelAfter(TimeSpan.FromSeconds(12));
            await transport.SendAsync(ProtocolCodec.Encode(type, ++seq, label, null, fields), timeout.Token)
                .ConfigureAwait(false);
            var bytes = await transport.ReceiveAsync(timeout.Token).ConfigureAwait(false);
            if (bytes is null) throw new IOException("Device disconnected.");
            var message = ProtocolCodec.Decode(bytes);
            if (message.Seq <= peerSeq || message.ReplyToSeq != seq ||
                (label != null && message.SessionId != label))
                throw new InvalidDataException("Unexpected transfer reply.");
            peerSeq = message.Seq;
            return message;
        }

        async Task WaitFor(string type, Fields fields, int offset, int length = 0, bool final = false)
        {
            string? lastPhase = null;
            while (true)
            {
                var reply = await Exchange(type, fields).ConfigureAwait(false);
                string? state = reply.PayloadString("state");
                if (reply.Type != "pack.status") throw new IOException("The device rejected the transfer protocol.");
                if (state is "pending" or "ready" or "review" or "committing" or "installed" &&
                    reply.PayloadInt64("offset") != (state == "pending" ? offset : offset + length))
                    throw new InvalidDataException("Unexpected transfer offset.");
                if (state == (final ? "installed" : "ready")) return;
                if ((state == "pending" && lastPhase != null) || (state == "review" && lastPhase == "committing"))
                    throw new InvalidDataException("Unexpected device transfer state.");
                if (state != "pending" && !(final && state is "review" or "committing"))
                    throw new IOException(state switch {
                        "closed" => "Open Receive from paired client on the device and try again.",
                        "busy" => "Another transfer owns the device.",
                        "expired" => "The device transfer window expired.",
                        "denied" => "Installation denied on the device.",
                        "cancelled" => "Transfer cancelled on the device.",
                        "failed" => "The device could not validate or install the archive.",
                        _ => "Unexpected device transfer state."
                    });
                if (state is "review" or "committing" && state != lastPhase)
                {
                    lastPhase = state;
                    onPhase?.Invoke(state);
                }
                if (final) await Task.Delay(150, ct).ConfigureAwait(false);
                type = "pack.poll";
                fields = new Fields();
            }
        }

        try
        {
            using var connect = CancellationTokenSource.CreateLinkedTokenSource(ct);
            connect.CancelAfter(TimeSpan.FromSeconds(12));
            await transport.ConnectAsync(connect.Token).ConfigureAwait(false);
            if (transport.NegotiatedSubprotocol != ProtocolLimits.SessionSubprotocol)
                throw new IOException("The device did not negotiate the required CLIENT protocol.");
            var hello = await Exchange("hello", new Fields(w => w.WriteString("client_name", "Extension Studio")))
                .ConfigureAwait(false);
            label = hello.SessionId;
            if (hello.Type != "hello.ack" || string.IsNullOrEmpty(label)) throw new IOException("Device hello failed.");
            await WaitFor("pack.offer", new Fields(w => {
                w.WriteNumber("size", archive.Length);
                w.WriteString("sha256", Convert.ToHexString(SHA256.HashData(archive)));
            }), 0).ConfigureAwait(false);
            onPhase?.Invoke("transfer");
            for (int offset = 0; offset < archive.Length;)
            {
                int length = Math.Min(256, archive.Length - offset);
                await WaitFor("pack.data", new Fields(w => {
                    w.WriteNumber("offset", offset);
                    w.WriteString("hex", Convert.ToHexString(archive, offset, length));
                }), offset, length).ConfigureAwait(false);
                offset += length;
            }
            await WaitFor("pack.end", new Fields(), archive.Length, final: true).ConfigureAwait(false);
        }
        finally
        {
            // Closing this non-HID connection revokes any outstanding transfer and pending review.
            using var close = new CancellationTokenSource(TimeSpan.FromSeconds(2));
            try { await transport.CloseAsync(close.Token).ConfigureAwait(false); }
            catch (Exception e) when (e is IOException or OperationCanceledException or System.Net.WebSockets.WebSocketException) { }
        }
    }
}
