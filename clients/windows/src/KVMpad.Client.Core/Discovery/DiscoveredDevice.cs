// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using KVMpad.Client.Core.Protocol;

namespace KVMpad.Client.Core.Discovery;

/// <summary>
/// One KVMpad seen on the LAN. The user clicks the name shown on the device's own screen, which is the whole
/// point of discovery: it removes the IP address from what anyone has to type (ADR-022 FR-721/723).
/// </summary>
public sealed class DiscoveredDevice
{
    public DiscoveredDevice(
        string instanceName,
        string displayName,
        string hostName,
        int port,
        IReadOnlyList<IPAddress> addresses,
        int protocolVersion,
        bool pairingOpen)
    {
        InstanceName = instanceName;
        DisplayName = displayName;
        HostName = hostName;
        Port = port;
        Addresses = addresses;
        ProtocolVersion = protocolVersion;
        PairingOpen = pairingOpen;
    }

    /// <summary>The DNS-SD instance name, e.g. <c>KVMpad-7F3A._kvmpad._tcp.local</c>.</summary>
    public string InstanceName { get; }

    /// <summary>The TXT <c>name</c> value, which is what the device shows on its screen.</summary>
    public string DisplayName { get; }

    /// <summary>The SRV target host name.</summary>
    public string HostName { get; }

    public int Port { get; }

    public IReadOnlyList<IPAddress> Addresses { get; }

    /// <summary>The TXT <c>v</c> value. Anything but 1 is a device this client cannot talk to.</summary>
    public int ProtocolVersion { get; }

    /// <summary>The TXT <c>pair</c> value: 1 only while a pairing window is open on the device (FR-723).</summary>
    public bool PairingOpen { get; }

    /// <summary>True when this client speaks the device's protocol version.</summary>
    public bool IsCompatible => ProtocolVersion == ProtocolLimits.Version;

    /// <summary>The address to connect to: the first IPv4 address, else the first of any family, else the host name.</summary>
    public string ConnectHost
    {
        get
        {
            foreach (IPAddress address in Addresses)
            {
                if (address.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork)
                    return address.ToString();
            }
            return Addresses.Count > 0 ? Addresses[0].ToString() : HostName;
        }
    }

    public override string ToString() => $"{DisplayName} ({ConnectHost}:{Port})";
}

/// <summary>Turns raw mDNS answers into the device list the UI shows.</summary>
public static class DeviceCollator
{
    /// <summary>
    /// Merges the records of one or more datagrams into devices. A record set that lacks an SRV record yields
    /// nothing: without a port there is no device to connect to, and inventing one would be a guess.
    /// </summary>
    public static IReadOnlyList<DiscoveredDevice> Collate(
        IEnumerable<MdnsResponse> responses,
        string serviceType = ProtocolLimits.ServiceType)
    {
        var instances = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var services = new Dictionary<string, SrvData>(StringComparer.OrdinalIgnoreCase);
        var texts = new Dictionary<string, Dictionary<string, string>>(StringComparer.OrdinalIgnoreCase);
        var addresses = new Dictionary<string, Dictionary<IPAddress, (long TimeMs, int Packet)>>(StringComparer.OrdinalIgnoreCase);
        int packet = 0;

        foreach (MdnsResponse response in responses)
        {
            packet++;
            if (response.Pointers.TryGetValue(serviceType, out List<string>? pointed))
            {
                foreach (string instance in pointed)
                    instances.Add(instance);
            }
            foreach (var (owner, goodbye) in response.WithdrawnServices)
            {
                if (services.TryGetValue(owner, out var current) && current.Port == goodbye.Port &&
                    current.Priority == goodbye.Priority && current.Weight == goodbye.Weight &&
                    string.Equals(current.Target, goodbye.Target, StringComparison.OrdinalIgnoreCase))
                    services.Remove(owner);
            }
            foreach (KeyValuePair<string, SrvData> service in response.Services)
                services[service.Key] = service.Value;
            foreach (KeyValuePair<string, Dictionary<string, string>> text in response.TextRecords)
                texts[text.Key] = text.Value;
            foreach (var (owner, withdrawn) in response.WithdrawnAddresses)
                if (addresses.TryGetValue(owner, out var previous)) previous.Remove(withdrawn);
            foreach (var (owner, announced) in response.Addresses)
            {
                if (!addresses.TryGetValue(owner, out var known))
                    addresses[owner] = known = [];
                // A unique RRset may span a burst of packets. Preserve siblings announced in the
                // preceding second (RFC 6762 §10.2), and prefer the most recent answers for connection.
                if (response.AddressFlushes.TryGetValue(owner, out var families))
                {
                    foreach (var (ip, lastSeen) in known)
                        if (families.Contains(ip.AddressFamily) && response.ReceivedAtMs - lastSeen.TimeMs >= 1000)
                            known.Remove(ip);
                }
                foreach (var ip in announced)
                    known[ip] = (response.ReceivedAtMs, packet);
            }
        }

        // A device that answered with SRV but whose PTR landed in another datagram is still a device.
        foreach (string serviceName in services.Keys)
        {
            if (serviceName.EndsWith(serviceType, StringComparison.OrdinalIgnoreCase))
                instances.Add(serviceName);
        }

        var devices = new List<DiscoveredDevice>();
        foreach (string instance in instances)
        {
            if (!services.TryGetValue(instance, out SrvData service))
                continue;

            texts.TryGetValue(instance, out Dictionary<string, string>? text);
            string display = text is not null && text.TryGetValue("name", out string? name) && name.Length > 0
                ? name
                : FirstLabel(instance);

            int version = 0;
            if (text is not null && text.TryGetValue("v", out string? versionText))
                _ = int.TryParse(versionText, out version);

            bool pairing = text is not null && text.TryGetValue("pair", out string? pair) && pair == "1";

            IReadOnlyList<IPAddress> deviceAddresses = addresses.TryGetValue(service.Target, out var known)
                ? known.OrderByDescending(item => item.Value.Packet).Select(item => item.Key).ToArray()
                : Array.Empty<IPAddress>();

            devices.Add(new DiscoveredDevice(
                instance,
                display,
                service.Target,
                service.Port,
                deviceAddresses,
                version,
                pairing));
        }

        devices.Sort(static (a, b) => string.Compare(a.DisplayName, b.DisplayName, StringComparison.OrdinalIgnoreCase));
        return devices;
    }

    private static string FirstLabel(string instanceName)
    {
        int dot = instanceName.IndexOf('.');
        return dot > 0 ? instanceName[..dot] : instanceName;
    }
}
