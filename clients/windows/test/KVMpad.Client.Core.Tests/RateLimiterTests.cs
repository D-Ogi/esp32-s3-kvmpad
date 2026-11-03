// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using KVMpad.Client.Core.Input;
using KVMpad.Client.Core.Protocol;
using KVMpad.Client.Core.Session;
using Xunit;

namespace KVMpad.Client.Core.Tests;

/// <summary>
/// The input budget of 03 §6.5: at most 120 msg/s at the device, which the client stays under; mouse moves
/// coalesced; deltas split rather than clipped; nothing else dropped.
/// </summary>
public sealed class RateLimiterTests
{
    [Fact]
    public void TheClientCeilingStaysBelowTheProtocolLimit()
    {
        Assert.True(ProtocolLimits.ClientInputMessagesPerSecond < ProtocolLimits.ServerInputMessagesPerSecond);
        Assert.True(ProtocolLimits.DefaultTypeTextMessagesPerSecond <= 60);
        Assert.True(ProtocolLimits.MaxTypeTextMessagesPerSecond <= 100);
    }

    [Fact]
    public void ABudgetAboveTheProtocolLimitIsRefused()
    {
        var clock = new FakeClock();
        Assert.Throws<ArgumentOutOfRangeException>(() => new InputRateLimiter(clock, 121));
        _ = new InputRateLimiter(clock, ProtocolLimits.ServerInputMessagesPerSecond);
    }

    [Fact]
    public void NoMoreThanTheBudgetLeavesInOneSecond()
    {
        var clock = new FakeClock();
        var limiter = new InputRateLimiter(clock, 100);

        // 400 distinct key edges, far more than a second's worth.
        for (int i = 0; i < 200; i++)
        {
            limiter.Enqueue(new KeyboardKeyPayload(HidUsage.A, true));
            limiter.Enqueue(new KeyboardKeyPayload(HidUsage.A, false));
        }

        int firstSecond = limiter.Drain().Count;
        Assert.Equal(100, firstSecond);

        // Still inside the same second: nothing more may go out.
        clock.AdvanceMilliseconds(999);
        Assert.Empty(limiter.Drain());

        // The window has rolled; the next hundred may go.
        clock.AdvanceMilliseconds(2);
        Assert.Equal(100, limiter.Drain().Count);
    }

    [Fact]
    public void NothingIsDroppedWhileWaitingForBudget()
    {
        var clock = new FakeClock();
        var limiter = new InputRateLimiter(clock, 10);

        for (int i = 0; i < 25; i++)
            Assert.True(limiter.Enqueue(new MouseButtonPayload(MouseButtons.Left, i % 2 == 0)));

        var released = new List<IInputPayload>();
        for (int second = 0; second < 3; second++)
        {
            released.AddRange(limiter.Drain());
            clock.Advance(TimeSpan.FromSeconds(1));
        }

        Assert.Equal(25, released.Count);
        Assert.Equal(0, limiter.PendingCount);
    }

    [Fact]
    public void AdjacentMovesCoalesceIntoOneMessage()
    {
        var clock = new FakeClock();
        var limiter = new InputRateLimiter(clock, 100);

        for (int i = 0; i < 50; i++)
            limiter.EnqueueMove(2, -1);

        IReadOnlyList<IInputPayload> released = limiter.Drain();
        Assert.Single(released);
        var move = Assert.IsType<MouseMovePayload>(released[0]);
        Assert.Equal(100, move.Dx);
        Assert.Equal(-50, move.Dy);
        Assert.Equal(49, limiter.CoalescedMoveCount);
    }

    [Fact]
    public void AClickKeepsItsPlaceBetweenTheMotionAroundIt()
    {
        // A drag depends on the button edge landing between the right two batches of motion. Coalescing may
        // only merge moves with moves, never across a button.
        var clock = new FakeClock();
        var limiter = new InputRateLimiter(clock, 100);

        limiter.EnqueueMove(5, 0);
        limiter.EnqueueMove(5, 0);
        limiter.Enqueue(new MouseButtonPayload(MouseButtons.Left, true));
        limiter.EnqueueMove(7, 0);
        limiter.Enqueue(new MouseButtonPayload(MouseButtons.Left, false));

        IReadOnlyList<IInputPayload> released = limiter.Drain();
        Assert.Equal(4, released.Count);
        Assert.Equal(10, Assert.IsType<MouseMovePayload>(released[0]).Dx);
        Assert.True(Assert.IsType<MouseButtonPayload>(released[1]).Down);
        Assert.Equal(7, Assert.IsType<MouseMovePayload>(released[2]).Dx);
        Assert.False(Assert.IsType<MouseButtonPayload>(released[3]).Down);
    }

    [Fact]
    public void AnOversizedDeltaIsSplitAndTheSumIsPreserved()
    {
        var clock = new FakeClock();
        var limiter = new InputRateLimiter(clock, 100);

        // Ten thousand counts on one axis: more than the ±4096 the protocol accepts per event.
        limiter.EnqueueMove(10_000, -9_000);

        IReadOnlyList<IInputPayload> released = limiter.Drain();
        Assert.True(released.Count >= 3);

        int dx = 0;
        int dy = 0;
        foreach (IInputPayload payload in released)
        {
            var move = Assert.IsType<MouseMovePayload>(payload);
            Assert.InRange(move.Dx, -ProtocolLimits.MouseDeltaLimit, ProtocolLimits.MouseDeltaLimit);
            Assert.InRange(move.Dy, -ProtocolLimits.MouseDeltaLimit, ProtocolLimits.MouseDeltaLimit);
            dx += move.Dx;
            dy += move.Dy;
        }

        // Not clipped, not dropped: exactly the motion that went in comes out (03 §6.5).
        Assert.Equal(10_000, dx);
        Assert.Equal(-9_000, dy);
    }

    [Fact]
    public void ASplitTooLargeForTheBudgetFinishesInTheNextWindow()
    {
        var clock = new FakeClock();
        var limiter = new InputRateLimiter(clock, 2);

        limiter.EnqueueMove(20_000, 0); // needs 5 messages, budget is 2 per second

        int dx = 0;
        for (int second = 0; second < 4; second++)
        {
            foreach (IInputPayload payload in limiter.Drain())
                dx += Assert.IsType<MouseMovePayload>(payload).Dx;
            clock.Advance(TimeSpan.FromSeconds(1));
        }

        Assert.Equal(20_000, dx);
        Assert.Equal(0, limiter.PendingCount);
    }

    [Fact]
    public void DiscardPendingDropsTheQueueButNotTheWindow()
    {
        var clock = new FakeClock();
        var limiter = new InputRateLimiter(clock, 5);

        for (int i = 0; i < 20; i++)
            limiter.Enqueue(new KeyboardKeyPayload(HidUsage.B, i % 2 == 0));
        Assert.Equal(5, limiter.Drain().Count);
        Assert.True(limiter.PendingCount > 0);

        limiter.DiscardPending();

        Assert.Equal(0, limiter.PendingCount);
        Assert.Empty(limiter.Drain());
        // The device's rolling window did not reset because our queue did, so the budget is still spent.
        Assert.True(limiter.TimeUntilNextSlot() > TimeSpan.Zero);
    }

    [Fact]
    public void AMoveMayNotBeEnqueuedAsADiscreteEvent()
    {
        var limiter = new InputRateLimiter(new FakeClock());
        Assert.Throws<ArgumentException>(() => limiter.Enqueue(new MouseMovePayload(1, 1)));
    }
}
