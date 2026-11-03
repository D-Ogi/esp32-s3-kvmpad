// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Text;

namespace KVMpad.Client.Core.Discovery;

/// <summary>DNS record types this browser understands.</summary>
public enum DnsRecordType : ushort
{
    A = 1,
    Ptr = 12,
    Txt = 16,
    Aaaa = 28,
    Srv = 33,
    Any = 255,
}

/// <summary>Raised when a datagram is not a DNS message this parser can read.</summary>
public sealed class MdnsFormatException : Exception
{
    public MdnsFormatException(string message) : base(message) { }
}

/// <summary>An <c>SRV</c> record's data.</summary>
public readonly struct SrvData
{
    public SrvData(ushort priority, ushort weight, ushort port, string target)
    {
        Priority = priority;
        Weight = weight;
        Port = port;
        Target = target;
    }

    public ushort Priority { get; }
    public ushort Weight { get; }
    public ushort Port { get; }
    public string Target { get; }
}

/// <summary>Everything a single mDNS datagram told us, grouped by record owner name.</summary>
public sealed class MdnsResponse
{
    /// <summary>Service instance names from <c>PTR</c> records, keyed by the service type that owns them.</summary>
    public Dictionary<string, List<string>> Pointers { get; } = new(StringComparer.OrdinalIgnoreCase);

    public Dictionary<string, SrvData> Services { get; } = new(StringComparer.OrdinalIgnoreCase);

    public Dictionary<string, Dictionary<string, string>> TextRecords { get; } = new(StringComparer.OrdinalIgnoreCase);

    public Dictionary<string, List<IPAddress>> Addresses { get; } = new(StringComparer.OrdinalIgnoreCase);
}

/// <summary>
/// A minimal DNS-SD codec: build one <c>PTR</c> question and read the answers.
/// </summary>
/// <remarks>
/// A query on UDP 5353 to the multicast group is all discovery needs (ADR-022 FR-723), so the client speaks
/// just enough DNS itself instead of installing a resolver service or taking a dependency on one. Parsing is
/// the part that touches data from the network, so it is a pure function over a span, with no allocation of
/// anything it has not length-checked, and it is unit-tested against canned datagrams rather than a live LAN.
/// </remarks>
public static class MdnsMessage
{
    public const int Port = 5353;
    public const string MulticastAddressV4 = "224.0.0.251";

    /// <summary>Builds a one-shot <c>PTR</c> query for a service type, e.g. <c>_kvmpad._tcp.local</c>.</summary>
    public static byte[] BuildPointerQuery(string serviceType)
    {
        if (string.IsNullOrWhiteSpace(serviceType))
            throw new ArgumentException("Service type is required.", nameof(serviceType));

        var bytes = new List<byte>(64);
        // Transaction id 0 and no flags: a multicast DNS query, not a unicast one.
        bytes.AddRange([0x00, 0x00, 0x00, 0x00]);
        bytes.AddRange([0x00, 0x01]); // one question
        bytes.AddRange([0x00, 0x00]); // no answers
        bytes.AddRange([0x00, 0x00]); // no authority records
        bytes.AddRange([0x00, 0x00]); // no additional records

        WriteName(bytes, serviceType);
        bytes.AddRange([0x00, (byte)DnsRecordType.Ptr]);
        bytes.AddRange([0x00, 0x01]); // class IN, QU bit clear
        return bytes.ToArray();
    }

    /// <summary>Parses one datagram. Unknown record types are skipped, not treated as an error.</summary>
    public static MdnsResponse Parse(ReadOnlySpan<byte> datagram)
    {
        if (datagram.Length < 12)
            throw new MdnsFormatException("Datagram shorter than a DNS header.");

        int questionCount = ReadUInt16(datagram, 4);
        int answerCount = ReadUInt16(datagram, 6);
        int authorityCount = ReadUInt16(datagram, 8);
        int additionalCount = ReadUInt16(datagram, 10);

        var response = new MdnsResponse();
        int offset = 12;

        for (int i = 0; i < questionCount; i++)
        {
            ReadName(datagram, ref offset);
            Require(datagram, offset, 4);
            offset += 4;
        }

        int records = answerCount + authorityCount + additionalCount;
        for (int i = 0; i < records && offset < datagram.Length; i++)
            ReadResourceRecord(datagram, ref offset, response);

        return response;
    }

