// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Threading;
using System.Threading.Tasks;
using KVMpad.Client.Core.Input;
using KVMpad.Client.Core.Protocol;
using KVMpad.Client.Core.Session;

namespace KVMpad.Client.Core.TypeText;

/// <summary>How a "type text" run ended.</summary>
public enum TypeTextOutcome
{
    Completed,

    /// <summary>Esc in the client, or the user pressed Cancel (ADR-019 FR-703).</summary>
    Cancelled,

    /// <summary>The lease or the link went away part way through. The device runs <c>RELEASE_ALL</c> too.</summary>
    SessionLost,
}

/// <summary>The result of a run, with enough detail for the UI and nothing about the text itself.</summary>
public sealed class TypeTextResult
{
    public TypeTextResult(TypeTextOutcome outcome, int messagesSent, int messageCount, TypeTextPlan plan)
    {
        Outcome = outcome;
        MessagesSent = messagesSent;
        MessageCount = messageCount;
        Plan = plan;
    }

    public TypeTextOutcome Outcome { get; }
    public int MessagesSent { get; }
    public int MessageCount { get; }
    public TypeTextPlan Plan { get; }
}

/// <summary>
/// Paces a <see cref="TypeTextPlan"/> into a live session and guarantees that an interruption leaves nothing
/// held down.
/// </summary>
/// <remarks>
/// Cancellation is a first-class path, not an error path: on Esc, on a lost lease and on a lost link the run
/// stops and the session's release-all balances every DOWN already sent (ADR-019 FR-703, AT-EXT-12). The run
/// never retries and never resumes — a half-typed password is the user's business to fix, and silently
/// continuing after a takeover would be worse than stopping.
/// </remarks>
public sealed class TypeTextRun
{
    private readonly SessionLogic _logic;
    private readonly Func<System.Collections.Generic.IReadOnlyList<byte[]>, Task> _sendFrames;
    private readonly IClock _clock;

    /// <param name="logic">The live session.</param>
    /// <param name="sendFrames">
    /// How the release frames reach the device. Supplied by the caller — normally
    /// <c>RemoteSession</c> — so that a cancel is not merely computed but actually sent.
    /// </param>
    /// <param name="clock">Monotonic clock; the tests drive it.</param>
    public TypeTextRun(
        SessionLogic logic,
        Func<System.Collections.Generic.IReadOnlyList<byte[]>, Task>? sendFrames = null,
        IClock? clock = null)
    {
        _logic = logic ?? throw new ArgumentNullException(nameof(logic));
        _sendFrames = sendFrames ?? (_ => Task.CompletedTask);
        _clock = clock ?? SystemClock.Instance;
    }

    /// <summary>Raised after each character's worth of messages, so the UI can show progress.</summary>
    public event Action<int, int>? Progress;

    /// <summary>
    /// Sends the plan at the configured rate. Returns when the plan is finished, the token is cancelled, or
    /// the session can no longer carry input.
    /// </summary>
    public async Task<TypeTextResult> RunAsync(
        TypeTextPlan plan,
        int messagesPerSecond,
        CancellationToken cancellationToken)
    {
        if (plan is null)
            throw new ArgumentNullException(nameof(plan));
        if (messagesPerSecond < 1 || messagesPerSecond > ProtocolLimits.MaxTypeTextMessagesPerSecond)
            throw new ArgumentOutOfRangeException(nameof(messagesPerSecond));

        TimeSpan start = _clock.Elapsed;
        int sent = 0;

        try
        {
            for (int i = 0; i < plan.MessageCount; i++)
            {
                cancellationToken.ThrowIfCancellationRequested();

                if (!_logic.CanSendInput)
                    return await FinishAsync(TypeTextOutcome.SessionLost, sent, plan).ConfigureAwait(false);

                TimeSpan due = start + TypeTextPlanner.ScheduleOffset(i, messagesPerSecond);
                TimeSpan wait = due - _clock.Elapsed;
                if (wait > TimeSpan.Zero)
                    await Task.Delay(wait, cancellationToken).ConfigureAwait(false);

                if (!_logic.QueueInput(plan.Messages[i]))
                {
                    // The session rejected the message: either the lease went away between the check and here,
                    // or the queue filled. Either way the run stops and the release path takes over.
                    return await FinishAsync(TypeTextOutcome.SessionLost, sent, plan).ConfigureAwait(false);
                }

                sent++;
                Progress?.Invoke(sent, plan.MessageCount);
            }

            return new TypeTextResult(TypeTextOutcome.Completed, sent, plan.MessageCount, plan);
        }
        catch (OperationCanceledException)
        {
            return await FinishAsync(TypeTextOutcome.Cancelled, sent, plan).ConfigureAwait(false);
        }
    }

    private async Task<TypeTextResult> FinishAsync(TypeTextOutcome outcome, int sent, TypeTextPlan plan)
    {
        // Whatever the reason, let go of everything first. The ledger knows which keys are still down even
        // mid-character, so a cancel between a modifier DOWN and its UP cannot leave AltGr stuck. The frames
        // are handed to the caller's sender rather than returned, so a cancel is sent, not just computed.
        System.Collections.Generic.IReadOnlyList<byte[]> frames =
            _logic.BuildReleaseAllFrames(ReleaseReason.TypeTextCancelled);
        if (frames.Count > 0)
            await _sendFrames(frames).ConfigureAwait(false);
        return new TypeTextResult(outcome, sent, plan.MessageCount, plan);
    }
}
