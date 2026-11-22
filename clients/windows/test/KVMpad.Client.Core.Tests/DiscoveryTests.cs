// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using KVMpad.Client.Core.Discovery;
using KVMpad.Client.Core.Protocol;
using KVMpad.Client.Core.Storage;
using Xunit;

namespace KVMpad.Client.Core.Tests;

/// <summary>
/// mDNS/DNS-SD for <c>_kvmpad._tcp.local</c> (ADR-022 FR-723). The datagrams are built inside the test: this
/// is unauthenticated data from a LAN, so the parser is exercised against both well-formed and hostile input
/// without touching a network.
/// </summary>
public sealed class DiscoveryTests
{
    [Fact]
    public void TheQueryAsksForThePointerRecordOfTheServiceType()
    {
        byte[] query = MdnsMessage.BuildPointerQuery(ProtocolLimits.ServiceType);

        // Header: id 0, flags 0, one question.
        Assert.Equal(0, query[0]);
        Assert.Equal(0, query[1]);
        Assert.Equal(0, query[2]);
        Assert.Equal(0, query[3]);
        Assert.Equal(1, query[5]);

        // Labels, then type PTR (12) and class IN (1).
        string encoded = Encoding.ASCII.GetString(query);
        Assert.Contains("_kvmpad", encoded);
        Assert.Contains("_tcp", encoded);
        Assert.Contains("local", encoded);
        Assert.Equal((byte)DnsRecordType.Ptr, query[^3]);
        Assert.Equal(1, query[^1]);
    }

    [Fact]
    public void AFullAnswerBecomesOneDeviceInTheList()
    {
        byte[] datagram = BuildAnswer("KVMpad-7F3A", 8443, "KVMpad-7F3A", version: "1", pairingOpen: true);

        MdnsResponse response = MdnsMessage.Parse(datagram);
        IReadOnlyList<DiscoveredDevice> devices = DeviceCollator.Collate([response]);

        DiscoveredDevice device = Assert.Single(devices);
        Assert.Equal("KVMpad-7F3A", device.DisplayName);
        Assert.Equal(8443, device.Port);
        Assert.Equal(1, device.ProtocolVersion);
        Assert.True(device.IsCompatible);
        Assert.True(device.PairingOpen);
        Assert.Equal("192.168.1.50", device.ConnectHost);
        Assert.Equal("KVMpad-7F3A._kvmpad._tcp.local", device.InstanceName);
    }

    [Theory]
    [InlineData("KVMpad-ABCD")]
    [InlineData("KVMpad-ABCD._kvmpad._tcp.local")]
    [InlineData("KVMPAD-ABCD._KVMPAD._TCP.LOCAL.")]
    public void DiscoveryRetainsTheActualCredentialIdOnRefreshAndAfterRestart(string storedId)
    {
        var stored = new PairedDevice(storedId, "Old name", "192.0.2.10", 8443, new string('a', 64),
            "fixture", DateTimeOffset.UnixEpoch);
        var seen = Assert.Single(DeviceCollator.Collate([MdnsMessage.Parse(
            BuildAnswer("KVMpad-ABCD", 9443, "New name", "1", false))]));

        foreach (var result in new[] {
            DeviceListEntry.Merge([], [stored]),
            DeviceListEntry.Merge([seen, seen], [stored]),
            DeviceListEntry.Merge([], [stored]),
            DeviceListEntry.Merge([seen], [stored])
        })
        {
            var row = Assert.Single(result);
            Assert.Equal(storedId, row.DeviceId);
            Assert.Same(stored, row.Paired);
        }
        var visible = Assert.Single(DeviceListEntry.Merge([seen], [stored]));
        Assert.Equal(9443, visible.Discovered!.Port);
        Assert.NotEqual(stored.Host, visible.Discovered.ConnectHost);
    }