    private static void ReadResourceRecord(ReadOnlySpan<byte> datagram, ref int offset, MdnsResponse response)
    {
        string name = ReadName(datagram, ref offset);
        Require(datagram, offset, 10);
        var type = (DnsRecordType)ReadUInt16(datagram, offset);
        int length = ReadUInt16(datagram, offset + 8);
        offset += 10;
        Require(datagram, offset, length);
        int dataStart = offset;
        int dataEnd = offset + length;
        offset = dataEnd;

        switch (type)
        {
            case DnsRecordType.Ptr:
            {
                int cursor = dataStart;
                string target = ReadName(datagram, ref cursor);
                if (!response.Pointers.TryGetValue(name, out List<string>? list))
                    response.Pointers[name] = list = new List<string>();
                if (!list.Contains(target, StringComparer.OrdinalIgnoreCase))
                    list.Add(target);
                break;
            }

            case DnsRecordType.Srv:
            {
                if (length < 7)
                    break;
                ushort priority = ReadUInt16(datagram, dataStart);
                ushort weight = ReadUInt16(datagram, dataStart + 2);
                ushort port = ReadUInt16(datagram, dataStart + 4);
                int cursor = dataStart + 6;
                string target = ReadName(datagram, ref cursor);
                response.Services[name] = new SrvData(priority, weight, port, target);
                break;
            }

            case DnsRecordType.Txt:
            {
                Dictionary<string, string> pairs = ParseTxt(datagram.Slice(dataStart, length));
                response.TextRecords[name] = pairs;
                break;
            }

            case DnsRecordType.A:
            {
                if (length != 4)
                    break;
                var address = new IPAddress(datagram.Slice(dataStart, 4).ToArray());
                AddAddress(response, name, address);
                break;
            }

            case DnsRecordType.Aaaa:
            {
                if (length != 16)
                    break;
                var address = new IPAddress(datagram.Slice(dataStart, 16).ToArray());
                AddAddress(response, name, address);
                break;
            }
        }
    }

    private static void AddAddress(MdnsResponse response, string name, IPAddress address)
    {
        if (!response.Addresses.TryGetValue(name, out List<IPAddress>? list))
            response.Addresses[name] = list = new List<IPAddress>();
        if (!list.Contains(address))
            list.Add(address);
    }

    /// <summary>TXT data is a sequence of length-prefixed <c>key=value</c> strings (RFC 6763 §6).</summary>
    private static Dictionary<string, string> ParseTxt(ReadOnlySpan<byte> data)
    {
        var pairs = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        int offset = 0;
        while (offset < data.Length)
        {
            int itemLength = data[offset++];
            if (itemLength == 0 || offset + itemLength > data.Length)
                break;
            string item = Encoding.UTF8.GetString(data.Slice(offset, itemLength));
            offset += itemLength;
            int equals = item.IndexOf('=');
            if (equals < 0)
                pairs[item] = string.Empty;
            else
                pairs[item[..equals]] = item[(equals + 1)..];
        }
        return pairs;
    }

    private static void WriteName(List<byte> bytes, string name)
    {
        foreach (string label in name.Split('.', StringSplitOptions.RemoveEmptyEntries))
        {
            byte[] encoded = Encoding.UTF8.GetBytes(label);
            if (encoded.Length > 63)
                throw new ArgumentException("A DNS label may not exceed 63 bytes.", nameof(name));
            bytes.Add((byte)encoded.Length);
            bytes.AddRange(encoded);
        }
        bytes.Add(0x00);
    }

    /// <summary>
    /// Reads a possibly compressed name and leaves <paramref name="offset"/> just after it. Pointer following
    /// is bounded by a hop count, so a datagram whose pointers form a loop cannot hang the parser.
    /// </summary>
    private static string ReadName(ReadOnlySpan<byte> datagram, ref int offset)
    {
        var labels = new List<string>(6);
        int cursor = offset;
        int hops = 0;
        bool jumped = false;

        while (true)
        {
            if (cursor >= datagram.Length)
                throw new MdnsFormatException("Name runs past the end of the datagram.");

            byte length = datagram[cursor];
            if (length == 0)
            {
                cursor++;
                break;
            }

            if ((length & 0xC0) == 0xC0)
            {
                if (cursor + 1 >= datagram.Length)
                    throw new MdnsFormatException("Truncated compression pointer.");
                int pointer = ((length & 0x3F) << 8) | datagram[cursor + 1];
                if (!jumped)
                {
                    offset = cursor + 2;
                    jumped = true;
                }
                if (++hops > 16)
                    throw new MdnsFormatException("Compression pointer loop.");
                if (pointer >= datagram.Length)
                    throw new MdnsFormatException("Compression pointer outside the datagram.");
                cursor = pointer;
                continue;
            }

            if ((length & 0xC0) != 0)
                throw new MdnsFormatException("Reserved label length bits set.");

            cursor++;
            Require(datagram, cursor, length);
            labels.Add(Encoding.UTF8.GetString(datagram.Slice(cursor, length)));
            cursor += length;
        }

        if (!jumped)
            offset = cursor;
        return string.Join('.', labels);
    }

    private static ushort ReadUInt16(ReadOnlySpan<byte> span, int offset)
    {
        Require(span, offset, 2);
        return (ushort)((span[offset] << 8) | span[offset + 1]);
    }

    private static void Require(ReadOnlySpan<byte> span, int offset, int length)
    {
        if (offset < 0 || length < 0 || offset + length > span.Length)
            throw new MdnsFormatException("Datagram truncated.");
    }
}
