// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using KVMpad.Client.Core.Session;

namespace KVMpad.Client.App;

internal static class HostCompanion
{
    private static ProcessStartInfo LaunchOptions()
    {
        string script = Path.Combine(AppContext.BaseDirectory, "host", "host.py");
        if (!File.Exists(script)) throw new HostFailure(HostFailureReason.MissingComponent);
        string? workspace = null;
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir != null; dir = dir.Parent)
        {
            if (File.Exists(Path.Combine(dir.FullName, "hosts", "windows", "host.py"))) {
                workspace = dir.Parent?.FullName;
                break;
            }
            if (File.Exists(Path.Combine(dir.FullName, "esp32-s3-kvmpad", "hosts", "windows", "host.py"))) {
                workspace = dir.FullName;
                break;
            }
        }
        string? python = Environment.GetEnvironmentVariable("KVMPAD_HOST_PYTHON");
        string? bundled = workspace == null ? null : Path.Combine(workspace, ".tools", "venv", "Scripts", "python.exe");
        string installed = Path.Combine(AppContext.BaseDirectory, "runtime", "python.exe");
        if (File.Exists(installed)) bundled = installed;
        if (string.IsNullOrWhiteSpace(python)) python = File.Exists(bundled) ? bundled : "py.exe";
        var options = new ProcessStartInfo(python!) {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardInput = true, RedirectStandardOutput = true,
            StandardInputEncoding = new UTF8Encoding(false), StandardOutputEncoding = Encoding.UTF8
        };
        if (Path.GetFileName(python).Equals("py.exe", StringComparison.OrdinalIgnoreCase))
            options.ArgumentList.Add("-3");
        options.ArgumentList.Add("-B");
        options.ArgumentList.Add(script);
        options.ArgumentList.Add("--paired-client");
        string? picker = Environment.GetEnvironmentVariable("KVMPAD_HOST_PICKER");
        if (string.IsNullOrWhiteSpace(picker)) {
            picker = Path.Combine(AppContext.BaseDirectory, "host", "kvmpad-capture.exe");
            if (!File.Exists(picker) && workspace != null)
                picker = Path.Combine(workspace, ".tools", "tmp", "host-capture", "kvmpad-capture.exe");
        }
        if (File.Exists(picker)) {
            options.ArgumentList.Add("--picker");
            options.ArgumentList.Add(picker);
        }
        string? msfs = Environment.GetEnvironmentVariable("KVMPAD_HOST_MSFS") ??
            Path.Combine(Path.GetDirectoryName(picker) ?? AppContext.BaseDirectory, "kvmpad-msfs.exe");
        if (File.Exists(msfs)) {
            options.ArgumentList.Add("--msfs");
            options.ArgumentList.Add(msfs);
        }
        // OBS remains disabled until the operator enters its password in the HOST window.
        options.ArgumentList.Add("--obs");
        foreach (var (name, option) in new[] { ("KVMPAD_HOST_OBS_SCENE", "--obs-scene"), ("KVMPAD_HOST_OBS_INPUT", "--obs-input") }) {
            if (Environment.GetEnvironmentVariable(name) is { Length: > 0 } value) {
                options.ArgumentList.Add(option);
                options.ArgumentList.Add(value);
            }
        }
        return options;
    }

    public static Task RunAsync(RemoteSession session, ClientWebSocketTransport transport,
                                Action ready, CancellationToken cancellationToken)
    {
        byte[]? board = transport.BoardCertificate;
        if (board == null || !transport.IsOpen)
            throw new HostFailure(HostFailureReason.NoSession);
        return RunAuthenticatedAsync(session, board, transport.BoardHost, ready, cancellationToken);
    }

    private static async Task RunAuthenticatedAsync(RemoteSession session, byte[] board, string boardHost,
                                                    Action ready, CancellationToken cancellationToken)
    {
        using var lifetime = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        void Ended(SessionEndReason _) => lifetime.Cancel();
        session.Ended += Ended;
        try
        {
            if (!session.Logic.HasAuthenticatedSession)
                throw new HostFailure(HostFailureReason.NoSession);
            IPAddress? address;
            if (!IPAddress.TryParse(boardHost, out address))
                address = (await Dns.GetHostAddressesAsync(boardHost, AddressFamily.InterNetwork, lifetime.Token)
                    .ConfigureAwait(false)).FirstOrDefault();
            if (address?.AddressFamily != AddressFamily.InterNetwork)
                throw new HostFailure(HostFailureReason.IPv4Required);
            using var process = StartProcess();
            bool offered = false;
            bool attached = false;
            try
            {
                using var startup = CancellationTokenSource.CreateLinkedTokenSource(lifetime.Token);
                startup.CancelAfter(TimeSpan.FromSeconds(20));
                var request = JsonSerializer.Serialize(new { board_der = Convert.ToBase64String(board), board_address = address.ToString() });
                await process.StandardInput.WriteLineAsync(request.AsMemory(), startup.Token).ConfigureAwait(false);
                await process.StandardInput.FlushAsync(startup.Token).ConfigureAwait(false);
                var (port, leaf) = ParseListener(await process.StandardOutput.ReadLineAsync(startup.Token).ConfigureAwait(false));
                offered = true;
                try { await session.Logic.AttachHostAsync(port, leaf, startup.Token).ConfigureAwait(false); }
                catch (IOException) { offered = false; throw; }
                catch (InvalidOperationException)
                {
                    offered = false;
                    throw new HostFailure(session.Logic.State is ClientSessionState.Disconnected or ClientSessionState.Handshaking
                        ? HostFailureReason.NoSession : HostFailureReason.Pending);
                }
                attached = true;
                ready();
                await process.WaitForExitAsync(lifetime.Token).ConfigureAwait(false);
                if (process.ExitCode != 0) throw new HostFailure(HostFailureReason.ProcessClosed);
            }
            catch (OperationCanceledException) when (!lifetime.IsCancellationRequested)
            { throw new HostFailure(offered ? HostFailureReason.RequestTimeout : HostFailureReason.ListenerTimeout); }
            catch (TimeoutException) { throw new HostFailure(HostFailureReason.RequestTimeout); }
            catch (IOException e) when (!offered && e is not HostFailure)
            { throw new HostFailure(HostFailureReason.Listener); }
            finally
            {
                // EOF ends the listener even when the CLIENT process dies without an orderly disconnect.
                try { process.StandardInput.Close(); }
                catch (IOException) { /* The companion already closed its input. */ }
                if (!process.HasExited) {
                    using var grace = new CancellationTokenSource(TimeSpan.FromSeconds(2));
                    try { await process.WaitForExitAsync(grace.Token).ConfigureAwait(false); }
                    catch (OperationCanceledException) {
                        if (!process.HasExited) process.Kill(entireProcessTree: true);
                        await process.WaitForExitAsync().ConfigureAwait(false);
                    }
                }
                if (offered && session.Logic.State != ClientSessionState.Disconnected)
                {
                    using var ending = new CancellationTokenSource(TimeSpan.FromSeconds(3));
                    try { await session.Logic.DetachHostAsync(ending.Token).ConfigureAwait(false); }
                    catch (Exception) when (!attached || lifetime.IsCancellationRequested)
                    { /* Preserve the original startup failure or explicit stop. */ }
                }
            }
        }
        catch (OperationCanceledException) when (lifetime.IsCancellationRequested && !cancellationToken.IsCancellationRequested)
        { throw new HostFailure(HostFailureReason.SessionClosed); }
        finally { session.Ended -= Ended; }
    }

    private static Process StartProcess()
    {
        try { return Process.Start(LaunchOptions()) ?? throw new HostFailure(HostFailureReason.StartProcess); }
        catch (Exception e) when (e is Win32Exception or InvalidOperationException)
        { throw new HostFailure(HostFailureReason.StartProcess); }
    }

    private static (int Port, byte[] Leaf) ParseListener(string? line)
    {
        if (line == null) throw new HostFailure(HostFailureReason.Listener);
        if (line.Length > 2048) throw new HostFailure(HostFailureReason.InvalidListener);
        try
        {
            using var reply = JsonDocument.Parse(line);
            var root = reply.RootElement;
            if (!root.TryGetProperty("port", out var portValue) || !portValue.TryGetInt32(out int port)
                || port is < 1 or > 65535 || !root.TryGetProperty("leaf", out var leafValue)
                || !leafValue.TryGetBytesFromBase64(out var leaf) || leaf.Length is < 1 or > 1024)
                throw new HostFailure(HostFailureReason.InvalidListener);
            using var cert = X509CertificateLoader.LoadCertificate(leaf);
            if (!cert.RawData.AsSpan().SequenceEqual(leaf)) throw new HostFailure(HostFailureReason.InvalidListener);
            return (port, leaf);
        }
        catch (Exception e) when (e is JsonException or InvalidOperationException or CryptographicException)
        { throw new HostFailure(HostFailureReason.InvalidListener); }
    }
}
