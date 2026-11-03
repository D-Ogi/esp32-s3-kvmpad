// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using KVMpad.Client.Core.Protocol;

namespace KVMpad.Client.Core.Session;

/// <summary>
/// Keeps the client's input stream inside the protocol budget without losing an event.
/// </summary>
/// <remarks>
/// <para>
/// The device coalesces, throttles and finally revokes a client that exceeds 120 input msg/s in a rolling
/// window (03 §6.5). Rather than get throttled, the client shapes its own stream to a lower ceiling, and it
/// does so in a way that cannot change what the host sees:
/// </para>
/// <list type="bullet">
/// <item><description>
/// <c>mouse.move</c> is the one coalescable kind, and it merges only with an immediately adjacent move. A
/// click therefore never changes its position in the stream relative to the motion around it, which is what
/// a drag depends on (03 §5.3).
/// </description></item>
/// <item><description>
/// Everything else queues. A key UP or a button UP is never dropped to save budget — it waits, and the whole
/// point of a budget the client sets below the device's is that the wait is short.
/// </description></item>
/// <item><description>
/// A merged move whose delta leaves ±4096 is split across several messages whose deltas sum to the original.
/// It is never clipped: the device answers an out-of-range delta with <c>ERR_RANGE</c> and explicitly forbids
/// hidden clipping, and silently dropping the excess would desynchronize the pointer.
/// </description></item>
/// </list>
/// </remarks>
public sealed class InputRateLimiter
{
    private readonly IClock _clock;
    private readonly int _messagesPerSecond;
    private readonly int _maxPending;
    private readonly Queue<TimeSpan> _sendTimes = new();
    private readonly List<PendingItem> _pending = new();

    private static readonly TimeSpan Window = TimeSpan.FromSeconds(1);

    public InputRateLimiter(
        IClock clock,
        int messagesPerSecond = ProtocolLimits.ClientInputMessagesPerSecond,
        int maxPending = 1024)
    {
        if (messagesPerSecond < 1 || messagesPerSecond > ProtocolLimits.ServerInputMessagesPerSecond)
            throw new ArgumentOutOfRangeException(
                nameof(messagesPerSecond), messagesPerSecond,
                $"The client budget must be between 1 and the protocol limit of {ProtocolLimits.ServerInputMessagesPerSecond}.");
        _clock = clock ?? throw new ArgumentNullException(nameof(clock));
        _messagesPerSecond = messagesPerSecond;
        _maxPending = maxPending;
    }

    /// <summary>The ceiling this limiter enforces, in messages per second.</summary>
    public int MessagesPerSecond => _messagesPerSecond;

    /// <summary>Items waiting for budget. Moves count as one item however much motion they carry.</summary>
    public int PendingCount => _pending.Count;

    /// <summary>Total messages released since construction, after coalescing and splitting.</summary>
    public long SentCount { get; private set; }

    /// <summary>Moves absorbed into an adjacent move instead of becoming their own message.</summary>
    public long CoalescedMoveCount { get; private set; }

    /// <summary>Moves that had to be split because the accumulated delta left the protocol range.</summary>
    public long SplitMoveCount { get; private set; }

    /// <summary>
    /// Adds relative motion. If the tail of the queue is already a move, the motion is folded into it; the
    /// accumulator is a long, so no amount of folding can overflow before the next drain.
    /// </summary>
    public bool EnqueueMove(int dx, int dy)
    {
        if (dx == 0 && dy == 0)
            return true;

        if (_pending.Count > 0)
        {
            PendingItem tail = _pending[^1];
            if (tail.IsMove)
            {
                _pending[^1] = PendingItem.Move(tail.Dx + dx, tail.Dy + dy);
                CoalescedMoveCount++;
                return true;
            }
        }

        if (_pending.Count >= _maxPending)
            return false;
        _pending.Add(PendingItem.Move(dx, dy));
        return true;
    }

    /// <summary>
    /// Adds an event that must reach the host exactly once: a button, a wheel step, a key or a shortcut.
    /// Returns false only when the queue is full, which the caller must treat as back pressure and answer by
    /// releasing everything it holds rather than by discarding the event.
    /// </summary>
    public bool Enqueue(IInputPayload payload)
    {
        if (payload is null)
            throw new ArgumentNullException(nameof(payload));
        if (payload.IsCoalescable)
            throw new ArgumentException("Use EnqueueMove for a coalescable payload.", nameof(payload));
        if (_pending.Count >= _maxPending)
            return false;
        _pending.Add(PendingItem.Discrete(payload));
        return true;
    }