    [Fact]
    public void ManualPairingKeepsTheConfirmedBoardIdentityAcrossAddressAndNameChanges()
    {
        string pin = new string('a', 64), other = new string('b', 64);
        const string name = "KVMpad-ABCD";
        var saved = new PairedDevice(name + "._kvmpad._tcp.local", name, "192.0.2.10", 8443,
            pin, "fixture", DateTimeOffset.UnixEpoch);
        Assert.Equal(name, DeviceListEntry.IdForManualPairing([], name, pin));
        Assert.Equal(saved.DeviceId, DeviceListEntry.IdForManualPairing([saved], "Renamed", pin.ToUpperInvariant()));
        string distinct = DeviceListEntry.IdForManualPairing([saved], name, other);
        Assert.True(Guid.TryParseExact(distinct, "N", out _));
        var second = new PairedDevice(distinct, name, "192.0.2.11", 8443, other, "fixture-2", DateTimeOffset.UnixEpoch);
        Assert.Equal(distinct, DeviceListEntry.IdForManualPairing([saved, second], name, other));
        Assert.True(Guid.TryParseExact(DeviceListEntry.IdForManualPairing([], "", other), "N", out _));
    }

    [Fact]
    public void DevicesWithMatchingAddressAndDisplayNameHaveSeparateCredentials()
    {
        var seen = Assert.Single(DeviceCollator.Collate([MdnsMessage.Parse(
            BuildAnswer("Desk.one", 8443, "KVMpad", "1", false))]));
        var stored = new PairedDevice("Desk.two", seen.DisplayName, seen.ConnectHost, seen.Port,
            new string('a', 64), "fixture", DateTimeOffset.UnixEpoch);
        var rows = DeviceListEntry.Merge([seen], [stored]);
        Assert.Collection(rows, live => Assert.Null(live.Paired), offline => Assert.Same(stored, offline.Paired));
        Assert.Equal("Desk.one", DeviceListEntry.InstanceKey("Desk.one._kvmpad._tcp.local"));
    }

    [Fact]
    public void WhenBothLegacyIdsExistTheLatestPairingWinsRegardlessOfEnumerationOrder()
    {
        var older = new PairedDevice("KVMpad-ABCD._kvmpad._tcp.local", "Desk", "192.0.2.10", 8443,
            new string('a', 64), "fixture-old", DateTimeOffset.UnixEpoch);
        var newer = new PairedDevice("KVMpad-ABCD", "Desk", "192.0.2.10", 8443,
            new string('b', 64), "fixture-new", DateTimeOffset.UnixEpoch.AddDays(1));
        Assert.Same(newer, Assert.Single(DeviceListEntry.Merge([], [older, newer])).Paired);
        Assert.Same(newer, Assert.Single(DeviceListEntry.Merge([], [newer, older])).Paired);
    }

    [Theory]
    [InlineData("KVMpad-ABCD")]
    [InlineData("KVMPAD-ABCD._KVMPAD._TCP.LOCAL.")]
    public async Task PairedCommandsUseCurrentAddressAndPortWithTheSelectedCredential(string storedId)
    {
        var paired = new PairedDevice(storedId, "Paired desk", "192.0.2.10", 8443, new string('a', 64),
            "fixture-token", DateTimeOffset.UnixEpoch);
        var devices = DeviceCollator.Collate([
            MdnsMessage.Parse(BuildAnswer("Other", 9999, "Paired desk", "1", false)),
            MdnsMessage.Parse(BuildAnswer("KVMpad-ABCD", 9443, "Renamed desk", "1", false))
        ]);
        int queries = 0;
        var current = await MdnsBrowser.ResolvePairedAsync(paired, CancellationToken.None, (window, _) => {
            ++queries;
            Assert.InRange(window, TimeSpan.FromMilliseconds(500), TimeSpan.FromSeconds(3));
            return Task.FromResult(devices);
        });
        Assert.Equal(1, queries);
        Assert.Equal("192.168.1.50", current.Host);
        Assert.Equal(9443, current.Port);
        Assert.Equal(paired.DeviceId, current.DeviceId);
        Assert.Equal(paired.DeviceName, current.DeviceName);
        Assert.Equal(paired.PinnedFingerprint, current.PinnedFingerprint);
        Assert.Equal(paired.Token, current.Token);
        Assert.Equal(paired.PairedAt, current.PairedAt);
        Assert.Equal("192.0.2.10", paired.Host);
        Assert.Equal(8443, paired.Port);
    }

