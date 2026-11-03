// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using KVMpad.Client.Core.Input;
using KVMpad.Client.Core.Protocol;

namespace KVMpad.Client.Core.TypeText;

/// <summary>Settings for one "type text" run (ADR-019 FR-704).</summary>
public sealed class TypeTextOptions
{
    /// <summary>The HOST layout the text is encoded for. The user picks it; a wrong pick gives wrong characters.</summary>
    public IHostLayoutTable Layout { get; init; } = HostLayouts.UsAnsi;

    /// <summary>Messages per second. Default 60, hard ceiling 100, both well under the device's 120 (FR-704).</summary>
    public int MessagesPerSecond { get; init; } = ProtocolLimits.DefaultTypeTextMessagesPerSecond;

    /// <summary>Maximum characters in one run. Longer needs an explicit confirmation in the UI (FR-704).</summary>
    public int MaxCharacters { get; init; } = ProtocolLimits.DefaultTypeTextMaxCharacters;
}

/// <summary>
/// The result of planning a run: the exact message sequence, and how much could not be encoded.
/// </summary>
/// <remarks>
/// Only counts are reported, never the characters themselves. The text being typed in is frequently a
/// password, and FR-705 forbids the client logging or storing it; a list of "the characters I could not type"
/// would be a fragment of that same text, so <see cref="UnmappedCharacterCount"/> and
/// <see cref="UnmappedDistinctCount"/> are all the UI gets. That still satisfies FR-702, which asks for the
/// count to be shown rather than the characters to be dropped silently.
/// </remarks>
public sealed class TypeTextPlan
{
    internal TypeTextPlan(
        IReadOnlyList<KeyboardKeyPayload> messages,
        int typedCharacterCount,
        int unmappedCharacterCount,
        int unmappedDistinctCount,
        string layoutId,
        bool truncated)
    {
        Messages = messages;
        TypedCharacterCount = typedCharacterCount;
        UnmappedCharacterCount = unmappedCharacterCount;
        UnmappedDistinctCount = unmappedDistinctCount;
        LayoutId = layoutId;
        Truncated = truncated;
    }

    /// <summary>The <c>keyboard.key</c> messages, in order. Nothing else is ever sent (ADR-019 FR-701).</summary>
    public IReadOnlyList<KeyboardKeyPayload> Messages { get; }

    public int TypedCharacterCount { get; }

    /// <summary>Characters this layout cannot produce. Reported, never approximated (FR-702).</summary>
    public int UnmappedCharacterCount { get; }

    /// <summary>How many distinct code points those were, so "one odd character 40 times" reads differently.</summary>
    public int UnmappedDistinctCount { get; }

    public string LayoutId { get; }

    /// <summary>True when the text was longer than <see cref="TypeTextOptions.MaxCharacters"/>.</summary>
    public bool Truncated { get; }

    public int MessageCount => Messages.Count;

    /// <summary>A secret-free summary for the UI.</summary>
    public string Describe()
    {
        string line = $"{TypedCharacterCount} character(s), {MessageCount} message(s), layout {LayoutId}";
        if (UnmappedCharacterCount > 0)
            line += $"; {UnmappedCharacterCount} character(s) ({UnmappedDistinctCount} distinct) not available on this layout";
        if (Truncated)
            line += "; truncated to the length limit";
        return line;
    }
}

