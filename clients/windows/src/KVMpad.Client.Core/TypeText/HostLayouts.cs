// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using KVMpad.Client.Core.Input;

namespace KVMpad.Client.Core.TypeText;

/// <summary>One key press that produces one character: a usage plus the modifiers held around it.</summary>
public readonly struct KeyStroke : IEquatable<KeyStroke>
{
    public KeyStroke(byte usage, HidModifiers modifiers = HidModifiers.None)
    {
        Usage = usage;
        Modifiers = modifiers;
    }

    public byte Usage { get; }
    public HidModifiers Modifiers { get; }

    /// <summary>Messages this stroke costs: 2 bare, 4 with one modifier, 6 with two (ADR-019 FR-704).</summary>
    public int MessageCount => 2 + (2 * System.Numerics.BitOperations.PopCount((uint)(byte)Modifiers));

    public bool Equals(KeyStroke other) => Usage == other.Usage && Modifiers == other.Modifiers;

    public override bool Equals(object? obj) => obj is KeyStroke other && Equals(other);

    public override int GetHashCode() => HashCode.Combine(Usage, Modifiers);

    public override string ToString() => $"{Modifiers}+0x{Usage:X2}";
}

/// <summary>
/// A character → keystroke table for one HOST keyboard layout.
/// </summary>
/// <remarks>
/// HID carries no Unicode: the usage says which physical key, and the layout loaded on the HOST decides which
/// character that produces (FR-202, FR-743). The client therefore needs a table per HOST layout, the device
/// tells it which one is in force, and a mismatch gives wrong characters — a property of HID, not a bug
/// (ADR-019). The tables are the generated ones of ADR-024, read from <c>layouts/kbd_layouts.json</c>;
/// <see cref="Id"/> is the xkeyboard-config <c>layout(variant)</c> form the device uses (FR-741/745).
/// </remarks>
public interface IHostLayoutTable
{
    /// <summary>xkeyboard-config style id, e.g. <c>us</c> or <c>pl(dvorak)</c>.</summary>
    string Id { get; }

    /// <summary>English name for the layout picker.</summary>
    string DisplayName { get; }

    /// <summary>Resolves a character reachable with one key press, or returns false.</summary>
    bool TryMap(char character, out KeyStroke stroke);

    /// <summary>
    /// Resolves a character into the presses that produce it: one for an ordinary key, two when the layout
    /// reaches it through a dead key (acute, grave, diaeresis…). Returns false when it cannot be produced.
    /// </summary>
    bool TryMapSequence(char character, out IReadOnlyList<KeyStroke> strokes);
}

/// <summary>Handling of the characters every layout treats the same way.</summary>
internal static class LayoutTables
{
    /// <summary>
    /// Whitespace and the control characters a text box can contain. A bare carriage return is Enter too; a
    /// CRLF pair is collapsed by the planner so one line break is one Enter. These keys carry no character in
    /// the generated tables, because they are the same on every layout.
    /// </summary>
    internal static bool TryMapWhitespace(char character, out KeyStroke stroke)
    {
        switch (character)
        {
            case ' ':
                stroke = new KeyStroke(HidUsage.Space);
                return true;
            case '\t':
                stroke = new KeyStroke(HidUsage.Tab);
                return true;
            case '\n':
            case '\r':
                stroke = new KeyStroke(HidUsage.Enter);
                return true;
            default:
                stroke = default;
                return false;
        }
    }
}

/// <summary>
/// The generated layouts available to this client (ADR-024, FR-741). The catalog is read once from the file
/// beside the binary; until then, and when that file is missing, only whitespace can be typed.
/// </summary>
public static class HostLayouts
{
    private static HostLayoutCatalog s_catalog = HostLayoutCatalog.Empty;
    private static readonly object s_gate = new();

    /// <summary>The device's own default layout, and this client's fallback (FR-741).</summary>
    public const string DefaultId = "us";

    /// <summary>
    /// The catalog in use. Loaded from <c>layouts/kbd_layouts.json</c> beside the binary on first use; a
    /// missing file leaves the empty catalog, whose <see cref="HostLayoutCatalog.LoadError"/> says so.
    /// </summary>
    public static HostLayoutCatalog Catalog
    {
        get
        {
            lock (s_gate)
            {
                if (s_catalog.SourcePath is null && s_catalog.LoadError is not null)
                    s_catalog = HostLayoutCatalog.LoadDefault();
                return s_catalog;
            }
        }
    }

    /// <summary>Uses a catalog loaded elsewhere (the tests, or an application that ships the file apart).</summary>
    public static void Use(HostLayoutCatalog catalog)
    {
        ArgumentNullException.ThrowIfNull(catalog);
        lock (s_gate)
            s_catalog = catalog;
    }

    /// <summary>What the catalog holds, for a picker. Empty when the generated file is not there.</summary>
    public static IReadOnlyList<HostLayoutInfo> All => Catalog.Layouts;

    /// <summary>The US ANSI table, or the empty table when the generated file is missing.</summary>
    public static IHostLayoutTable UsAnsi => ById(DefaultId);

    /// <summary>
    /// Finds a layout by id, falling back to US ANSI, which is the device's own default (FR-741), and to an
    /// empty table when even that is unavailable — the client never invents a mapping.
    /// </summary>
    public static IHostLayoutTable ById(string? id)
    {
        HostLayoutCatalog catalog = Catalog;
        return catalog.Get(id) ?? catalog.Get(DefaultId) ?? EmptyLayout.Instance;
    }
}

/// <summary>
/// What is left when the generated file is unavailable: the keys that are the same everywhere, and nothing
/// else. Typing then reports every other character as unavailable instead of sending a guess (FR-702).
/// </summary>
internal sealed class EmptyLayout : IHostLayoutTable
{
    internal static readonly EmptyLayout Instance = new();

    public string Id => "";

    public string DisplayName => "No layout table loaded";

    public bool TryMap(char character, out KeyStroke stroke) => LayoutTables.TryMapWhitespace(character, out stroke);

    public bool TryMapSequence(char character, out IReadOnlyList<KeyStroke> strokes)
    {
        if (LayoutTables.TryMapWhitespace(character, out KeyStroke one))
        {
            strokes = [one];
            return true;
        }
        strokes = [];
        return false;
    }
}