    [Theory]
    [InlineData("no announcements")]
    [InlineData("other instance")]
    [InlineData("no port")]
    [InlineData("socket unavailable")]
    [InlineData("interface unavailable")]
    public async Task PairedCommandsKeepSavedEndpointWhenDiscoveryIsUnavailable(string scenario)
    {
        var paired = new PairedDevice("Desk", "My desk", "192.0.2.10", 8443, new string('a', 64),
            "fixture", DateTimeOffset.UnixEpoch);
        Task<IReadOnlyList<DiscoveredDevice>> Browse(TimeSpan window, CancellationToken token) => scenario switch {
            "no announcements" => Task.FromResult<IReadOnlyList<DiscoveredDevice>>([]),
            "socket unavailable" => throw new SocketException((int)SocketError.AddressAlreadyInUse),
            "interface unavailable" => throw new NetworkInformationException(),
            _ => Task.FromResult(DeviceCollator.Collate([MdnsMessage.Parse(BuildAnswer(
                scenario == "no port" ? "Desk" : "Different", scenario == "no port" ? 0 : 9443,
                paired.DeviceName, "1", false))]))
        };
        Assert.Same(paired, await MdnsBrowser.ResolvePairedAsync(paired, CancellationToken.None, Browse));
    }

    [Fact]
    public async Task CancelledDiscoveryCannotStartAPairedConnection()
    {
        using var stop = new CancellationTokenSource();
        var paired = new PairedDevice("Desk", "My desk", "192.0.2.10", 8443, new string('a', 64),
            "fixture", DateTimeOffset.UnixEpoch);
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            MdnsBrowser.ResolvePairedAsync(paired, stop.Token, (_, _) => {
                stop.Cancel();
                return Task.FromResult<IReadOnlyList<DiscoveredDevice>>([]);
            }));
    }

    [Fact]
    public void PairingClosedAndAnIncompatibleVersionAreBothVisible()
    {
        MdnsResponse response = MdnsMessage.Parse(
            BuildAnswer("KVMpad-0001", 8443, "KVMpad-0001", version: "2", pairingOpen: false));

        DiscoveredDevice device = Assert.Single(DeviceCollator.Collate([response]));

        Assert.False(device.PairingOpen);
        Assert.Equal(2, device.ProtocolVersion);
        Assert.False(device.IsCompatible);
    }

    [Fact]
    public void TheTxtKeysAreTheThreeAdr029Names()
    {
        // ADR-029 §4 fixes the service and the TXT keys: `name`, `v=1`, `pair=1|0`. The names are what the
        // firmware advertises, so a rename on either side makes every device invisible — spell them out here
        // rather than only exercising them through the collator.
        Assert.Equal("_kvmpad._tcp.local", ProtocolLimits.ServiceType);

        MdnsResponse response = MdnsMessage.Parse(
            BuildAnswer("KVMpad-7F3A", 8443, "Lab unit", version: "1", pairingOpen: true));
        Dictionary<string, string> text = response.TextRecords["KVMpad-7F3A._kvmpad._tcp.local"];

        Assert.Equal(["name", "pair", "v"], Sorted(text.Keys));
        Assert.Equal("Lab unit", text["name"]);
        Assert.Equal("1", text["v"]);
        Assert.Equal("1", text["pair"]);

        // And the advertised `name` is what the list shows, not the instance label.
        Assert.Equal("Lab unit", Assert.Single(DeviceCollator.Collate([response])).DisplayName);
    }

    private static List<string> Sorted(IEnumerable<string> values)
    {
        var list = new List<string>(values);
        list.Sort(StringComparer.Ordinal);
        return list;
    }

    [Fact]
    public void NameCompressionIsFollowed()
    {
        // The SRV and TXT owner names in the datagram below are compression pointers back to the PTR target,
        // which is how a real responder writes them.
        byte[] datagram = BuildAnswer("KVMpad-7F3A", 8443, "Lab device", "1", true, compressOwnerNames: true);

        MdnsResponse response = MdnsMessage.Parse(datagram);

        Assert.True(response.Services.ContainsKey("KVMpad-7F3A._kvmpad._tcp.local"));
        Assert.True(response.TextRecords.ContainsKey("KVMpad-7F3A._kvmpad._tcp.local"));
        DiscoveredDevice device = Assert.Single(DeviceCollator.Collate([response]));
        Assert.Equal("Lab device", device.DisplayName);
    }

    [Fact]
    public void RecordsSpreadAcrossTwoDatagramsStillMerge()
    {
        byte[] first = BuildAnswer("KVMpad-7F3A", 8443, "KVMpad-7F3A", "1", false, ptrOnly: true);
        byte[] second = BuildAnswer("KVMpad-7F3A", 8443, "KVMpad-7F3A", "1", false, skipPtr: true);

        IReadOnlyList<DiscoveredDevice> devices = DeviceCollator.Collate(
            [MdnsMessage.Parse(first), MdnsMessage.Parse(second)]);

        DiscoveredDevice device = Assert.Single(devices);
        Assert.Equal(8443, device.Port);
    }

    [Fact]
    public void AnInstanceWithoutAnSrvRecordIsNotOfferedToConnectTo()
    {
        // Without a port there is nothing to connect to, and inventing a default would be a guess.
        byte[] ptrOnly = BuildAnswer("KVMpad-7F3A", 8443, "KVMpad-7F3A", "1", false, ptrOnly: true);

        Assert.Empty(DeviceCollator.Collate([MdnsMessage.Parse(ptrOnly)]));
    }

    [Fact]
    public void ACompressionPointerLoopIsRejectedRatherThanLoopingForever()
    {
        // A hostile datagram: a name whose compression pointer points at itself.
        byte[] datagram =
        [
            0x00, 0x00, 0x84, 0x00,
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
            0xC0, 0x0C, // the name at offset 12 points to offset 12
        ];

        Assert.Throws<MdnsFormatException>(() => MdnsMessage.Parse(datagram));
    }

    [Theory]
    [InlineData(new byte[] { 0x00 })]
    [InlineData(new byte[] { 0x00, 0x00, 0x84, 0x00, 0x00, 0x00, 0x00, 0x05 })]
    public void ATruncatedDatagramIsRejected(byte[] datagram)
    {
        Assert.Throws<MdnsFormatException>(() => MdnsMessage.Parse(datagram));
    }

    [Fact]
    public void ARecordLengthPastTheEndOfTheDatagramIsRejected()
    {
        var bytes = new List<byte>
        {
            0x00, 0x00, 0x84, 0x00,
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        };
        WriteName(bytes, "a.local");
        bytes.AddRange([0x00, (byte)DnsRecordType.Txt]);
        bytes.AddRange([0x80, 0x01]);
        bytes.AddRange([0x00, 0x00, 0x00, 0x78]);
        bytes.AddRange([0xFF, 0xFF]); // claims 65535 bytes of data
        bytes.Add(0x01);

        Assert.Throws<MdnsFormatException>(() => MdnsMessage.Parse(bytes.ToArray()));
    }

    [Fact]
    public void TxtEntriesWithoutAnEqualsSignBecomeEmptyValues()
    {
        var bytes = new List<byte>
        {
            0x00, 0x00, 0x84, 0x00,
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        };
        WriteName(bytes, "x._kvmpad._tcp.local");
        bytes.AddRange([0x00, (byte)DnsRecordType.Txt]);
        bytes.AddRange([0x80, 0x01]);
        bytes.AddRange([0x00, 0x00, 0x00, 0x78]);
        byte[] data = [4, (byte)'f', (byte)'l', (byte)'a', (byte)'g', 3, (byte)'v', (byte)'=', (byte)'1'];
        bytes.AddRange([(byte)(data.Length >> 8), (byte)(data.Length & 0xFF)]);
        bytes.AddRange(data);

        MdnsResponse response = MdnsMessage.Parse(bytes.ToArray());
        Dictionary<string, string> text = response.TextRecords["x._kvmpad._tcp.local"];

        Assert.Equal(string.Empty, text["flag"]);
        Assert.Equal("1", text["v"]);
    }

    [Fact]
    public void ChangedDhcpAddressReplacesOnlyItsFamilyAndKeepsTheWholeNewSet()
    {
        var old = EndpointRecords(0, port: 8443, addresses: ["192.0.2.10", "2001:db8::1"]);
        var changed = EndpointRecords(1500, port: 9443, addresses: ["192.0.2.20", "192.0.2.21"]);
        var device = Assert.Single(DeviceCollator.Collate([old, changed]));
        Assert.Equal("192.0.2.20", device.ConnectHost);
        Assert.Equal(9443, device.Port);
        Assert.Equal(["192.0.2.20", "192.0.2.21", "2001:db8::1"], device.Addresses.Select(ip => ip.ToString()));
        var ipv6Only = EndpointRecords(2600, addresses: ["2001:db8::2"]);
        device = Assert.Single(DeviceCollator.Collate([old, changed, ipv6Only]));
        Assert.Equal("192.0.2.20", device.ConnectHost);
        Assert.Equal(["192.0.2.20", "192.0.2.21", "2001:db8::2"], device.Addresses.Select(ip => ip.ToString()).Order());
    }

    [Fact]
    public void SplitFlushAndSharedAnswersPreserveMultipleCurrentAddresses()
    {
        var first = EndpointRecords(0, port: 8443, addresses: ["192.0.2.10"]);
        var second = EndpointRecords(100, addresses: ["192.0.2.11"]);
        var shared = EndpointRecords(1600, flush: false, addresses: ["192.0.2.12", "192.0.2.12"]);
        var device = Assert.Single(DeviceCollator.Collate([first, second, shared]));
        Assert.Equal(["192.0.2.10", "192.0.2.11", "192.0.2.12"],
            device.Addresses.Select(ip => ip.ToString()).Order());
    }

    [Fact]
    public void GoodbyeWithdrawsOnlyTheNamedAddressAndMatchingSrv()
    {
        var first = EndpointRecords(0, port: 8443, addresses: ["192.0.2.10", "2001:db8::1"]);
        var current = EndpointRecords(3, port: 9443, addresses: ["192.0.2.20", "2001:db8::2"]);
        var goodbye = EndpointRecords(8, port: 8443, ttl: 0, addresses: ["192.0.2.10", "2001:db8::1"]);
        var device = Assert.Single(DeviceCollator.Collate([first, current, goodbye]));
        Assert.Equal(9443, device.Port);
        Assert.Equal(["192.0.2.20", "2001:db8::2"], device.Addresses.Select(ip => ip.ToString()));
        Assert.Empty(DeviceCollator.Collate([first, current, goodbye, EndpointRecords(9, port: 9443, ttl: 0)]));
        var reannounced = EndpointRecords(10, port: 8443, addresses: ["192.0.2.10"]);
        var restored = Assert.Single(DeviceCollator.Collate([first, goodbye, reannounced]));
        Assert.Equal(8443, restored.Port);
        Assert.Equal("192.0.2.10", restored.ConnectHost);
    }

    [Fact]
    public void KnownAnswersInAQueryCannotRestoreAnOldEndpoint()
    {
        var current = EndpointRecords(0, port: 9443, addresses: ["192.0.2.20"]);
        var query = EndpointRecords(2000, port: 8443, response: false, addresses: ["192.0.2.10"]);
        var device = Assert.Single(DeviceCollator.Collate([current, query]));
        Assert.Equal(9443, device.Port);
        Assert.Equal("192.0.2.20", Assert.Single(device.Addresses).ToString());
    }

    // ---- datagram construction -------------------------------------------------------------------

    private static MdnsResponse EndpointRecords(long timeMs, int? port = null, uint ttl = 120, bool flush = true,
        bool response = true, params string[] addresses)
    {
        var records = new List<(string Owner, DnsRecordType Type, byte[] Data)>();
        if (port is int p)
        {
            var srv = new List<byte> { 0, 0, 0, 0, (byte)(p >> 8), (byte)p };
            WriteName(srv, "Desk.local");
            records.Add(("Desk._kvmpad._tcp.local", DnsRecordType.Srv, srv.ToArray()));
        }
        foreach (var address in addresses)
        {
            byte[] data = IPAddress.Parse(address).GetAddressBytes();
            records.Add(("Desk.local", data.Length == 4 ? DnsRecordType.A : DnsRecordType.Aaaa, data));
        }
        var message = new List<byte> { 0, 0, response ? (byte)0x84 : (byte)0, 0, 0, 0, 0, (byte)records.Count, 0, 0, 0, 0 };
        foreach (var (owner, type, data) in records)
        {
            WriteName(message, owner);
            message.AddRange([0, (byte)type, flush ? (byte)0x80 : (byte)0, 1,
                (byte)(ttl >> 24), (byte)(ttl >> 16), (byte)(ttl >> 8), (byte)ttl, 0, (byte)data.Length]);
            message.AddRange(data);
        }
        var parsed = MdnsMessage.Parse(message.ToArray());
        parsed.ReceivedAtMs = timeMs;
        return parsed;
    }

    private static byte[] BuildAnswer(
        string instance,
        int port,
        string displayName,
        string version,
        bool pairingOpen,
        bool compressOwnerNames = false,
        bool ptrOnly = false,
        bool skipPtr = false)
    {
        const string serviceType = "_kvmpad._tcp.local";
        string instanceName = instance + "." + serviceType;
        string hostName = instance + ".local";

        var records = new List<Action<List<byte>>>();
        var bytes = new List<byte>();

        // Header: a response with the authoritative-answer flag set.
        bytes.AddRange([0x00, 0x00, 0x84, 0x00]);
        int answerCount = ptrOnly ? 1 : skipPtr ? 3 : 4;
        bytes.AddRange([0x00, 0x00]);
        bytes.AddRange([(byte)(answerCount >> 8), (byte)(answerCount & 0xFF)]);
        bytes.AddRange([0x00, 0x00, 0x00, 0x00]);

        int instanceNameOffset = -1;

        if (!skipPtr)
        {
            WriteName(bytes, serviceType);
            bytes.AddRange([0x00, (byte)DnsRecordType.Ptr]);
            bytes.AddRange([0x00, 0x01]);
            bytes.AddRange([0x00, 0x00, 0x00, 0x78]);
            var target = new List<byte>();
            WriteName(target, instanceName);
            bytes.AddRange([(byte)(target.Count >> 8), (byte)(target.Count & 0xFF)]);
            instanceNameOffset = bytes.Count;
            bytes.AddRange(target);
        }

        if (ptrOnly)
            return bytes.ToArray();

        bool useCompression = compressOwnerNames && instanceNameOffset >= 0 && instanceNameOffset < 0x3FFF;

        void WriteOwner()
        {
            if (useCompression)
                bytes.AddRange([(byte)(0xC0 | (instanceNameOffset >> 8)), (byte)(instanceNameOffset & 0xFF)]);
            else
                WriteName(bytes, instanceName);
        }

        // SRV
        WriteOwner();
        bytes.AddRange([0x00, (byte)DnsRecordType.Srv]);
        bytes.AddRange([0x80, 0x01]);
        bytes.AddRange([0x00, 0x00, 0x00, 0x78]);
        var srv = new List<byte> { 0x00, 0x00, 0x00, 0x00, (byte)(port >> 8), (byte)(port & 0xFF) };
        WriteName(srv, hostName);
        bytes.AddRange([(byte)(srv.Count >> 8), (byte)(srv.Count & 0xFF)]);
        bytes.AddRange(srv);

        // TXT
        WriteOwner();
        bytes.AddRange([0x00, (byte)DnsRecordType.Txt]);
        bytes.AddRange([0x80, 0x01]);
        bytes.AddRange([0x00, 0x00, 0x00, 0x78]);
        var txt = new List<byte>();
        AddTxtItem(txt, "name=" + displayName);
        AddTxtItem(txt, "v=" + version);
        AddTxtItem(txt, "pair=" + (pairingOpen ? "1" : "0"));
        bytes.AddRange([(byte)(txt.Count >> 8), (byte)(txt.Count & 0xFF)]);
        bytes.AddRange(txt);

        // A
        WriteName(bytes, hostName);
        bytes.AddRange([0x00, (byte)DnsRecordType.A]);
        bytes.AddRange([0x80, 0x01]);
        bytes.AddRange([0x00, 0x00, 0x00, 0x78]);
        bytes.AddRange([0x00, 0x04]);
        bytes.AddRange([192, 168, 1, 50]);

        _ = records;
        return bytes.ToArray();
    }

    private static void AddTxtItem(List<byte> bytes, string item)
    {
        byte[] encoded = Encoding.UTF8.GetBytes(item);
        bytes.Add((byte)encoded.Length);
        bytes.AddRange(encoded);
    }

    private static void WriteName(List<byte> bytes, string name)
    {
        foreach (string label in name.Split('.', StringSplitOptions.RemoveEmptyEntries))
        {
            byte[] encoded = Encoding.UTF8.GetBytes(label);
            bytes.Add((byte)encoded.Length);
            bytes.AddRange(encoded);
        }
        bytes.Add(0x00);
    }
}
