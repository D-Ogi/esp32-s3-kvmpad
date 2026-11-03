// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using KVMpad.Client.Core.Input;

namespace KVMpad.Client.Core.TypeText;

/// <summary>What the picker needs to know about one generated layout, without reading its tables.</summary>
public readonly record struct HostLayoutInfo(string Id, string Name, string Geometry, bool AltGr, bool BestEffort);

/// <summary>
/// The generated host layouts of ADR-024, read from <c>layouts/kbd_layouts.json</c> — the same file the device
/// compiles into its firmware, produced by <c>tools/kbd_layouts/gen_layouts.py</c> from xkeyboard-config.
/// </summary>
/// <remarks>
/// <para>
/// The file is about 4.5 MB and holds 581 layouts, so it is indexed once (id, name, geometry and the two flags
/// per entry, with the byte range of the entry) and a layout's character table is built only when that layout
/// is actually selected. Nothing is deserialized into objects in bulk and no NuGet dependency is involved:
/// <c>System.Text.Json</c>'s reader and <see cref="JsonDocument"/> are enough (ADR-028).
/// </para>
/// <para>
/// A missing or malformed file leaves an <see cref="Empty"/> catalog whose layouts produce nothing but
/// whitespace. That is deliberate: the client would rather type nothing and say so than guess at a layout and
/// send the HOST characters nobody asked for (FR-702).
/// </para>
/// </remarks>
public sealed class HostLayoutCatalog
{
    /// <summary>Where the file sits beside the application and the tests.</summary>
    public const string DefaultRelativePath = "layouts/kbd_layouts.json";

    private readonly byte[] _json;
    private readonly List<HostLayoutInfo> _infos;
    private readonly List<(int Start, int Length)> _ranges;
    private readonly Dictionary<string, IHostLayoutTable> _tables =
        new(StringComparer.OrdinalIgnoreCase);
    private readonly object _gate = new();

    private HostLayoutCatalog(byte[] json, List<HostLayoutInfo> infos, List<(int, int)> ranges, string? sourcePath,
                              string? loadError)
    {
        _json = json;
        _infos = infos;
        _ranges = ranges;
        SourcePath = sourcePath;
        LoadError = loadError;
    }

    /// <summary>A catalog with no layouts, used when the file is absent or unreadable.</summary>
    public static HostLayoutCatalog Empty { get; } =
        new([], [], [], null, "The generated layout file was not loaded.");

    /// <summary>The file this catalog was read from, or null for <see cref="Empty"/>.</summary>
    public string? SourcePath { get; }

    /// <summary>Why the catalog is empty, in one sentence for the UI, or null when it loaded. Never the data.</summary>
    public string? LoadError { get; }

    public bool IsAvailable => _infos.Count > 0;

    public IReadOnlyList<HostLayoutInfo> Layouts => _infos;

    /// <summary>Reads the file beside the running binary; an absent or broken file gives <see cref="Empty"/>.</summary>
    public static HostLayoutCatalog LoadDefault()
        => TryLoad(Path.Combine(AppContext.BaseDirectory, DefaultRelativePath), out HostLayoutCatalog catalog)
               ? catalog
               : catalog;

    /// <summary>
    /// Reads and indexes one generated file. Returns false, with an <see cref="Empty"/>-like catalog carrying
    /// <see cref="LoadError"/>, when the file is missing or is not the file this client expects.
    /// </summary>
    public static bool TryLoad(string path, out HostLayoutCatalog catalog)
    {
        ArgumentNullException.ThrowIfNull(path);
        byte[] json;
        try
        {
            json = File.ReadAllBytes(path);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or NotSupportedException
                                      or ArgumentException)
        {
            catalog = new HostLayoutCatalog([], [], [], path, "The generated layout file could not be read.");
            return false;
        }

        var infos = new List<HostLayoutInfo>(600);
        var ranges = new List<(int, int)>(600);
        try
        {
            Index(json, infos, ranges);
        }
        catch (JsonException)
        {
            // The message of a JsonException carries a fragment of the document, so it is not propagated.
            catalog = new HostLayoutCatalog([], [], [], path, "The generated layout file is not valid JSON.");
            return false;
        }

        if (infos.Count == 0)
        {
            catalog = new HostLayoutCatalog([], [], [], path, "The generated layout file holds no layouts.");
            return false;
        }

        catalog = new HostLayoutCatalog(json, infos, ranges, path, null);
        return true;
    }

    /// <summary>The table for one id (<c>us</c>, <c>pl(dvorak)</c>), built on first use, or null.</summary>
    public IHostLayoutTable? Get(string? id)
    {
        if (string.IsNullOrEmpty(id))
            return null;
        lock (_gate)
        {
            if (_tables.TryGetValue(id, out IHostLayoutTable? cached))
                return cached;
            for (int i = 0; i < _infos.Count; i++)
            {
                if (!string.Equals(_infos[i].Id, id, StringComparison.OrdinalIgnoreCase))
                    continue;
                IHostLayoutTable table = Build(_infos[i], _json.AsSpan(_ranges[i].Start, _ranges[i].Length));
                _tables[table.Id] = table;
                return table;
            }
            return null;
        }
    }

