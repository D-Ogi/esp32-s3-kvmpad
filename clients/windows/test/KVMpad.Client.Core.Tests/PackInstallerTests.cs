// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using KVMpad.Client.Core.Protocol;
using KVMpad.Client.Core.Session;
using Xunit;

namespace KVMpad.Client.Core.Tests;

public sealed class PackInstallerTests
{
    private sealed class Link : IMessageTransport
    {
        public bool IsOpen { get; private set; }
        public string? NegotiatedSubprotocol { get; set; } = ProtocolLimits.SessionSubprotocol;
        public readonly List<string> Types = [];
        public readonly MemoryStream Bytes = new();
        public string[] FinalStates = ["review", "review", "committing", "committing", "installed"];
        public string? FaultAt, Fault;
        public string LastState = "";
        public long LastOffset;
        public byte[] Digest = [];
        private byte[]? _reply;
        private string _state = "";
        private int _finalPolls;
        private int _size;

        public Task ConnectAsync(CancellationToken token) { IsOpen = true; return Task.CompletedTask; }
        public Task CloseAsync(CancellationToken token) { IsOpen = false; return Task.CompletedTask; }
        public ValueTask DisposeAsync() { IsOpen = false; return ValueTask.CompletedTask; }
        public Task<byte[]?> ReceiveAsync(CancellationToken token) => Task.FromResult(_reply);

        public Task SendAsync(ReadOnlyMemory<byte> frame, CancellationToken token)
        {
            Assert.True(frame.Length <= 1024);
            using var doc = JsonDocument.Parse(frame);
            var msg = doc.RootElement;
            var type = msg.GetProperty("type").GetString()!;
            Types.Add(type);
            ulong seq = msg.GetProperty("seq").GetUInt64();
            var p = msg.GetProperty("payload");
            string response = "pack.status", reported = "pending";
            long offset = Bytes.Length;
            if (type != "hello") Assert.Equal("sid-1", msg.GetProperty("session_id").GetString());
            switch (type)
            {
                case "hello":
                    response = "hello.ack";
                    break;
                case "pack.offer":
                    _size = p.GetProperty("size").GetInt32();
                    Digest = Convert.FromHexString(p.GetProperty("sha256").GetString()!);
                    _state = "ready";
                    break;
                case "pack.data":
                    Assert.Equal(Bytes.Length, p.GetProperty("offset").GetInt32());
                    byte[] data = Convert.FromHexString(p.GetProperty("hex").GetString()!);
                    Assert.InRange(data.Length, 1, 256);
                    Bytes.Write(data);
                    break;
                case "pack.end":
                    Assert.Equal(_size, Bytes.Length);
                    Assert.Equal(Digest, SHA256.HashData(Bytes.ToArray()));
                    _state = "review";
                    break;
                case "pack.poll":
                    // The receiver acknowledges a data chunk before its ready reply advances the offset.
                    reported = _state == "review" ? FinalStates[_finalPolls++] : _state;
                    break;
                default:
                    throw new InvalidOperationException("Unexpected non-transfer action.");
            }
            string? fault = FaultAt == (type == "hello" ? type : reported) ? Fault : null;
            LastState = reported;
            LastOffset = offset;
            _reply = JsonSerializer.SerializeToUtf8Bytes(new {
                v = 1, type = fault == "type" ? "hello.ack" : response, seq = fault == "seq" ? seq - 1 : seq,
                session_id = fault == "session" ? "sid-2" : "sid-1", reply_to_seq = fault == "reply" ? seq+1 : seq,
                payload = new { state = reported, offset = fault == "offset" ? offset + 1 : offset }
            });
            return Task.CompletedTask;
        }
    }

    [Fact]
    public async Task CompleteRequiresVerifiedLocalCommit()
    {
        var link = new Link();
        var bytes = new byte[777];
        for (int i = 0; i < bytes.Length; i++) bytes[i] = (byte)i;
        var phases = new List<(string, string, long)>();
        await PackInstaller.InstallAsync(link, bytes, CancellationToken.None,
            phase => phases.Add((phase, link.LastState, link.LastOffset)));
        Assert.Equal([("transfer", "ready", 0L), ("review", "review", 777L), ("committing", "committing", 777L)], phases);
        Assert.Equal("installed", link.LastState);
        Assert.Equal(bytes, link.Bytes.ToArray());
        Assert.Equal(4, link.Types.FindAll(t => t == "pack.data").Count);
        Assert.False(link.IsOpen);
    }

    [Theory]
    [InlineData("denied")]
    [InlineData("expired")]
    [InlineData("failed")]
    [InlineData("cancelled")]
    public async Task RejectsNonCommitTerminal(string terminal)
    {
        var link = new Link { FinalStates = ["review", terminal] };
        var phases = new List<string>();
        await Assert.ThrowsAsync<IOException>(() => PackInstaller.InstallAsync(link, [42], CancellationToken.None, phases.Add));
        Assert.Equal(["transfer", "review"], phases);
        Assert.False(link.IsOpen);
    }

    [Fact]
    public async Task RejectsMismatchedCorrelationAndOversize()
    {
        var link = new Link { FaultAt = "hello", Fault = "reply" };
        await Assert.ThrowsAsync<InvalidDataException>(() => PackInstaller.InstallAsync(link, [42], CancellationToken.None));
        Assert.DoesNotContain("pack.offer", link.Types);
        await Assert.ThrowsAsync<InvalidDataException>(() =>
            PackInstaller.InstallAsync(link, new byte[PackInstaller.MaxArchive+1], CancellationToken.None));
    }

    [Fact]
    public async Task FastCommitReportsOnlyObservedPhases()
    {
        var link = new Link { FinalStates = ["installed"] };
        var phases = new List<string>();
        await PackInstaller.InstallAsync(link, [42], CancellationToken.None, phases.Add);
        Assert.Equal(["transfer"], phases);
    }

    [Theory]
    [InlineData("pending", "offset", "")]
    [InlineData("ready", "offset", "")]
    [InlineData("review", "offset", "transfer")]
    [InlineData("committing", "offset", "transfer,review")]
    [InlineData("installed", "offset", "transfer,review,committing")]
    [InlineData("ready", "session", "")]
    [InlineData("review", "seq", "transfer")]
    [InlineData("committing", "reply", "transfer,review")]
    [InlineData("review", "type", "transfer")]
    public async Task MalformedRepliesCannotAnnounceProgress(string state, string fault, string expected)
    {
        var link = new Link { FaultAt = state, Fault = fault };
        var phases = new List<string>();
        var error = await Record.ExceptionAsync(() => PackInstaller.InstallAsync(link, [42], CancellationToken.None, phases.Add));
        Assert.True(error is IOException or InvalidDataException);
        Assert.Equal(expected, string.Join(",", phases));
        Assert.False(link.IsOpen);
    }

    [Theory]
    [InlineData("review")]
    [InlineData("pending")]
    public async Task RejectsBackwardProgress(string backwards)
    {
        var link = new Link { FinalStates = ["committing", backwards] };
        var phases = new List<string>();
        await Assert.ThrowsAsync<InvalidDataException>(() =>
            PackInstaller.InstallAsync(link, [42], CancellationToken.None, phases.Add));
        Assert.Equal(["transfer", "committing"], phases);
    }
}
