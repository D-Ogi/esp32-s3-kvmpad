// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
using System;
using System.Collections.Generic;
using System.Linq;
using KVMpad.Client.Core.Protocol;
using KVMpad.Client.Core.Storage;

namespace KVMpad.Client.Core.Discovery;

/// <summary>A discovered endpoint and its stored pairing, either of which may be present on its own.</summary>
public sealed class DeviceListEntry
{
    private DeviceListEntry(DiscoveredDevice? discovered, PairedDevice? paired)
    {
        Discovered = discovered;
        Paired = paired;
    }

    public DiscoveredDevice? Discovered { get; }
    public PairedDevice? Paired { get; }
    public PairedDevice? PairedForConnection =>
        Discovered is { Port: > 0 and <= 65535 } seen && !string.IsNullOrWhiteSpace(seen.ConnectHost)
            ? Paired?.WithEndpoint(seen.ConnectHost, seen.Port)
            : Paired;
    public string DeviceId => Paired?.DeviceId ?? Discovered!.InstanceName;

    public static string InstanceKey(string id)
    {
        const string suffix = "." + ProtocolLimits.ServiceType;
        string name = id.TrimEnd('.');
        return name.EndsWith(suffix, StringComparison.OrdinalIgnoreCase) ? name[..^suffix.Length] : name;
    }

    public static string IdForManualPairing(IReadOnlyList<PairedDevice> stored, string name, string confirmedPin)
    {
        var prior = stored.Where(p => string.Equals(p.PinnedFingerprint, confirmedPin, StringComparison.OrdinalIgnoreCase))
            .OrderByDescending(p => p.PairedAt).ThenBy(p => p.DeviceId, StringComparer.Ordinal).FirstOrDefault();
        if (prior != null) return prior.DeviceId;
        // The board normally uses its confirmed name as its DNS-SD instance.
        if (string.IsNullOrWhiteSpace(name) || stored.Any(p =>
            string.Equals(InstanceKey(p.DeviceId), InstanceKey(name), StringComparison.OrdinalIgnoreCase)))
            return Guid.NewGuid().ToString("N");
        return name;
    }

    public static IReadOnlyList<DeviceListEntry> Merge(IEnumerable<DiscoveredDevice> discovered,
        IEnumerable<PairedDevice> paired)
    {
        // Older stores can contain a short ID as well as the full instance name. Prefer the latest pairing
        // and keep its actual credential key and certificate pin throughout connection and HOST startup.
        var stored = paired.GroupBy(p => InstanceKey(p.DeviceId), StringComparer.OrdinalIgnoreCase)
            .ToDictionary(g => g.Key, g => g.OrderByDescending(p => p.PairedAt)
                .ThenBy(p => p.DeviceId, StringComparer.Ordinal).First(), StringComparer.OrdinalIgnoreCase);
        var rows = new List<DeviceListEntry>();
        foreach (var seen in discovered.DistinctBy(d => InstanceKey(d.InstanceName), StringComparer.OrdinalIgnoreCase))
        {
            stored.Remove(InstanceKey(seen.InstanceName), out var credential);
            rows.Add(new DeviceListEntry(seen, credential));
        }
        rows.AddRange(stored.Values.Select(p => new DeviceListEntry(null, p)));
        return rows;
    }
}
