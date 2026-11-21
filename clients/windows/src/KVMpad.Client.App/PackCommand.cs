// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
using System;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using KVMpad.Client.App.Interop;
using KVMpad.Client.Core.Discovery;
using KVMpad.Client.Core.Display;
using KVMpad.Client.Core.Input;
using KVMpad.Client.Core.Session;
using KVMpad.Client.Core.Storage;

namespace KVMpad.Client.App;

internal static class PackCommand
{
    internal static async Task<int> RunAsync(string[] args)
    {
        try
        {
            if (args is ["--usb-match-signal", var serial, "640x480" or "1024x600"] &&
                serial.Length == 12 && serial.All(Uri.IsHexDigit))
            {
                var expected = args[2] == "1024x600" ? new DisplayResolution(1024, 600) : new DisplayResolution(640, 480);
                var changed = new UsbMonitorDisplayService().MatchSignal(serial, expected);
                using var output = new StreamWriter(Console.OpenStandardOutput());
                await output.WriteLineAsync($"Desktop {changed.Current}, USB signal {changed.Signal}");
                return 0;
            }
            var store = new CredentialManagerStore();
            if (args is ["--host", var deviceId] && deviceId.Length is > 0 and <= 128)
            {
                var paired = SelectPaired(store, deviceId);
                paired = await MdnsBrowser.ResolvePairedAsync(paired, CancellationToken.None).ConfigureAwait(false);
                await RunConnectedAsync(paired, (session, transport, output, token) =>
                    HostCompanion.RunAsync(session, transport, () => output.WriteLine("HOST attached"), token)).ConfigureAwait(false);
                return 0;
            }
            if (args is ["--usb-resolution", var selected, "640x480" or "1024x600"] && selected.Length is > 0 and <= 128)
            {
                var paired = await MdnsBrowser.ResolvePairedAsync(SelectPaired(store, selected), CancellationToken.None);
                var size = args[2] == "1024x600" ? new DisplayResolution(1024,600) : new DisplayResolution(640,480);
                await RunConnectedAsync(paired, async (session, transport, output, token) => {
                    var service = new UsbMonitorDisplayService();
                    var active = service.Read();
                    if (session.Logic.DisplaySerial != active.Serial)
                        throw new InvalidOperationException("The active USB display must match the paired KVMpad and support profile selection.");
                    var changed = await service.ApplyProfileAsync(active, size, session.Logic.RequestDisplayProfileAsync, token);
                    await output.WriteLineAsync($"Desktop {changed.Current}, USB signal {changed.Signal}");
                }).ConfigureAwait(false);
                return 0;
            }
            if (args is ["--pack-devices"])
            {
                var list = DeviceListEntry.Merge([], store.List()).Select(row => new { id = row.DeviceId, name = row.Paired!.DeviceName });
                using var output = Console.OpenStandardOutput();
                await JsonSerializer.SerializeAsync(output, list).ConfigureAwait(false);
                return 0;
            }
            if (args is not ["--pack-install", var id] || string.IsNullOrWhiteSpace(id) || id.Length > 128)
                throw new InvalidDataException("Invalid paired-device selection.");
            var device = SelectPaired(store, id);
            using var input = Console.OpenStandardInput();
            byte[] buffer = new byte[PackInstaller.MaxArchive + 1];
            int count = 0;
            while (count < buffer.Length)
            {
                int n = await input.ReadAsync(buffer.AsMemory(count)).ConfigureAwait(false);
                if (n == 0) break;
                count += n;
            }
            if (count is < 1 or > PackInstaller.MaxArchive) throw new InvalidDataException("Invalid archive size.");
            device = await MdnsBrowser.ResolvePairedAsync(device, CancellationToken.None).ConfigureAwait(false);
            await using var link = new ClientWebSocketTransport(device.ToEndpoint());
            using var progress = new StreamWriter(Console.OpenStandardOutput()) { AutoFlush = true };
            try
            {
                await PackInstaller.InstallAsync(link, buffer[..count], CancellationToken.None, progress.WriteLine)
                    .ConfigureAwait(false);
            }
            catch (Exception) when (HostDiagnostics.HandshakeFailure(link) is { } failure) { throw failure; }
            return 0;
        }
        catch (Exception e)
        {
            // Only local, fixed transfer diagnostics are printable; transport exceptions may contain endpoints.
            string reason = args is ["--usb-match-signal" or "--usb-resolution", _, _] && e is InvalidOperationException ? e.Message
                : args is ["--host", _] or ["--usb-resolution", _, _] ? HostDiagnostics.Describe(e)
                : e is HostFailure || e.GetType() == typeof(IOException) || e.GetType() == typeof(InvalidDataException)
                ? e.Message : HostDiagnostics.Describe(e, "The authenticated device transfer could not finish.");
            using var err = new StreamWriter(Console.OpenStandardError());
            await err.WriteLineAsync(reason).ConfigureAwait(false);
            return 1;
        }
    }

    private static PairedDevice SelectPaired(ICredentialStore store, string deviceId)
    {
        string key = DeviceListEntry.InstanceKey(deviceId);
        return DeviceListEntry.Merge([], store.List())
            .FirstOrDefault(row => string.Equals(DeviceListEntry.InstanceKey(row.DeviceId), key,
                StringComparison.OrdinalIgnoreCase))?.Paired
            ?? throw new HostFailure(HostFailureReason.Unpaired);
    }

    private static async Task RunConnectedAsync(PairedDevice paired,
        Func<RemoteSession,ClientWebSocketTransport,StreamWriter,CancellationToken,Task> action)
    {
        var transport = new ClientWebSocketTransport(paired.ToEndpoint());
        var logic = new SessionLogic(SystemClock.Instance, new SessionOptions { ClientName = Environment.MachineName }, new CaptureLedger());
        await using var session = new RemoteSession(transport, logic);
        using var lifetime = new CancellationTokenSource();
        var connected = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        logic.StateChanged += state => {
            if (state == ClientSessionState.Connected) connected.TrySetResult();
        };
        Task run = session.RunAsync(lifetime.Token);
        try
        {
            Task first = await Task.WhenAny(run, connected.Task).WaitAsync(TimeSpan.FromSeconds(12)).ConfigureAwait(false);
            await first.ConfigureAwait(false);
            if (first == run) throw new HostFailure(logic.LastErrorCode == null ? HostFailureReason.SessionClosed
                : HostFailureReason.Rejected, logic.LastErrorCode);
            using var output = new StreamWriter(Console.OpenStandardOutput()) { AutoFlush = true };
            await action(session, transport, output, lifetime.Token).ConfigureAwait(false);
        }
        finally
        {
            lifetime.Cancel();
            // Abort an incomplete TCP/TLS connection as well as a connected WebSocket.
            try { await run.ConfigureAwait(false); }
            catch (OperationCanceledException) when (lifetime.IsCancellationRequested) { }
            catch (Exception) when (HostDiagnostics.HandshakeFailure(transport) is { } failure) { throw failure; }
        }
    }
}