/// <summary>
/// Turns text into a sequence of <c>keyboard.key</c> messages (ADR-019 option B).
/// </summary>
/// <remarks>
/// <para>
/// Each character becomes modifiers DOWN, key DOWN, key UP, modifiers UP, which is the order 02 §7.3 requires
/// and the only order that leaves no modifier held if the run is interrupted between characters. Modifiers are
/// pressed and released per character rather than held across a run of capitals: it costs more messages, but
/// it keeps the arithmetic in FR-704 exact and means any prefix of the sequence is a balanced, stuck-key-free
/// state.
/// </para>
/// <para>
/// No new firmware command is involved. The device sees ordinary key presses and cannot tell this apart from a
/// human typing, which is precisely why FR-309 is not at risk.
/// </para>
/// </remarks>
public static class TypeTextPlanner
{
    /// <summary>Plans a run. The text is used and discarded; nothing about its content is retained (FR-705).</summary>
    public static TypeTextPlan Plan(string text, TypeTextOptions? options = null)
    {
        if (text is null)
            throw new ArgumentNullException(nameof(text));
        options ??= new TypeTextOptions();
        if (options.MessagesPerSecond < 1 || options.MessagesPerSecond > ProtocolLimits.MaxTypeTextMessagesPerSecond)
            throw new ArgumentOutOfRangeException(
                nameof(options), options.MessagesPerSecond,
                $"The type-text rate must be between 1 and {ProtocolLimits.MaxTypeTextMessagesPerSecond} msg/s (ADR-019 FR-704).");

        IHostLayoutTable layout = options.Layout;
        bool truncated = text.Length > options.MaxCharacters;
        int limit = truncated ? options.MaxCharacters : text.Length;

        var messages = new List<KeyboardKeyPayload>(limit * 2);
        var unmapped = new HashSet<char>();
        int typed = 0;
        int unmappedCount = 0;

        for (int i = 0; i < limit; i++)
        {
            char character = text[i];

            // CRLF is one line break, so a pasted Windows text does not double-space the host.
            if (character == '\r' && i + 1 < limit && text[i + 1] == '\n')
                continue;

            // A character is one key press, or - on a layout that reaches it through a dead key - the dead key
            // and then the key it composes with. Either way every press is balanced on its own.
            if (!layout.TryMapSequence(character, out IReadOnlyList<KeyStroke> strokes) || strokes.Count == 0 ||
                !AllValid(strokes))
            {
                unmappedCount++;
                unmapped.Add(character);
                continue;
            }

            foreach (KeyStroke stroke in strokes)
            {
                byte[] modifierUsages = HidModifierUsages.Expand(stroke.Modifiers);

                foreach (byte modifier in modifierUsages)
                    messages.Add(new KeyboardKeyPayload(modifier, true));
                messages.Add(new KeyboardKeyPayload(stroke.Usage, true));
                messages.Add(new KeyboardKeyPayload(stroke.Usage, false));
                for (int m = modifierUsages.Length - 1; m >= 0; m--)
                    messages.Add(new KeyboardKeyPayload(modifierUsages[m], false));
            }

            typed++;
        }

        return new TypeTextPlan(messages, typed, unmappedCount, unmapped.Count, layout.Id, truncated);
    }

    /// <summary>A sequence is used only if every press in it is a key this protocol can carry (03 section 5.2).</summary>
    private static bool AllValid(IReadOnlyList<KeyStroke> strokes)
    {
        for (int i = 0; i < strokes.Count; i++)
        {
            if (!HidUsage.IsValidKey(strokes[i].Usage))
                return false;
        }
        return true;
    }

    /// <summary>
    /// When message number <paramref name="index"/> may be sent, measured from the start of the run. The
    /// schedule is what keeps the burst inside the budget; the session's rate limiter is the second line of
    /// defense, not the first (FR-704).
    /// </summary>
    public static TimeSpan ScheduleOffset(int index, int messagesPerSecond)
    {
        if (index < 0)
            throw new ArgumentOutOfRangeException(nameof(index));
        if (messagesPerSecond < 1)
            throw new ArgumentOutOfRangeException(nameof(messagesPerSecond));
        return TimeSpan.FromSeconds((double)index / messagesPerSecond);
    }

    /// <summary>How long the whole plan takes at the configured rate, for the progress UI.</summary>
    public static TimeSpan EstimatedDuration(TypeTextPlan plan, int messagesPerSecond)
        => ScheduleOffset(Math.Max(0, plan.MessageCount - 1), messagesPerSecond);
}
