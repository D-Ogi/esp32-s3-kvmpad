// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.Text;
using KVMpad.Client.Core.Discovery;
using KVMpad.Client.Core.Protocol;
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

    // ---- datagram construction -------------------------------------------------------------------

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
