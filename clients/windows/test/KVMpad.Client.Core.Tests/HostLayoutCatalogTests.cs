// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.IO;
using KVMpad.Client.Core.Input;
using KVMpad.Client.Core.TypeText;
using Xunit;

namespace KVMpad.Client.Core.Tests;

/// <summary>
/// The generated host layouts of ADR-024 (TASK-071): the client reads the same <c>kbd_layouts.json</c> the
/// device compiles into its firmware, instead of the two tables it used to carry by hand. Nothing here edits
/// the generated file, and no test prints a character it typed.
/// </summary>
public sealed class HostLayoutCatalogTests
{
    private static string FilePath =>
        Path.Combine(AppContext.BaseDirectory, "layouts", "kbd_layouts.json");

    private static HostLayoutCatalog Load()
    {
        Assert.True(HostLayoutCatalog.TryLoad(FilePath, out HostLayoutCatalog catalog), catalog.LoadError);
        return catalog;
    }

    [Fact]
    public void TheGeneratedFileShipsBesideTheBinaryAndHoldsEveryLayout()
    {
        HostLayoutCatalog catalog = Load();

        Assert.True(catalog.IsAvailable);
        Assert.Null(catalog.LoadError);
        // The device's own tables hold 581 layouts; the client reads the same file, so it holds the same.
        Assert.Equal(581, catalog.Layouts.Count);
        Assert.Contains(catalog.Layouts, l => l.Id == "us" && l.Geometry == "ANSI");
        Assert.Contains(catalog.Layouts, l => l.Id == "pl");
        Assert.Contains(catalog.Layouts, l => l.Id == "de(neo)"); // a variant, which the old client had no way to reach
        // Every entry is usable as a picker row: an id and a name, never an empty one.
        foreach (HostLayoutInfo info in catalog.Layouts)
        {
            Assert.False(string.IsNullOrWhiteSpace(info.Id));
            Assert.False(string.IsNullOrWhiteSpace(info.Name));
        }
    }

    [Fact]
    public void ALayoutIsBuiltOnceAndOnlyWhenItIsAskedFor()
    {
        HostLayoutCatalog catalog = Load();

        IHostLayoutTable? first = catalog.Get("de");
        IHostLayoutTable? again = catalog.Get("DE"); // the id is matched without regard to case
        Assert.NotNull(first);
        Assert.Same(first, again);
        Assert.Equal("de", first!.Id);
        Assert.Null(catalog.Get("klingon"));
        Assert.Null(catalog.Get(null));
    }

    [Fact]
    public void TheGermanLayoutIsTheOneTheDeviceDraws()
    {
        IHostLayoutTable de = Load().Get("de")!;

        // QWERTZ: the two swapped keys say the table is the real one and not a US table under another name.
        Assert.True(de.TryMap('z', out KeyStroke z));
        Assert.Equal(HidUsage.Y, z.Usage);
        Assert.True(de.TryMap('y', out KeyStroke y));
        Assert.Equal(HidUsage.Z, y.Usage);
        Assert.True(de.TryMap('ß', out KeyStroke sz));
        Assert.Equal(HidModifiers.None, sz.Modifiers);
        // AltGr reaches the third level, exactly as the device's own legends show it.
        Assert.True(de.TryMap('@', out KeyStroke at));
        Assert.Equal(HidModifiers.RightAlt, at.Modifiers);
    }

