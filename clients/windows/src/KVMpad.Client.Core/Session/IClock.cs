// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Diagnostics;

namespace KVMpad.Client.Core.Session;

/// <summary>
/// A monotonic clock. Everything with a deadline — the heartbeat, the rate window, the "type text" pacing —
/// reads time through this interface so the tests can drive it instead of sleeping.
/// </summary>
public interface IClock
{
    /// <summary>Time since an arbitrary fixed origin. Never goes backwards and is unaffected by wall-clock changes.</summary>
    TimeSpan Elapsed { get; }
}

/// <summary>The production clock, backed by <see cref="Stopwatch"/>.</summary>
public sealed class SystemClock : IClock
{
    private readonly Stopwatch _stopwatch = Stopwatch.StartNew();

    public static SystemClock Instance { get; } = new();

    public TimeSpan Elapsed => _stopwatch.Elapsed;
}
