// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using KVMpad.Client.Core.Input;
using KVMpad.Client.Core.Protocol;
using KVMpad.Client.Core.Session;
using KVMpad.Client.Core.TypeText;
using Xunit;

namespace KVMpad.Client.Core.Tests;

/// <summary>
/// "Type text" per ADR-019: a CLIENT-side function that sends only <c>keyboard.key</c>, from an explicit table
/// per host layout, within the rate budget, interruptible, and with unrepresentable characters reported rather
/// than guessed. This covers AT-EXT-10 at the unit level.
/// </summary>
public sealed class TypeTextTests
{
    /// <summary>
    /// The generated tables of ADR-024, read from layouts/kbd_layouts.json beside the test binary - the same
    /// file, and the same loading path, the application uses. The hand-written US and Polish tables are gone.
    /// </summary>
    private static readonly IHostLayoutTable Us = HostLayouts.ById("us");
    private static readonly IHostLayoutTable Polish = HostLayouts.ById("pl");

    [Theory]
    // Unshifted US ANSI.
    [InlineData('a', HidUsage.A, HidModifiers.None)]
    [InlineData('z', HidUsage.Z, HidModifiers.None)]
    [InlineData('1', HidUsage.D1, HidModifiers.None)]
    [InlineData('0', HidUsage.D0, HidModifiers.None)]
    [InlineData(' ', HidUsage.Space, HidModifiers.None)]
    [InlineData('\t', HidUsage.Tab, HidModifiers.None)]
    [InlineData('\n', HidUsage.Enter, HidModifiers.None)]
    [InlineData('-', HidUsage.Minus, HidModifiers.None)]
    [InlineData('=', HidUsage.Equal, HidModifiers.None)]
    [InlineData('[', HidUsage.LeftBracket, HidModifiers.None)]
    [InlineData(';', HidUsage.Semicolon, HidModifiers.None)]
    [InlineData('\'', HidUsage.Apostrophe, HidModifiers.None)]
    [InlineData('`', HidUsage.Grave, HidModifiers.None)]
    [InlineData(',', HidUsage.Comma, HidModifiers.None)]
    [InlineData('.', HidUsage.Period, HidModifiers.None)]
    [InlineData('/', HidUsage.Slash, HidModifiers.None)]
    [InlineData('\\', HidUsage.Backslash, HidModifiers.None)]
    // Shifted US ANSI.
    [InlineData('A', HidUsage.A, HidModifiers.LeftShift)]
    [InlineData('Z', HidUsage.Z, HidModifiers.LeftShift)]
    [InlineData('!', HidUsage.D1, HidModifiers.LeftShift)]
    [InlineData('@', HidUsage.D2, HidModifiers.LeftShift)]
    [InlineData('#', HidUsage.D3, HidModifiers.LeftShift)]
    [InlineData('$', HidUsage.D4, HidModifiers.LeftShift)]
    [InlineData('%', HidUsage.D5, HidModifiers.LeftShift)]
    [InlineData('^', HidUsage.D6, HidModifiers.LeftShift)]
    [InlineData('&', HidUsage.D7, HidModifiers.LeftShift)]
    [InlineData('*', HidUsage.D8, HidModifiers.LeftShift)]
    [InlineData('(', HidUsage.D9, HidModifiers.LeftShift)]
    [InlineData(')', HidUsage.D0, HidModifiers.LeftShift)]
    [InlineData('_', HidUsage.Minus, HidModifiers.LeftShift)]
    [InlineData('+', HidUsage.Equal, HidModifiers.LeftShift)]
    [InlineData('{', HidUsage.LeftBracket, HidModifiers.LeftShift)]
    [InlineData('}', HidUsage.RightBracket, HidModifiers.LeftShift)]
    [InlineData('|', HidUsage.Backslash, HidModifiers.LeftShift)]
    [InlineData(':', HidUsage.Semicolon, HidModifiers.LeftShift)]
    [InlineData('"', HidUsage.Apostrophe, HidModifiers.LeftShift)]
    [InlineData('<', HidUsage.Comma, HidModifiers.LeftShift)]
    [InlineData('>', HidUsage.Period, HidModifiers.LeftShift)]
    [InlineData('?', HidUsage.Slash, HidModifiers.LeftShift)]
    [InlineData('~', HidUsage.Grave, HidModifiers.LeftShift)]
    public void TheUsTableMapsEveryPrintableAsciiCharacter(char character, byte usage, HidModifiers modifiers)
    {
        Assert.True(Us.TryMap(character, out KeyStroke stroke));
        Assert.Equal(usage, stroke.Usage);
        Assert.Equal(modifiers, stroke.Modifiers);
    }