    /// <summary>One pass over the document: the five scalar fields of each layout and where that layout sits.</summary>
    private static void Index(ReadOnlySpan<byte> json, List<HostLayoutInfo> infos, List<(int, int)> ranges)
    {
        var reader = new Utf8JsonReader(json, isFinalBlock: true, state: default);
        // Walk to the "layouts" array without materializing provenance or mods.
        if (!reader.Read() || reader.TokenType != JsonTokenType.StartObject)
            throw new JsonException();
        while (reader.Read() && reader.TokenType == JsonTokenType.PropertyName)
        {
            bool isLayouts = reader.ValueTextEquals("layouts"u8);
            if (!reader.Read())
                throw new JsonException();
            if (!isLayouts)
            {
                reader.Skip();
                continue;
            }
            if (reader.TokenType != JsonTokenType.StartArray)
                throw new JsonException();
            while (reader.Read() && reader.TokenType == JsonTokenType.StartObject)
            {
                int start = (int)reader.TokenStartIndex;
                reader.Skip();
                int end = (int)reader.BytesConsumed;
                ReadOnlySpan<byte> slice = json[start..end];
                using var entry = JsonDocument.Parse(slice.ToArray());
                JsonElement root = entry.RootElement;
                string entryId = root.TryGetProperty("id", out JsonElement idEl) ? idEl.GetString() ?? "" : "";
                if (entryId.Length == 0)
                    continue;
                infos.Add(new HostLayoutInfo(
                    entryId,
                    root.TryGetProperty("name", out JsonElement nameEl) ? nameEl.GetString() ?? entryId : entryId,
                    root.TryGetProperty("geometry", out JsonElement geoEl) ? geoEl.GetString() ?? "" : "",
                    root.TryGetProperty("altgr", out JsonElement altEl) && altEl.ValueKind == JsonValueKind.True,
                    root.TryGetProperty("best_effort", out JsonElement beEl) && beEl.ValueKind == JsonValueKind.True));
                ranges.Add((start, end - start));
            }
            return;
        }
        throw new JsonException();
    }

    /// <summary>Builds one layout's character table from its slice of the document.</summary>
    private static IHostLayoutTable Build(HostLayoutInfo info, ReadOnlySpan<byte> slice)
    {
        var table = new Dictionary<char, KeyStroke[]>(256);
        using var entry = JsonDocument.Parse(slice.ToArray());
        if (entry.RootElement.TryGetProperty("chars", out JsonElement chars) &&
            chars.ValueKind == JsonValueKind.Object)
        {
            foreach (JsonProperty ch in chars.EnumerateObject())
            {
                // One UTF-16 char per entry: a character outside the BMP cannot be a key of this table, and the
                // generator does not emit one. Such an entry is skipped rather than approximated.
                if (ch.Name.Length != 1 || ch.Value.ValueKind != JsonValueKind.Array)
                    continue;
                if (!TryStrokes(ch.Value, out KeyStroke[] strokes))
                    continue;
                table[ch.Name[0]] = strokes;
            }
        }
        return new GeneratedLayoutTable(info, table);
    }

    /// <summary>One <c>[[usage, level], ...]</c> array: a direct key, or a dead key and the key it composes with.</summary>
    private static bool TryStrokes(JsonElement value, out KeyStroke[] strokes)
    {
        int n = value.GetArrayLength();
        if (n is < 1 or > 4)
        {
            strokes = [];
            return false;
        }
        var list = new KeyStroke[n];
        int i = 0;
        foreach (JsonElement pair in value.EnumerateArray())
        {
            if (pair.ValueKind != JsonValueKind.Array || pair.GetArrayLength() != 2 ||
                !pair[0].TryGetInt32(out int usage) || !pair[1].TryGetInt32(out int level) ||
                usage is < 0 or > 0xFF || level is < 0 or > 3)
            {
                strokes = [];
                return false;
            }
            var stroke = new KeyStroke((byte)usage, ModifiersOfLevel(level));
            if (!HidUsage.IsValidKey(stroke.Usage))
            {
                strokes = [];
                return false;
            }
            list[i++] = stroke;
        }
        strokes = list;
        return true;
    }

    /// <summary>
    /// The level the generator writes is the modifier combination that reaches it: bit 0 Shift, bit 1 AltGr
    /// (the <c>mods</c> object of the file). AltGr is the right Alt usage, as 03 section 5.2 lists it.
    /// </summary>
    internal static HidModifiers ModifiersOfLevel(int level)
    {
        HidModifiers mods = HidModifiers.None;
        if ((level & 1) != 0)
            mods |= HidModifiers.LeftShift;
        if ((level & 2) != 0)
            mods |= HidModifiers.RightAlt;
        return mods;
    }
}

/// <summary>One generated layout: the characters it can produce and the strokes that produce them.</summary>
internal sealed class GeneratedLayoutTable : IHostLayoutTable
{
    private readonly Dictionary<char, KeyStroke[]> _table;

    internal GeneratedLayoutTable(HostLayoutInfo info, Dictionary<char, KeyStroke[]> table)
    {
        Info = info;
        _table = table;
    }

    public HostLayoutInfo Info { get; }

    public string Id => Info.Id;

    public string DisplayName => Info.Name;

    public bool TryMap(char character, out KeyStroke stroke)
    {
        if (LayoutTables.TryMapWhitespace(character, out stroke))
            return true;
        if (_table.TryGetValue(character, out KeyStroke[]? strokes) && strokes.Length == 1)
        {
            stroke = strokes[0];
            return true;
        }
        stroke = default;
        return false;
    }

    public bool TryMapSequence(char character, out IReadOnlyList<KeyStroke> strokes)
    {
        if (LayoutTables.TryMapWhitespace(character, out KeyStroke one))
        {
            strokes = [one];
            return true;
        }
        if (_table.TryGetValue(character, out KeyStroke[]? found))
        {
            strokes = found;
            return true;
        }
        strokes = [];
        return false;
    }
}
