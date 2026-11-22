// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using KVMpad.Client.Core.Input;
using KVMpad.Client.Core.Protocol;
using KVMpad.Client.Core.Session;
using Xunit;

namespace KVMpad.Client.Core.Tests;

public sealed class SshControlSetupTests : IDisposable
{
    private readonly DirectoryInfo _folder = Directory.CreateTempSubdirectory("kvmpad-ssh-");
    private readonly SessionLogic _logic = new(new FakeClock(), new SessionOptions(), new CaptureLedger());
    private readonly byte[] _board = Key(0x21), _operator = Key(0x76);
    private string Public => Path.Combine(_folder.FullName, "operator's key.pub");
    private string FilePin => Convert.ToHexString(SHA256.HashData(_board)).ToLowerInvariant();
    private string VerifiedFile => Path.Combine(_folder.FullName, ".ssh", "kvmpad", $"kvmpad-{FilePin}.hosts");
    private string ScanLine => "source ssh-ed25519 " + Convert.ToBase64String(_board);
    private string CanonicalRecord => $"kvmpad-{FilePin} ssh-ed25519 {Convert.ToBase64String(_board)}\n";

    public SshControlSetupTests()
    {
        File.WriteAllText(Public, "ssh-ed25519 " + Convert.ToBase64String(_operator) + " user comment\r\n");
        OpenPairedSession(FilePin);
    }

    private void OpenPairedSession(string pin)
    {
        _logic.OnTransportOpened();
        _logic.OnMessage(ProtocolCodec.Decode(JsonSerializer.SerializeToUtf8Bytes(new {
            v=1, type="hello.ack", seq=1, session_id="paired",
            payload=new {capabilities=new[]{"ssh_key_binding"}, ssh_host_key_sha256=pin}
        })));
    }

    private static byte[] Key(byte fill)
        => [0, 0, 0, 11, .."ssh-ed25519"u8, 0, 0, 0, 32, ..Enumerable.Repeat(fill, 32)];

