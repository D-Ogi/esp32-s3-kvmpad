// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;

namespace KVMpad.Client.Core.Input;

/// <summary>One descriptor lookup per connected Raw Input handle. Allow PnP time to publish missing properties.</summary>
public sealed class RawInputIdentityCache(Func<nint, bool?> resolve)
{
    private readonly Dictionary<nint, (bool? Match, long RetryAfter)> _known = new();

    public bool? IsKvmpad(nint device) => IsKvmpad(device, Environment.TickCount64);

    internal bool? IsKvmpad(nint device, long now)
    {
        if (device == 0) return false;
        if (_known.TryGetValue(device, out var known) && (known.Match.HasValue || now < known.RetryAfter))
            return known.Match;
        bool? match = resolve(device);
        _known[device] = (match, now + 1000);
        return match;
    }

    public bool? IsAttached(IEnumerable<nint> inputs)
    {
        bool? attached = false;
        foreach (nint input in inputs) {
            bool? match = IsKvmpad(input);
            if (match == true) return true;
            if (match == null) attached = null;
        }
        return attached;
    }

    public static bool AllowCapture(bool? attached, Action<bool> stopCapture)
    {
        if (attached == false) return true;
        stopCapture(attached == true);
        return false;
    }

    public void Forget(nint device) => _known.Remove(device);
    public void Clear() => _known.Clear();
}
