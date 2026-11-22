// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

namespace KVMpad.Client.Core.Display;

public readonly record struct DisplayResolution(int Width, int Height)
{
    public override string ToString() => $"{Width} × {Height}";
}

public sealed class UsbMonitorDisplay
{
    internal DisplayPath Board { get; }
    internal DisplayPath[] Paths { get; }
    internal SourceMode Mode { get; }
    public string Serial => Board.UsbId!.Split('\\')[2];
    public string SourceName => Board.Name;
    public DisplayResolution Current => Mode.Size;
    public DisplayResolution? Signal => Board.Signal;
    public IReadOnlyList<DisplayResolution> Modes { get; }
    public string? UnavailableSignalMode => Signal is { } size && size != Current && !Modes.Contains(size)
        ? $"USB signal {size} is active. Windows is using a {Current} desktop and does not currently offer {size} at these display settings."
        : null;

    public bool HasSameSource(UsbMonitorDisplay other) => Board.UsbId == other.Board.UsbId &&
        Board.Source == other.Board.Source && Board.Target == other.Board.Target && SourceName == other.SourceName;

    public string? ChangeSinceApply(UsbMonitorDisplay? applied)
    {
        if (applied == null || Board.UsbId != applied.Board.UsbId) return null;
        bool reattached = !HasSameSource(applied);
        if (Current != applied.Current || Signal != applied.Signal)
        {
            string change = reattached ? "Windows reconnected this display" : "Windows changed the display settings";
            return $"{change} after you applied {applied.Current}. The desktop is now {Current}.";
        }
        return reattached ? $"Windows reconnected this display. The desktop is still {Current}." : null;
    }

    internal UsbMonitorDisplay(DisplayPath board, DisplayPath[] paths, SourceMode current, DisplayResolution[] modes)
    {
        Board = board;
        Paths = paths;
        Mode = current;
        Modes = Array.AsReadOnly(modes);
    }
}

internal readonly record struct DisplayEndpoint(uint Low, int High, uint Id);
internal readonly record struct DisplayPosition(int X, int Y);
internal sealed record DisplayPath(DisplayEndpoint Source, DisplayEndpoint Target, string Name, string? UsbId,
    int X, int Y, DisplayResolution Size, DisplayResolution? Signal, uint Rotation, uint Scaling,
    uint RefreshNumerator, uint RefreshDenominator, uint OutputTechnology, uint Scanline);
internal readonly record struct SourceMode(DisplayResolution Size, int Hertz, int Orientation, int Bits, int Flags);

internal interface IDisplayApi
{
    DisplayPath[] ActivePaths();
    SourceMode Current(string name);
    IEnumerable<SourceMode> Offered(string name);
    void Change(string name, SourceMode mode, bool test, DisplayPosition? position);
}

public sealed class UsbMonitorDisplayService
{
    private readonly IDisplayApi api;
    // EnumDisplaySettingsEx initializes its enumeration at index zero.
    private static readonly object Gate = new();
    public UsbMonitorDisplayService() : this(new WindowsDisplayApi()) { }
    internal UsbMonitorDisplayService(IDisplayApi api) => this.api = api;

    public UsbMonitorDisplay Read()
    {
        lock (Gate) return Snapshot();
    }