    [Fact]
    public void EveryPrintableAsciiCharacterIsInTheUsTable()
    {
        var missing = new List<char>();
        for (char c = ' '; c <= '~'; c++)
        {
            if (!Us.TryMap(c, out _))
                missing.Add(c);
        }
        Assert.Empty(missing);
    }

    [Theory]
    // AT-EXT-10/11: the Polish programmers layout reaches ąćęłńóśźż through AltGr, i.e. the right Alt usage.
    [InlineData('ą', HidUsage.A)] // a-ogonek
    [InlineData('ć', HidUsage.C)] // c-acute
    [InlineData('ę', HidUsage.E)] // e-ogonek
    [InlineData('ł', HidUsage.L)] // l-stroke
    [InlineData('ń', HidUsage.N)] // n-acute
    [InlineData('ó', HidUsage.O)] // o-acute
    [InlineData('ś', HidUsage.S)] // s-acute
    [InlineData('ź', HidUsage.X)] // z-acute sits on the X key
    [InlineData('ż', HidUsage.Z)] // z-dot-above
    public void ThePolishTableUsesAltGrForTheDiacritics(char character, byte usage)
    {
        Assert.True(Polish.TryMap(character, out KeyStroke lower));
        Assert.Equal(usage, lower.Usage);
        Assert.Equal(HidModifiers.RightAlt, lower.Modifiers);

        char upper = char.ToUpperInvariant(character);
        Assert.True(Polish.TryMap(upper, out KeyStroke capital));
        Assert.Equal(usage, capital.Usage);
        Assert.Equal(HidModifiers.RightAlt | HidModifiers.LeftShift, capital.Modifiers);
    }

    [Fact]
    public void ThePolishLayoutStillTypesPlainAscii()
    {
        Assert.True(Polish.TryMap('q', out KeyStroke stroke));
        Assert.Equal(HidUsage.Q, stroke.Usage);
        Assert.Equal(HidModifiers.None, stroke.Modifiers);
    }

    [Fact]
    public void AnUnshiftedCharacterCostsTwoMessagesDownThenUp()
    {
        TypeTextPlan plan = TypeTextPlanner.Plan("ab");

        Assert.Equal(2, plan.TypedCharacterCount);
        Assert.Equal(4, plan.MessageCount);
        Assert.Equal(HidUsage.A, plan.Messages[0].UsageId);
        Assert.True(plan.Messages[0].Down);
        Assert.Equal(HidUsage.A, plan.Messages[1].UsageId);
        Assert.False(plan.Messages[1].Down);
        Assert.Equal(HidUsage.B, plan.Messages[2].UsageId);
        Assert.True(plan.Messages[2].Down);
    }

    [Fact]
    public void AShiftedCharacterCostsFourMessagesInTheOrderTheSpecRequires()
    {
        // 02 §7.3: modifiers DOWN, key DOWN, key UP, modifiers UP. ADR-019 FR-704 counts this as 4 messages.
        TypeTextPlan plan = TypeTextPlanner.Plan("A");

        Assert.Equal(4, plan.MessageCount);
        Assert.Equal((HidUsage.LeftShift, true), (plan.Messages[0].UsageId, plan.Messages[0].Down));
        Assert.Equal((HidUsage.A, true), (plan.Messages[1].UsageId, plan.Messages[1].Down));
        Assert.Equal((HidUsage.A, false), (plan.Messages[2].UsageId, plan.Messages[2].Down));
        Assert.Equal((HidUsage.LeftShift, false), (plan.Messages[3].UsageId, plan.Messages[3].Down));
    }

    [Fact]
    public void AnAltGrCharacterOnThePolishLayoutCostsSixMessages()
    {
        TypeTextPlan plan = TypeTextPlanner.Plan(
            "Ą", new TypeTextOptions { Layout = Polish });

        // Two modifiers, so two DOWN plus key DOWN plus key UP plus two UP.
        Assert.Equal(6, plan.MessageCount);
        Assert.Equal(HidUsage.LeftShift, plan.Messages[0].UsageId);
        Assert.Equal(HidUsage.RightAlt, plan.Messages[1].UsageId);
        Assert.Equal(HidUsage.A, plan.Messages[2].UsageId);
        Assert.Equal(HidUsage.A, plan.Messages[3].UsageId);
        // Released in reverse, so Shift is last out just as it was first in.
        Assert.Equal(HidUsage.RightAlt, plan.Messages[4].UsageId);
        Assert.Equal(HidUsage.LeftShift, plan.Messages[5].UsageId);
        Assert.False(plan.Messages[5].Down);
    }