    private async Task Acknowledge()
    {
        var limit = Stopwatch.StartNew();
        while (limit.Elapsed < TimeSpan.FromSeconds(3))
        {
            var queued = _logic.OnTick();
            if (queued.Count == 0) { await Task.Delay(5); continue; }
            var request = ProtocolCodec.Decode(Assert.Single(queued));
            Assert.Equal("ssh.key.bind", request.Type);
            Assert.Equal(Convert.ToHexString(SHA256.HashData(_operator)), request.PayloadString("sha256"));
            _logic.OnMessage(ProtocolCodec.Decode(JsonSerializer.SerializeToUtf8Bytes(new {
                v=1,type="ssh.key.bound",seq=2,session_id="paired",payload=new {ack_seq=request.Seq}
            })));
            return;
        }
        throw new TimeoutException("No SSH binding request reached the paired session.");
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task MatchingDigestCannotAuthorizeAMalformedEd25519Blob(bool cached)
    {
        byte[] malformed = [.._board, 0];
        string pin = Convert.ToHexString(SHA256.HashData(malformed)).ToLowerInvariant();
        _logic.OnTransportClosed();
        OpenPairedSession(pin);
        string alias = "kvmpad-" + pin;
        string path = Path.Combine(Path.GetDirectoryName(VerifiedFile)!, alias + ".hosts");
        string record = $"{alias} ssh-ed25519 {Convert.ToBase64String(malformed)}\n";
        if (cached)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, record);
        }
        int calls = 0;
        var setup = new SshControlSetup(_logic, "kvmpad.local");
        var pending = setup.PrepareAsync(Public, _folder.FullName, "ssh.exe",
            (_, _) => { calls++; return Task.FromResult(record); }, default);
        await Acknowledge();
        await Assert.ThrowsAsync<InvalidDataException>(() => pending);
        Assert.Equal(1, calls);
        Assert.Equal(cached, File.Exists(path));
    }

    [Theory]
    [InlineData("10.4.3.2", true)]
    [InlineData("fe80::88%5", false)]
    [InlineData("kvmpad.local", false)]
    public async Task ActualConnectedEndpointAndOnlyVerifiedBoardKeyReachTheCommand(string host, bool sibling)
    {
        if (sibling) File.WriteAllText(Public[..^4], "never read");
        var setup = new SshControlSetup(_logic, host);
        bool scanned = false;
        Task<string> pending = setup.PrepareAsync(Public, _folder.FullName, "ssh.exe", (address, _) => {
            scanned = true;
            Assert.Equal(host, address);
            return Task.FromResult("# banner\nother ssh-ed25519 " + Convert.ToBase64String(_operator) + "\n" + ScanLine);
        }, default);
        Assert.False(scanned);
        await Acknowledge();
        string command = await pending;
        Assert.True(scanned);
        Assert.Empty(_logic.OnTick());
        Assert.False(_logic.CanSendInput);
        Assert.Equal(CanonicalRecord, File.ReadAllText(VerifiedFile));
        Assert.Contains($"HostKeyAlias=kvmpad-{FilePin}", command);
        Assert.Contains($"UserKnownHostsFile=~/.ssh/kvmpad/kvmpad-{FilePin}.hosts", command);
        Assert.Contains("GlobalKnownHostsFile=none", command);
        Assert.Contains("StrictHostKeyChecking=yes", command);
        Assert.Contains("ServerAliveInterval=1", command);
        Assert.Contains("ServerAliveCountMax=45", command);
        Assert.Contains(" -F none -T ", command);
        Assert.Contains(" -i '" + (sibling ? Public[..^4] : Public).Replace("'", "''") + "'", command);
        Assert.EndsWith($"-p 22 -s 'kvmpad@{host}' kvmpad", command);
    }

    [Fact]
    public async Task RevalidatedCacheStillBindsAndRewritesOnlyTheCanonicalRecord()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(VerifiedFile)!);
        string other = $"kvmpad-{FilePin} ssh-ed25519 {Convert.ToBase64String(_operator)}\n";
        File.WriteAllText(VerifiedFile, other + CanonicalRecord.Replace(" ", "\t").Replace("\n", "\r\n") + "* " + other);
        var setup = new SshControlSetup(_logic, "kvmpad.local");
        var pending = setup.PrepareAsync(Public, _folder.FullName, "ssh.exe",
            (_, _) => throw new Exception("Unexpected scan"), default);
        await Acknowledge();
        string command = await pending;
        Assert.Equal(CanonicalRecord, File.ReadAllText(VerifiedFile));
        Assert.Contains("StrictHostKeyChecking=yes", command);
        Assert.Contains($"HostKeyAlias=kvmpad-{FilePin}", command);
    }

    [Theory]
    [InlineData("other-alias", "ssh-ed25519", "board")]
    [InlineData("same", "ssh-ed25519", "operator")]
    [InlineData("same", "ssh-ed25519", "malformed")]
    [InlineData("same", "ssh-rsa", "board")]
    public async Task UntrustedCacheRequiresAFreshPinnedScan(string name, string type, string key)
    {
        string encoded = key switch {
            "board" => Convert.ToBase64String(_board),
            "operator" => Convert.ToBase64String(_operator),
            _ => Convert.ToBase64String([.._board, 0])
        };
        Directory.CreateDirectory(Path.GetDirectoryName(VerifiedFile)!);
        string alias = name == "same" ? "kvmpad-" + FilePin : name;
        File.WriteAllText(VerifiedFile, $"{alias} {type} {encoded}\n");
        var setup = new SshControlSetup(_logic, "kvmpad.local");
        int calls = 0;
        var pending = setup.PrepareAsync(Public, _folder.FullName, "ssh.exe",
            (_, _) => { calls++; return Task.FromResult(ScanLine); }, default);
        await Acknowledge();
        await pending;
        Assert.Equal(1, calls);
        Assert.Equal(CanonicalRecord, File.ReadAllText(VerifiedFile));
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(true, false)]
    [InlineData(false, true)]
    public async Task TransientScanFailureGetsOneRetry(bool timeout, bool failsAgain)
    {
        int calls = 0;
        var setup = new SshControlSetup(_logic, "kvmpad.local");
        var pending = setup.PrepareAsync(Public, _folder.FullName, "ssh.exe", (_, _) => {
            if (++calls == 1 || failsAgain)
            {
                if (timeout) throw new TimeoutException();
                throw new IOException();
            }
            return Task.FromResult(ScanLine);
        }, default);
        await Acknowledge();
        if (failsAgain)
        {
            await Assert.ThrowsAsync<IOException>(() => pending);
            Assert.False(File.Exists(VerifiedFile));
        }
        else
        {
            await pending;
            Assert.Equal(CanonicalRecord, File.ReadAllText(VerifiedFile));
        }
        Assert.Equal(2, calls);
    }

    [Theory]
    [InlineData("cancel")]
    [InlineData("cancel-after-read")]
    [InlineData("disconnect")]
    public async Task CancellationOrSessionChangeDuringFirstScanPreventsRetry(string interruption)
    {
        using var cancel = new CancellationTokenSource();
        var setup = new SshControlSetup(_logic, "kvmpad.local");
        int calls = 0;
        var pending = setup.PrepareAsync(Public, _folder.FullName, "ssh.exe", (_, token) => {
            calls++;
            if (interruption == "disconnect") _logic.OnTransportClosed();
            else cancel.Cancel();
            if (interruption == "cancel") token.ThrowIfCancellationRequested();
            throw new IOException("Scan interrupted");
        }, cancel.Token);
        await Acknowledge();
        if (interruption == "disconnect") await Assert.ThrowsAsync<InvalidOperationException>(() => pending);
        else await Assert.ThrowsAnyAsync<OperationCanceledException>(() => pending);
        Assert.Equal(1, calls);
        Assert.False(File.Exists(VerifiedFile));
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task MismatchOrLostPairedSessionLeavesNoHostFile(bool disconnect)
    {
        var setup = new SshControlSetup(_logic, "kvmpad.local");
        int calls = 0;
        Task<string> pending = setup.PrepareAsync(Public, _folder.FullName, "ssh.exe", (_, _) => {
            calls++;
            if (disconnect) _logic.OnTransportClosed();
            return Task.FromResult(disconnect ? ScanLine : "x ssh-ed25519 " + Convert.ToBase64String(_operator));
        }, default);
        await Acknowledge();
        if (disconnect) await Assert.ThrowsAsync<InvalidOperationException>(() => pending);
        else await Assert.ThrowsAsync<InvalidDataException>(() => pending);
        Assert.Equal(1, calls);
        Assert.False(File.Exists(VerifiedFile));
    }

    [Theory]
    [InlineData("-----BEGIN OPENSSH PRIVATE KEY-----")]
    [InlineData("ssh-rsa AAAAC3NzaC1lZDI1NTE5")]
    [InlineData("ssh-ed25519 !!!!")]
    public async Task InvalidOperatorKeyNeverBindsOrScans(string text)
    {
        File.WriteAllText(Public, text);
        var setup = new SshControlSetup(_logic, "kvmpad.local");
        await Assert.ThrowsAsync<InvalidDataException>(() => setup.PrepareAsync(Public, _folder.FullName, "ssh.exe",
            (_, _) => throw new Exception("Unexpected scan"), default));
        Assert.Empty(_logic.OnTick());
    }

    public void Dispose() => _folder.Delete(recursive: true);
}
