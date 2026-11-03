// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.Linq;
using System.Net.WebSockets;
using System.Threading;
using System.Threading.Tasks;
using KVMpad.Client.Core.Discovery;
using KVMpad.Client.Core.Session;
using Xunit;

namespace KVMpad.Client.Core.Tests;

/// <summary>
/// Skipped unless <c>KVMPAD_LIVE_HOST</c> names a DEVICE on the LAN (and <c>KVMPAD_LIVE_PIN</c> its certificate
/// SHA-256 from the console line <c>remote status</c>). These run the shipping CLIENT code against real
/// firmware, never past the admission: no pairing, no token, nothing that could reach the HOST.
/// </summary>
public sealed class LiveDeviceFactAttribute : FactAttribute
{
    public LiveDeviceFactAttribute()
    {
        if (string.IsNullOrEmpty(Environment.GetEnvironmentVariable(LiveDeviceTests.HostVar))
            || string.IsNullOrEmpty(Environment.GetEnvironmentVariable(LiveDeviceTests.PinVar)))
            Skip = $"Live DEVICE test: set {LiveDeviceTests.HostVar} and {LiveDeviceTests.PinVar}.";
    }
}

public sealed class LiveDeviceTests
{
    public const string HostVar = "KVMPAD_LIVE_HOST";
    public const string PinVar = "KVMPAD_LIVE_PIN";
    private const int DevicePort = 8443;

    // Never a real token: a fixed string of the right shape, so the refusal is the admission's, not the parser's.
    private static readonly string BogusToken = new('A', 43);

    private static string Host => Environment.GetEnvironmentVariable(HostVar)!;
    private static string Pin => Environment.GetEnvironmentVariable(PinVar)!.ToLowerInvariant();

    [LiveDeviceFact]
    public async Task Browse_finds_the_device_with_its_port_and_a_closed_window()
    {
        var browser = new MdnsBrowser();
        IReadOnlyList<DiscoveredDevice> found = await browser.BrowseAsync(TimeSpan.FromSeconds(4), CancellationToken.None);

        DiscoveredDevice? device = found.FirstOrDefault(d => d.Addresses.Any(a => a.ToString() == Host));
        Assert.NotNull(device);
        Assert.Equal(DevicePort, device!.Port);
        Assert.True(device.IsCompatible);
        Assert.False(device.PairingOpen); // SEC-103: pair=1 only while a window is open
    }

    [LiveDeviceFact]
    public async Task Pinned_tls_reaches_the_admission_which_refuses_while_remote_is_off()
    {
        await using var transport = new ClientWebSocketTransport(new SessionEndpoint(Host, DevicePort, Pin, BogusToken));
        WebSocketException ex = await Assert.ThrowsAsync<WebSocketException>(() => transport.ConnectAsync(CancellationToken.None));

        Assert.Null(transport.LastFailureReason); // the pin was accepted: the refusal came after TLS
        Assert.Contains("403", ex.Message);       // FORBIDDEN: Settings -> Remote is off (ADR-032 §2)
        Assert.False(transport.IsOpen);
    }

    [LiveDeviceFact]
    public async Task A_wrong_pin_stops_the_connection_before_any_request()
    {
        string wrong = new string(Pin.Reverse().ToArray());
        await using var transport = new ClientWebSocketTransport(new SessionEndpoint(Host, DevicePort, wrong, BogusToken));
        await Assert.ThrowsAnyAsync<Exception>(() => transport.ConnectAsync(CancellationToken.None));

        Assert.NotNull(transport.LastFailureReason); // rejected by the pinner, not by the device
        Assert.False(transport.IsOpen);
    }
}