    [Fact]
    public void EveryPrefixOfAPlanIsBalancedAtACharacterBoundary()
    {
        // The property that makes an interruption safe: each character leaves nothing held (ADR-019 FR-703).
        TypeTextPlan plan = TypeTextPlanner.Plan("Hello, World! 42");

        var held = new HashSet<byte>();
        int boundaries = 0;
        foreach (KeyboardKeyPayload message in plan.Messages)
        {
            if (message.Down)
                Assert.True(held.Add(message.UsageId));
            else
                Assert.True(held.Remove(message.UsageId));
            if (held.Count == 0)
                boundaries++;
        }

        Assert.Empty(held);
        Assert.Equal(plan.TypedCharacterCount, boundaries);
    }

    [Fact]
    public void UnrepresentableCharactersAreCountedNotGuessedAt()
    {
        // FR-702: characters outside the table are skipped and their count shown.
        TypeTextPlan plan = TypeTextPlanner.Plan("abąćą—");

        Assert.Equal(2, plan.TypedCharacterCount);
        Assert.Equal(4, plan.UnmappedCharacterCount);
        Assert.Equal(3, plan.UnmappedDistinctCount);
        Assert.Contains("not available", plan.Describe());
    }

    [Fact]
    public void TheDescriptionNeverRepeatsTheText()
    {
        // FR-705: the client must not log or store the text being typed in.
        TypeTextPlan plan = TypeTextPlanner.Plan("correct horse battery staple —");
        string description = plan.Describe();

        Assert.DoesNotContain("horse", description);
        Assert.DoesNotContain("staple", description);
        Assert.DoesNotContain("—", description);
    }

    [Fact]
    public void CrLfBecomesOneEnter()
    {
        TypeTextPlan plan = TypeTextPlanner.Plan("a\r\nb");

        Assert.Equal(3, plan.TypedCharacterCount);
        Assert.Equal(6, plan.MessageCount);
        Assert.Equal(HidUsage.Enter, plan.Messages[2].UsageId);
        Assert.Equal(HidUsage.B, plan.Messages[4].UsageId);
    }

    [Fact]
    public void ALoneCarriageReturnIsStillEnter()
    {
        TypeTextPlan plan = TypeTextPlanner.Plan("a\rb");
        Assert.Equal(3, plan.TypedCharacterCount);
        Assert.Equal(HidUsage.Enter, plan.Messages[2].UsageId);
    }

    [Fact]
    public void TextLongerThanTheLimitIsTruncatedAndSaysSo()
    {
        var text = new string('x', ProtocolLimits.DefaultTypeTextMaxCharacters + 100);
        TypeTextPlan plan = TypeTextPlanner.Plan(text);

        Assert.True(plan.Truncated);
        Assert.Equal(ProtocolLimits.DefaultTypeTextMaxCharacters, plan.TypedCharacterCount);
        Assert.Contains("truncated", plan.Describe());
    }