    public UsbMonitorDisplay MatchSignal(string serial, DisplayResolution expected)
    {
        var observed = Read();
        if (!String.Equals(observed.Serial, serial, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("The active USB display belongs to a different KVMpad.");
        if (observed.Signal != expected)
            throw new InvalidOperationException("The USB display signal does not match the requested resolution.");
        return RequireSignal(Apply(observed, expected), expected);
    }

    public static IReadOnlyList<DisplayResolution> Choices(UsbMonitorDisplay display, bool pairedProfile)
        => pairedProfile ? display.Modes.Concat(new DisplayResolution[] { new(640,480), new(1024,600) }
            .Where(size => size != display.Signal)).Distinct()
            .OrderBy(s => s.Width).ThenBy(s => s.Height).ToArray() : display.Modes;

    public async Task<UsbMonitorDisplay> ApplyProfileAsync(UsbMonitorDisplay observed, DisplayResolution size,
        Func<string,bool,CancellationToken,Task> request, CancellationToken cancellationToken)
    {
        if (size != new DisplayResolution(640,480) && size != new DisplayResolution(1024,600))
            return Apply(observed, size);
        var before = Read();
        if (before.Mode != observed.Mode || !before.Paths.SequenceEqual(observed.Paths)) throw Changed();
        if (before.Signal == size) return RequireSignal(Apply(before, size), size);
        var planned = PlanLayout(before, size);
        await request(before.Serial, size.Width == 1024, cancellationToken).ConfigureAwait(false);
        // A different GDI source name is normal when the driver reattaches.
        string? unavailable = null;
        for (int attempt = 0; attempt < 40; ++attempt)
        {
            await Task.Delay(300, cancellationToken).ConfigureAwait(false);
            UsbMonitorDisplay active;
            try { active = Read(); }
            catch (InvalidOperationException) { unavailable = null; continue; }
            if (active.Board.UsbId != before.Board.UsbId) throw Changed();
            unavailable = active.Signal == size ? active.UnavailableSignalMode : null;
            if (active.Signal != size || unavailable != null) continue;
            if (!before.Paths.Where(p => p.Target != before.Board.Target).SequenceEqual(
                 active.Paths.Where(p => p.Target != active.Board.Target))) throw Changed();
            var origin = new DisplayPosition(active.Board.X, active.Board.Y);
            if (origin != new DisplayPosition(before.Board.X, before.Board.Y) && origin != planned) throw Changed();
            return RequireSignal(Apply(active, size, planned), size);
        }
        throw new InvalidOperationException(unavailable ??
            "Windows did not confirm the requested USB signal. Check the monitor on KVMpad and refresh.");
    }

    public UsbMonitorDisplay Apply(UsbMonitorDisplay observed, DisplayResolution mode)
        => Apply(observed, mode, null);

    private static UsbMonitorDisplay RequireSignal(UsbMonitorDisplay result, DisplayResolution expected)
        => result.Signal == expected && result.Current == expected ? result :
            throw new InvalidOperationException("Windows did not confirm matching desktop and USB signal resolutions.");

    private UsbMonitorDisplay Apply(UsbMonitorDisplay observed, DisplayResolution mode, DisplayPosition? planned)
    {
        ArgumentNullException.ThrowIfNull(observed);
        lock (Gate)
        {
            UsbMonitorDisplay now = Snapshot();
            if (observed.Mode != now.Mode || !observed.Paths.SequenceEqual(now.Paths))
                throw Changed();
            var origin = new DisplayPosition(now.Board.X, now.Board.Y);
            if (now.Current == mode && (planned == null || planned == origin)) return now;
            if (!observed.Modes.Contains(mode) || !now.Modes.Contains(mode))
                throw new InvalidOperationException((now.Signal == mode ? now.UnavailableSignalMode : null) ??
                    "Windows does not offer that resolution at the current refresh rate and orientation.");
            var target = planned ?? PlanLayout(now, mode);
            DisplayPosition? position = target == origin ? null : target;
            var resized = now.Board with { Size = mode, X = target.X, Y = target.Y };
            SourceMode request = now.Mode with { Size = mode };
            api.Change(now.SourceName, request, test: true, position);
            if (!now.Paths.SequenceEqual(Topology()) || api.Current(now.SourceName) != now.Mode) throw Changed();
            api.Change(now.SourceName, request, test: false, position);
            UsbMonitorDisplay result = Snapshot();
            // The target signal and its scaling policy can reflect the new source dimensions.
            var expected = now.Paths.Select(p => p.Target == now.Board.Target
                ? resized with { Signal = result.Signal, Scaling = result.Board.Scaling } : p);
            if (result.Mode != request || !expected.SequenceEqual(result.Paths))
                throw new InvalidOperationException("Windows did not confirm the requested resolution with the original display layout. Refresh the display list.");
            return result;
        }
    }

    private static DisplayPosition PlanLayout(UsbMonitorDisplay now, DisplayResolution mode)
    {
        var board = now.Board;
        var others = now.Paths.Where(p => p.Target != board.Target).ToArray();
        bool Fits(DisplayPath resized) => !others.Any(p => Overlaps(p, resized)) && KeepsConnected(now.Paths, board, resized);
        var fixedOrigin = board with { Size = mode };
        if (Fits(fixedOrigin)) return new(board.X, board.Y);

        // Keep the origin of the primary monitor fixed. For an extended desktop,
        // its right or bottom edge can stay against the same neighboring display.
        if (board.X != 0 || board.Y != 0)
        {
            var touching = others.Where(p => Touches(board, p)).ToArray();
            long x = touching.Any(p => Right(board) == p.X) ? Right(board) - mode.Width : board.X;
            long y = touching.Any(p => Bottom(board) == p.Y) ? Bottom(board) - mode.Height : board.Y;
            foreach (var (left, top) in new[] { (x, (long)board.Y), ((long)board.X, y), (x, y) })
            {
                if ((left == board.X && top == board.Y) || left is < int.MinValue or > int.MaxValue ||
                    top is < int.MinValue or > int.MaxValue || (left == 0 && top == 0)) continue;
                var anchored = fixedOrigin with { X = (int)left, Y = (int)top };
                if (Fits(anchored) && touching.All(p => Touches(anchored, p))) return new(anchored.X, anchored.Y);
            }
        }
        throw new InvalidOperationException("This size would overlap or separate the displays. Adjust their layout in Windows display settings first.");
    }

    private UsbMonitorDisplay Snapshot()
    {
        var paths = Topology();
        var boards = paths.Where(p => p.UsbId != null).ToArray();
        if (boards.Length == 0) throw new InvalidOperationException("No active USB KVMpad display was found. Start USB monitor mode on the board and select Extend in Windows.");
        if (boards.Length > 1) throw new InvalidOperationException("More than one USB KVMpad display is active. Connect one KVMpad display to change its resolution.");
        var board = boards[0];
        if (paths.Length < 2 || paths.Any(p => p.Target != board.Target && (p.Source == board.Source || Overlaps(p, board))))
            throw new InvalidOperationException("Select Extend in Windows to give the USB KVMpad its own desktop.");
        SourceMode current = api.Current(board.Name);
        if (current.Size != board.Size) throw Changed();
        var offered = api.Offered(board.Name).Where(m => m == (current with { Size = m.Size }))
            .Select(m => m.Size).Distinct().OrderBy(s => s.Width).ThenBy(s => s.Height).ToArray();
        if (!paths.SequenceEqual(Topology()) || api.Current(board.Name) != current) throw Changed();
        return new(board, paths, current, offered);
    }

    private DisplayPath[] Topology() => api.ActivePaths().OrderBy(p => p.Target.High)
        .ThenBy(p => p.Target.Low).ThenBy(p => p.Target.Id).ToArray();
    private static InvalidOperationException Changed() =>
        new("The Windows display layout changed. Refresh the USB display list and try again.");
    private static long Right(DisplayPath p) => (long)p.X + p.Size.Width;
    private static long Bottom(DisplayPath p) => (long)p.Y + p.Size.Height;
    private static bool Overlaps(DisplayPath a, DisplayPath b) =>
        a.X < Right(b) && b.X < Right(a) && a.Y < Bottom(b) && b.Y < Bottom(a);
    private static bool Touches(DisplayPath a, DisplayPath b) =>
        ((Right(a) == b.X || Right(b) == a.X) && a.Y < Bottom(b) && b.Y < Bottom(a)) ||
        ((Bottom(a) == b.Y || Bottom(b) == a.Y) && a.X < Right(b) && b.X < Right(a));

    private static bool KeepsConnected(DisplayPath[] paths, DisplayPath board, DisplayPath resized)
    {
        // Do not cut an existing chain of adjacent desktops when shrinking its middle display.
        HashSet<DisplayEndpoint> Reachable(DisplayPath start)
        {
            var seen = new HashSet<DisplayEndpoint> {start.Source};
            var queue = new Queue<DisplayPath>();
            queue.Enqueue(start);
            while (queue.TryDequeue(out var p))
                foreach (var neighbor in paths)
                    if (Touches(p, neighbor) && seen.Add(neighbor.Source)) queue.Enqueue(neighbor);
            return seen;
        }
        return Reachable(board).IsSubsetOf(Reachable(resized));
    }

    // Walk upwards from the active monitor or its adapter. Composite HID siblings
    // share the serial-bearing parent, so only interface 00 can establish ownership.
    internal static string? BoardIdentity(IEnumerable<string> ancestry)
    {
        const string standalone = "VID_303A&PID_2987", composite = "VID_303A&PID_2986";
        bool displayFunction = false;
        foreach (var id in ancestry)
        {
            string[] parts = id.ToUpperInvariant().Split('\\');
            if (parts.Length != 3 || parts[0] != "USB") continue;
            if (parts[1] == composite + "&MI_00") { displayFunction = true; continue; }
            if ((parts[1] == standalone && !displayFunction || parts[1] == composite && displayFunction) &&
                parts[2].Length == 12 && parts[2].All(char.IsAsciiHexDigit))
                return string.Join('\\', parts);
            return null;
        }
        return null;
    }
}