    [Fact]
    public void ADeadKeyCharacterIsTwoPressesAndIsTypedInThatOrder()
    {
        // The German layout reaches Â through the dead circumflex and then Shift+A: the old single-stroke
        // table could not express this at all, and such a character was simply reported as unavailable.
        IHostLayoutTable de = Load().Get("de")!;
        Assert.True(de.TryMapSequence('Â', out IReadOnlyList<KeyStroke> strokes));
        Assert.Equal(2, strokes.Count);
        Assert.NotEqual((byte)0, strokes[0].Usage);
        Assert.Equal(HidUsage.A, strokes[1].Usage);
        Assert.Equal(HidModifiers.LeftShift, strokes[1].Modifiers);
        Assert.False(de.TryMap('Â', out _)); // not reachable with one press, so TryMap says no

        TypeTextPlan plan = TypeTextPlanner.Plan("Â", new TypeTextOptions { Layout = de });
        Assert.Equal(1, plan.TypedCharacterCount);
        Assert.Equal(0, plan.UnmappedCharacterCount);
        // Press and release of the dead key, then of the letter, each with its own modifiers around it.
        Assert.Equal(strokes[0].MessageCount + strokes[1].MessageCount, plan.MessageCount);
        Assert.Equal(strokes[0].Usage, plan.Messages[strokes[0].Modifiers == HidModifiers.None ? 0 : 1].UsageId);
        // Balanced at the end: nothing is left down.
        var held = new Dictionary<byte, int>();
        foreach (Protocol.KeyboardKeyPayload m in plan.Messages)
            held[m.UsageId] = held.TryGetValue(m.UsageId, out int c) ? c + (m.Down ? 1 : -1) : (m.Down ? 1 : -1);
        foreach (KeyValuePair<byte, int> pair in held)
            Assert.Equal(0, pair.Value);
    }

    [Fact]
    public void AMissingFileLeavesALayoutThatTypesNothingRatherThanAGuess()
    {
        string absent = Path.Combine(AppContext.BaseDirectory, "layouts", "no-such-file.json");
        Assert.False(HostLayoutCatalog.TryLoad(absent, out HostLayoutCatalog catalog));
        Assert.False(catalog.IsAvailable);
        Assert.NotNull(catalog.LoadError);
        Assert.Empty(catalog.Layouts);
        Assert.Null(catalog.Get("us"));

        // The client then falls back to a table that can only type the keys every layout shares. The fallback
        // is used directly here: HostLayouts is process-wide state and a test does not reach into it.
        IHostLayoutTable fallback = EmptyLayout.Instance;
        Assert.True(fallback.TryMap(' ', out KeyStroke space));
        Assert.Equal(HidUsage.Space, space.Usage);
        Assert.False(fallback.TryMap('a', out _));

        TypeTextPlan plan = TypeTextPlanner.Plan("ab c", new TypeTextOptions { Layout = fallback });
        Assert.Equal(1, plan.TypedCharacterCount); // the space, and nothing else
        Assert.Equal(3, plan.UnmappedCharacterCount);
        Assert.DoesNotContain("ab c", plan.Describe()); // the text is never in a message (FR-705)
    }

    [Fact]
    public void AFileThatIsNotTheGeneratedOneIsRefusedWithoutQuotingIt()
    {
        string path = Path.Combine(Path.GetTempPath(), $"kvmpad-layouts-{Guid.NewGuid():N}.json");
        File.WriteAllText(path, "{\"layouts\":\"not an array of layouts\"}");
        try
        {
            Assert.False(HostLayoutCatalog.TryLoad(path, out HostLayoutCatalog catalog));
            Assert.False(catalog.IsAvailable);
            Assert.NotNull(catalog.LoadError);
            Assert.DoesNotContain("not an array", catalog.LoadError!);
        }
        finally
        {
            File.Delete(path);
        }
    }

    [Fact]
    public void TheLevelOfAKeyIsTheModifierCombinationThatReachesIt()
    {
        Assert.Equal(HidModifiers.None, HostLayoutCatalog.ModifiersOfLevel(0));
        Assert.Equal(HidModifiers.LeftShift, HostLayoutCatalog.ModifiersOfLevel(1));
        Assert.Equal(HidModifiers.RightAlt, HostLayoutCatalog.ModifiersOfLevel(2));
        Assert.Equal(HidModifiers.RightAlt | HidModifiers.LeftShift, HostLayoutCatalog.ModifiersOfLevel(3));
    }
}