    [Fact]
    public void ARateAboveTheAdrCeilingIsRefused()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            TypeTextPlanner.Plan("a", new TypeTextOptions { MessagesPerSecond = 101 }));
        _ = TypeTextPlanner.Plan("a", new TypeTextOptions { MessagesPerSecond = 100 });
    }

    [Fact]
    public void TheScheduleSpreadsMessagesAtTheConfiguredRate()
    {
        Assert.Equal(TimeSpan.Zero, TypeTextPlanner.ScheduleOffset(0, 60));
        Assert.Equal(TimeSpan.FromSeconds(1), TypeTextPlanner.ScheduleOffset(60, 60));
        Assert.Equal(TimeSpan.FromSeconds(2), TypeTextPlanner.ScheduleOffset(120, 60));

        // 60 msg/s with 2 messages per bare character is about 30 characters a second (ADR-019 FR-704).
        TypeTextPlan plan = TypeTextPlanner.Plan(new string('a', 30));
        Assert.Equal(60, plan.MessageCount);
        Assert.True(TypeTextPlanner.EstimatedDuration(plan, 60) <= TimeSpan.FromSeconds(1));
    }

    [Fact]
    public void LayoutLookupFallsBackToUsRatherThanFailing()
    {
        Assert.Equal("us", HostLayouts.ById(null).Id);
        Assert.Equal("us", HostLayouts.ById("klingon").Id);
        Assert.Equal("pl", HostLayouts.ById("pl").Id);
        Assert.Equal("pl(dvorak)", HostLayouts.ById("pl(dvorak)").Id); // a variant, which only the file has
        Assert.Equal("pl", HostLayouts.ById("PL").Id);
    }

    [Fact]
    public async Task ARunSendsEveryMessageAndTheSessionAcceptsThem()
    {
        (SessionLogic logic, _) = Live();
        var run = new TypeTextRun(logic, _ => Task.CompletedTask);
        TypeTextPlan plan = TypeTextPlanner.Plan("hi");

        TypeTextResult result = await run.RunAsync(plan, 100, CancellationToken.None);

        Assert.Equal(TypeTextOutcome.Completed, result.Outcome);
        Assert.Equal(plan.MessageCount, result.MessagesSent);
        Assert.Empty(logic.Ledger.PressedKeys);
    }

    [Fact]
    public async Task CancellingMidRunReleasesWhatWasHeldAndReportsCancelled()
    {
        // AT-EXT-12: interrupt half way through, no stuck keys.
        (SessionLogic logic, _) = Live();
        var sentFrames = new List<byte[]>();
        var run = new TypeTextRun(logic, frames => { sentFrames.AddRange(frames); return Task.CompletedTask; });

        // A plan whose first message is a modifier DOWN, so cancelling right after it leaves Shift held.
        TypeTextPlan plan = TypeTextPlanner.Plan("ABC");
        using var cts = new CancellationTokenSource();
        run.Progress += (sent, _) =>
        {
            if (sent == 1)
                cts.Cancel();
        };

        TypeTextResult result = await run.RunAsync(plan, 100, cts.Token);

        Assert.Equal(TypeTextOutcome.Cancelled, result.Outcome);
        Assert.Empty(logic.Ledger.PressedKeys);
        // The modifier that was down got an explicit UP, and it reached the sender.
        Assert.Single(sentFrames);
        ServerMessage released = ProtocolCodec.Decode(sentFrames[0]);
        Assert.Equal(HidUsage.LeftShift, (byte)released.PayloadInt64("usage_id")!.Value);
        Assert.False(released.PayloadBool("down"));
    }

    [Fact]
    public async Task LosingTheLeaseMidRunStopsItWithoutSendingMore()
    {
        (SessionLogic logic, _) = Live();
        var run = new TypeTextRun(logic, _ => Task.CompletedTask);
        TypeTextPlan plan = TypeTextPlanner.Plan("ABCDEF");

        run.Progress += (sent, _) =>
        {
            if (sent == 2)
            {
                logic.OnMessage(ProtocolCodec.Decode(
                    """{"v":1,"type":"control.revoked","seq":9,"session_id":"c-1","payload":{"reason":"local_override"}}"""));
            }
        };

        TypeTextResult result = await run.RunAsync(plan, 100, CancellationToken.None);

        Assert.Equal(TypeTextOutcome.SessionLost, result.Outcome);
        Assert.True(result.MessagesSent < plan.MessageCount);
        Assert.Empty(logic.Ledger.PressedKeys);
    }

    [Fact]
    public async Task ARunWithoutALeaseSendsNothing()
    {
        var clock = new FakeClock();
        var logic = new SessionLogic(clock, new SessionOptions(), new CaptureLedger());
        var run = new TypeTextRun(logic, _ => Task.CompletedTask);

        TypeTextResult result = await run.RunAsync(TypeTextPlanner.Plan("abc"), 100, CancellationToken.None);

        Assert.Equal(TypeTextOutcome.SessionLost, result.Outcome);
        Assert.Equal(0, result.MessagesSent);
    }

    private static (SessionLogic Logic, FakeClock Clock) Live()
    {
        var clock = new FakeClock();
        var logic = new SessionLogic(clock, new SessionOptions(), new CaptureLedger());
        logic.OnTransportOpened();
        logic.OnMessage(ProtocolCodec.Decode(
            """{"v":1,"type":"hello.ack","seq":1,"session_id":"c-1","payload":{}}"""));
        logic.OnMessage(ProtocolCodec.Decode(
            """{"v":1,"type":"control.granted","seq":2,"session_id":"c-1","lease_epoch":3,"payload":{}}"""));
        return (logic, clock);
    }
}
