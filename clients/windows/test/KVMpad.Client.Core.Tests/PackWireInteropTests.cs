// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using KVMpad.Client.Core.Protocol;
using KVMpad.Client.Core.Session;
using Xunit;

namespace KVMpad.Client.Core.Tests;

public sealed class PackPeerFactAttribute : FactAttribute
{
    public PackPeerFactAttribute()
    {
        if (!File.Exists(Environment.GetEnvironmentVariable("KVMPAD_TEST_PACK_PEER")) ||
            !File.Exists(Environment.GetEnvironmentVariable("KVMPAD_TEST_PACK_ARCHIVE")))
            Skip = "Set KVMPAD_TEST_PACK_PEER to test_pack_receive and KVMPAD_TEST_PACK_ARCHIVE to a test pack.";
    }
}

public sealed class PackWireInteropTests
{
    private sealed class NativePeer : IMessageTransport
    {
        private readonly Process _peer;
        public bool IsOpen => !_peer.HasExited;
        public string? NegotiatedSubprotocol => ProtocolLimits.SessionSubprotocol;
        public NativePeer(string path, string archive, byte[] bytes)
        {
            var options = new ProcessStartInfo(path) {
                UseShellExecute = false, CreateNoWindow = true, RedirectStandardInput = true,
                RedirectStandardOutput = true, StandardInputEncoding = new UTF8Encoding(false)
            };
            options.ArgumentList.Add("--peer");
            options.ArgumentList.Add(archive);
            options.ArgumentList.Add(Convert.ToHexString(SHA256.HashData(bytes)));
            _peer = Process.Start(options)!;
        }

        public Task ConnectAsync(CancellationToken ct) => Task.CompletedTask;
        public async Task SendAsync(ReadOnlyMemory<byte> frame, CancellationToken ct)
        {
            await _peer.StandardInput.WriteLineAsync(Encoding.UTF8.GetString(frame.Span).AsMemory(),ct);
            await _peer.StandardInput.FlushAsync(ct);
        }
        public async Task<byte[]?> ReceiveAsync(CancellationToken ct)
        {
            string? reply = await _peer.StandardOutput.ReadLineAsync(ct);
            return reply == null ? null : Encoding.UTF8.GetBytes(reply);
        }
        public async Task CloseAsync(CancellationToken ct)
        {
            _peer.StandardInput.Close();
            await _peer.WaitForExitAsync(ct);
            Assert.Equal(0,_peer.ExitCode);
        }
        public ValueTask DisposeAsync()
        {
            if (!_peer.HasExited) _peer.Kill();
            _peer.Dispose();
            return ValueTask.CompletedTask;
        }
    }

    [PackPeerFact]
    public async Task WindowsUploaderAndNativeReceiverAgreeOnEveryByte()
    {
        string path = Environment.GetEnvironmentVariable("KVMPAD_TEST_PACK_ARCHIVE")!;
        byte[] bytes = await File.ReadAllBytesAsync(path);
        await using var link = new NativePeer(Environment.GetEnvironmentVariable("KVMPAD_TEST_PACK_PEER")!,path,bytes);
        var phases = new List<string>();
        await PackInstaller.InstallAsync(link,bytes,CancellationToken.None,phases.Add);
        // The native fixture physically approves and finishes between two polls.
        Assert.Equal(["transfer", "review"], phases);
        Assert.False(link.IsOpen);
    }
}