    /// <summary>
    /// Takes everything the budget allows right now, in order. An empty result means the window is full and
    /// the caller should come back later; nothing is lost in the meantime.
    /// </summary>
    public IReadOnlyList<IInputPayload> Drain()
    {
        TimeSpan now = _clock.Elapsed;
        TrimWindow(now);

        int budget = _messagesPerSecond - _sendTimes.Count;
        if (budget <= 0 || _pending.Count == 0)
            return Array.Empty<IInputPayload>();

        var released = new List<IInputPayload>(Math.Min(budget, _pending.Count));
        int consumed = 0;

        while (consumed < _pending.Count && released.Count < budget)
        {
            PendingItem item = _pending[consumed];
            if (!item.IsMove)
            {
                released.Add(item.Payload!);
                consumed++;
                continue;
            }

            int parts = PartsFor(item.Dx, item.Dy);
            if (parts == 1)
            {
                released.Add(new MouseMovePayload((int)item.Dx, (int)item.Dy));
                consumed++;
                continue;
            }

            // Release as many whole slices as the budget allows and keep the remainder queued, so motion is
            // delayed but never thrown away.
            int slicesAllowed = budget - released.Count;
            long dx = item.Dx;
            long dy = item.Dy;
            int emitted = 0;
            while (emitted < slicesAllowed && parts > 0)
            {
                long sliceDx = Slice(dx, parts);
                long sliceDy = Slice(dy, parts);
                released.Add(new MouseMovePayload((int)sliceDx, (int)sliceDy));
                dx -= sliceDx;
                dy -= sliceDy;
                parts--;
                emitted++;
                SplitMoveCount++;
            }

            if (dx == 0 && dy == 0)
            {
                consumed++;
            }
            else
            {
                _pending[consumed] = PendingItem.Move(dx, dy);
            }
            break;
        }

        if (consumed > 0)
            _pending.RemoveRange(0, consumed);

        foreach (IInputPayload _ in released)
            _sendTimes.Enqueue(now);
        SentCount += released.Count;
        return released;
    }

    /// <summary>
    /// How long until at least one more message may be sent. <see cref="TimeSpan.Zero"/> when budget is free.
    /// </summary>
    public TimeSpan TimeUntilNextSlot()
    {
        TimeSpan now = _clock.Elapsed;
        TrimWindow(now);
        if (_sendTimes.Count < _messagesPerSecond)
            return TimeSpan.Zero;
        TimeSpan oldest = _sendTimes.Peek();
        TimeSpan wait = oldest + Window - now;
        return wait > TimeSpan.Zero ? wait : TimeSpan.Zero;
    }

    /// <summary>
    /// Drops every queued event. Called when the session ends: an event queued under the old session must not
    /// travel under the new one (03 §6.4). Released-message history stays, because the device's rolling window
    /// does not reset just because our socket did.
    /// </summary>
    public void DiscardPending() => _pending.Clear();

    private void TrimWindow(TimeSpan now)
    {
        while (_sendTimes.Count > 0 && now - _sendTimes.Peek() >= Window)
            _sendTimes.Dequeue();
    }

    private static int PartsFor(long dx, long dy)
    {
        long limit = ProtocolLimits.MouseDeltaLimit;
        long partsX = (Math.Abs(dx) + limit - 1) / limit;
        long partsY = (Math.Abs(dy) + limit - 1) / limit;
        long parts = Math.Max(1, Math.Max(partsX, partsY));
        return (int)Math.Min(parts, int.MaxValue);
    }

    /// <summary>One of <paramref name="parts"/> equal shares of <paramref name="total"/>, rounded away from zero.</summary>
    private static long Slice(long total, int parts)
    {
        if (parts <= 1)
            return total;
        long magnitude = (Math.Abs(total) + parts - 1) / parts;
        magnitude = Math.Min(magnitude, ProtocolLimits.MouseDeltaLimit);
        return total < 0 ? -magnitude : magnitude;
    }

    private readonly struct PendingItem
    {
        private PendingItem(bool isMove, long dx, long dy, IInputPayload? payload)
        {
            IsMove = isMove;
            Dx = dx;
            Dy = dy;
            Payload = payload;
        }

        public static PendingItem Move(long dx, long dy) => new(true, dx, dy, null);

        public static PendingItem Discrete(IInputPayload payload) => new(false, 0, 0, payload);

        public bool IsMove { get; }
        public long Dx { get; }
        public long Dy { get; }
        public IInputPayload? Payload { get; }
    }
}
