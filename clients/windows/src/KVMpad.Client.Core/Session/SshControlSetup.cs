// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
using System;
using System.Buffers.Binary;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace KVMpad.Client.Core.Session;

/// <summary>Prepare a pinned OpenSSH subsystem command on the currently authenticated CLIENT session.</summary>
public sealed class SshControlSetup(SessionLogic session, string host)
{
    public bool Available => session.HasAuthenticatedSession && session.SshHostKeySha256 != null;

    public Task<string> PrepareAsync(string publicKeyFile, CancellationToken token)
    {
        string openssh = Path.Combine(Environment.SystemDirectory, "OpenSSH");
        string ssh = Path.Combine(openssh, "ssh.exe");
        string scanner = Path.Combine(openssh, "ssh-keyscan.exe");
        if (!File.Exists(ssh))
            throw new InvalidOperationException("The Windows OpenSSH Client is required for this command.");
        return PrepareAsync(publicKeyFile, Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ssh,
            (address, cancel) => File.Exists(scanner) ? ScanAsync(scanner, address, 22, cancel) :
                throw new InvalidOperationException("Windows ssh-keyscan is required to read the SSH host key."), token);
    }

    internal async Task<string> PrepareAsync(string publicKeyFile, string home, string ssh,
        Func<string, CancellationToken, Task<string>> scan, CancellationToken token)
    {
        string? sessionId = session.SessionId, pin = session.SshHostKeySha256;
        if (!Available || sessionId == null || pin == null)
            throw new InvalidOperationException("Connect to a paired KVMpad that offers SSH first.");
        // ssh-keyscan accepts extra syntax in a host argument (including option and host lists).
        if (host.Length == 0 || host.Length > 253 || host[0] == '-' ||
            !(IPAddress.TryParse(host, out _) || Uri.CheckHostName(host) == UriHostNameType.Dns) ||
            host.Any(c => !char.IsAsciiLetterOrDigit(c) && c is not ('.' or ':' or '%' or '-')))
            throw new InvalidOperationException("The connected device address is unsuitable for OpenSSH.");
        if (!publicKeyFile.EndsWith(".pub", StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Choose an OpenSSH public key file ending in .pub.");
        publicKeyFile = Path.GetFullPath(publicKeyFile);
        string publicLine;
        using (var file = File.OpenRead(publicKeyFile))
        {
            if (file.Length is < 1 or > 16384) throw InvalidPublicKey();
            byte[] bytes = new byte[(int)file.Length];
            await file.ReadExactlyAsync(bytes, token).ConfigureAwait(false);
            publicLine = Encoding.UTF8.GetString(bytes).Trim();
        }
        var fields = publicLine.Split([' ', '\t'], 3, StringSplitOptions.RemoveEmptyEntries);
        if (publicLine.Contains('\n') || publicLine.Contains('\r') || fields.Length < 2 ||
            !TryPublicBlob(fields[0], fields[1], out byte[] operatorBlob)) throw InvalidPublicKey();

        // Binding opens the board's SSH listener. A key scan only performs key exchange.
        CheckSession();
        await session.BindSshKeyAsync(SHA256.HashData(operatorBlob), token).ConfigureAwait(false);
        CheckSession();
        string alias = "kvmpad-" + pin.ToLowerInvariant();
        string relative = ".ssh/kvmpad/" + alias + ".hosts";
        string path = Path.Combine(home, relative);
        string? boardKey = await ReadCachedKeyAsync(path, alias, pin, token).ConfigureAwait(false);
        CheckSession();
        if (boardKey == null)
        {
            string scanned;
            try { scanned = await scan(host, token).ConfigureAwait(false); }
            catch (Exception ex) when (ex is IOException or TimeoutException)
            {
                CheckSession();
                scanned = await scan(host, token).ConfigureAwait(false);
            }
            CheckSession();
            boardKey = FindPinnedKey(scanned, pin, alias: null)
                ?? throw new InvalidDataException("The SSH host key did not match the fingerprint from the paired connection.");
        }

        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        string temporary = path + "." + Guid.NewGuid().ToString("N");
        try
        {
            await File.WriteAllTextAsync(temporary, $"{alias} ssh-ed25519 {boardKey}\n", Encoding.ASCII, token)
                .ConfigureAwait(false);
            CheckSession();
            File.Move(temporary, path, overwrite: true);
        }
        finally { File.Delete(temporary); }
        CheckSession();

        // An agent can use the public file to select a loaded key. For a standard key pair,
        // OpenSSH opens its sibling private file itself and handles any passphrase prompt.
        string identity = File.Exists(publicKeyFile[..^4]) ? publicKeyFile[..^4] : publicKeyFile;
        return $"& {PowerShellLiteral(ssh)} -F none -T -o ServerAliveInterval=1 -o ServerAliveCountMax=45 -o StrictHostKeyChecking=yes " +
               $"-o UserKnownHostsFile=~/{relative} -o GlobalKnownHostsFile=none -o UpdateHostKeys=no " +
               $"-o HostKeyAlias={alias} -o HostKeyAlgorithms=ssh-ed25519 -o PreferredAuthentications=publickey " +
               $"-o IdentitiesOnly=yes -i {PowerShellLiteral(identity)} -p 22 -s {PowerShellLiteral("kvmpad@" + host)} kvmpad";

        void CheckSession()
        {
            token.ThrowIfCancellationRequested();
            if (!Available || session.SessionId != sessionId || session.SshHostKeySha256 != pin)
                throw new InvalidOperationException("The paired connection changed. Reconnect and prepare a new SSH command.");
        }
    }

    private static InvalidDataException InvalidPublicKey()
        => new("Choose a single OpenSSH Ed25519, ECDSA or RSA public key in .pub format.");

    private static async Task<string?> ReadCachedKeyAsync(string path, string alias, string pin, CancellationToken token)
    {
        try
        {
            using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete);
            long length = file.Length;
            if (length is < 1 or > 16384) return null;
            byte[] bytes = new byte[(int)length];
            await file.ReadExactlyAsync(bytes, token).ConfigureAwait(false);
            return FindPinnedKey(Encoding.ASCII.GetString(bytes), pin, alias);
        }
        catch (FileNotFoundException) { return null; }
        catch (DirectoryNotFoundException) { return null; }
    }

    private static string? FindPinnedKey(string records, string pin, string? alias)
    {
        foreach (string line in records.Split('\n'))
        {
            var parts = line.TrimEnd('\r').Split([' ', '\t'], StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length == 3 && parts[0][0] is not ('#' or '@') && (alias == null || parts[0] == alias) &&
                parts[1] == "ssh-ed25519" && TryPublicBlob(parts[1], parts[2], out var blob) &&
                Convert.ToHexString(SHA256.HashData(blob)).Equals(pin, StringComparison.OrdinalIgnoreCase))
                return Convert.ToBase64String(blob);
        }
        return null;
    }

    private static bool TryPublicBlob(string type, string encoded, out byte[] blob)
    {
        blob = [];
        if (encoded.Length is 0 or > 16384 ||
            type is not ("ssh-ed25519" or "ssh-rsa" or "ecdsa-sha2-nistp256" or "ecdsa-sha2-nistp384" or "ecdsa-sha2-nistp521"))
            return false;
        try { blob = Convert.FromBase64String(encoded); }
        catch (FormatException) { return false; }
        ReadOnlySpan<byte> rest = blob;
        if (!Field(ref rest).SequenceEqual(Encoding.ASCII.GetBytes(type))) return false;
        var first = Field(ref rest);
        if (type == "ssh-ed25519") return first.Length == 32 && rest.IsEmpty;
        var second = Field(ref rest);
        if (!rest.IsEmpty || first.IsEmpty || second.IsEmpty) return false;
        if (type == "ssh-rsa") return true;
        int pointSize = type.EndsWith("256") ? 65 : type.EndsWith("384") ? 97 : 133;
        return first.SequenceEqual(Encoding.ASCII.GetBytes(type[11..])) && second.Length == pointSize && second[0] == 4;
    }

    private static ReadOnlySpan<byte> Field(ref ReadOnlySpan<byte> rest)
    {
        if (rest.Length < 4 || BinaryPrimitives.ReadUInt32BigEndian(rest) > rest.Length - 4) return [];
        int count = BinaryPrimitives.ReadInt32BigEndian(rest);
        var field = rest.Slice(4, count);
        rest = rest[(4 + count)..];
        return field;
    }

    private static string PowerShellLiteral(string value)
    {
        var result = new StringBuilder("'");
        foreach (char c in value)
        {
            if (c is '\'' or '\u2018' or '\u2019' or '\u201a' or '\u201b') result.Append(c);
            result.Append(c);
        }
        return result.Append('\'').ToString();
    }

    internal static async Task<string> ScanAsync(string scanner, string address, int port, CancellationToken token)
    {
        var start = new ProcessStartInfo(scanner) { UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (string arg in new[] { "-T", "5", "-t", "ed25519", "-p", port.ToString(), address })
            start.ArgumentList.Add(arg);
        using var process = Process.Start(start) ?? throw new InvalidOperationException("Windows SSH key scan could not start.");
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(TimeSpan.FromSeconds(8));
        try
        {
            Task<string> output = process.StandardOutput.ReadToEndAsync(timeout.Token);
            Task<string> errors = process.StandardError.ReadToEndAsync(timeout.Token);
            await Task.WhenAll(output, errors, process.WaitForExitAsync(timeout.Token)).ConfigureAwait(false);
            if (string.IsNullOrWhiteSpace(output.Result))
                throw new IOException("Could not read the KVMpad SSH host key on TCP port 22. Keep the paired connection open and try again.");
            if (process.ExitCode != 0)
                throw new InvalidDataException("Windows SSH key scan returned incomplete host key data.");
            return output.Result;
        }
        catch (OperationCanceledException) when (!token.IsCancellationRequested)
        { throw new TimeoutException("The KVMpad SSH host key scan timed out."); }
        finally { if (!process.HasExited) process.Kill(); }
    }
}
