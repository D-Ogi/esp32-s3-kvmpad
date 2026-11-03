// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using KVMpad.Client.Core.Protocol;

namespace KVMpad.Client.Core.Input;

/// <summary>Why the client is letting go of everything it holds down.</summary>
public enum ReleaseReason
{
    /// <summary>The user pressed the capture toggle. The lease is still ours, so the UP events go out.</summary>
    CaptureToggledOff,

    /// <summary>The client's own window stopped being the active one.</summary>
    FocusLost,

    /// <summary>The Windows session locked, switched or logged off (WM_WTSSESSION_CHANGE).</summary>
    SessionChanged,

    /// <summary>The CLIENT machine is suspending (PBT_APMSUSPEND).</summary>
    Suspend,

    /// <summary>The application is shutting down.</summary>
    Shutdown,

    /// <summary>The transport went away. Nothing can be sent; the device runs RELEASE_ALL on its own.</summary>
    ConnectionLost,

    /// <summary>The device revoked control. We no longer have an epoch, so input would only earn errors.</summary>
    ControlRevoked,

    /// <summary>A "type text" run was cancelled part way through (ADR-019 FR-703).</summary>
    TypeTextCancelled,

    /// <summary>
    /// A KVMpad is attached to the CLIENT machine by USB: capturing would feed its own output back to it in a loop
    /// (<see cref="KvmpadUsbIdentity"/>).
    /// </summary>
    LoopGuard,
}

/// <summary>One key or button the client must let go of.</summary>
public readonly struct ReleaseEvent
{
    private ReleaseEvent(byte keyUsage, string? button)
    {
        KeyUsage = keyUsage;
        Button = button;
    }

    public static ReleaseEvent ForKey(byte usage) => new(usage, null);

    public static ReleaseEvent ForButton(string button) => new(HidUsage.None, button);

    public byte KeyUsage { get; }
    public string? Button { get; }
    public bool IsKey => Button is null;
}

/// <summary>The outcome of a release-all: what to send, and whether it can be sent at all.</summary>
public sealed class ReleaseAllResult
{
    public ReleaseAllResult(ReleaseReason reason, IReadOnlyList<ReleaseEvent> events)
    {
        Reason = reason;
        Events = events;
    }

    public ReleaseReason Reason { get; }

    /// <summary>UP events in reverse press order, so a modifier pressed first is released last (02 §7.3).</summary>
    public IReadOnlyList<ReleaseEvent> Events { get; }

    /// <summary>True when the lease and the link are still available, so these UP events must actually be sent.</summary>
    public bool ShouldTransmit => RequiresTransmit(Reason);

    /// <summary>
    /// Whether the UP events can reach the device. On a lost connection or a revoked lease they cannot: the
    /// device performs <c>RELEASE_ALL</c> itself on loss of owner, USB or session (AGENTS §2.3, 03 §4.2), and
    /// sending stale input would only collect <c>ERR_STALE_EPOCH</c>. Every other reason keeps the lease, so
    /// the client owes the host an explicit UP for each key it pressed (ADR-021).
    /// </summary>
    public static bool RequiresTransmit(ReleaseReason reason) => reason switch
    {
        ReleaseReason.ConnectionLost => false,
        ReleaseReason.ControlRevoked => false,
        _ => true,
    };
}

/// <summary>
/// The client's record of everything it currently holds down. Its whole purpose is that no key can stay
/// stuck: every exit from capture, every loss of the lease and every shutdown path runs through
/// <see cref="ReleaseAll"/>, which empties the ledger and reports the UP events that balance the DOWN
/// events the client sent (ADR-021, AGENTS §2.3).
/// </summary>
/// <remarks>
/// Nothing here records what was typed: the ledger holds only the usages still physically down, it is cleared
/// on every release, and it is never written to a log (TECH-046, ADR-019 FR-705).
/// </remarks>
public sealed class CaptureLedger
{
    private readonly List<byte> _pressedKeys = new(8);
    private readonly List<string> _pressedButtons = new(3);

    /// <summary>Usages currently held down, in press order.</summary>
    public IReadOnlyList<byte> PressedKeys => _pressedKeys;

    /// <summary>Mouse buttons currently held down, in press order.</summary>
    public IReadOnlyList<string> PressedButtons => _pressedButtons;

    /// <summary>True when the client holds at least one key or button.</summary>
    public bool HasPressed => _pressedKeys.Count > 0 || _pressedButtons.Count > 0;

    /// <summary>Count of UP events seen without a matching DOWN. A metric only; never a keyboard reset (03 §6.5).</summary>
    public int OrphanUpCount { get; private set; }

    /// <summary>Count of DOWN events suppressed because the key was already down (auto-repeat).</summary>
    public int SuppressedRepeatCount { get; private set; }

    /// <summary>
    /// Records a key event and reports whether it must be forwarded. A DOWN for a key already down is an
    /// idempotent no-op, so Windows auto-repeat never turns into a flood of identical reports; an UP without a
    /// DOWN is dropped and counted (03 §6.5).
    /// </summary>
    public bool OnKey(byte usage, bool down)
    {
        if (!HidUsage.IsValidKey(usage))
            return false;

        int index = _pressedKeys.IndexOf(usage);
        if (down)
        {
            if (index >= 0)
            {
                SuppressedRepeatCount++;
                return false;
            }
            _pressedKeys.Add(usage);
            return true;
        }

        if (index < 0)
        {
            OrphanUpCount++;
            return false;
        }
        _pressedKeys.RemoveAt(index);
        return true;
    }

    /// <summary>The same contract as <see cref="OnKey"/>, for a mouse button.</summary>
    public bool OnMouseButton(string button, bool down)
    {
        if (button != MouseButtons.Left && button != MouseButtons.Right && button != MouseButtons.Middle)
            return false;

        int index = _pressedButtons.IndexOf(button);
        if (down)
        {
            if (index >= 0)
            {
                SuppressedRepeatCount++;
                return false;
            }
            _pressedButtons.Add(button);
            return true;
        }

        if (index < 0)
        {
            OrphanUpCount++;
            return false;
        }
        _pressedButtons.RemoveAt(index);
        return true;
    }

    /// <summary>
    /// Empties the ledger and returns the UP events that balance it. Buttons are released before keys, and
    /// within each group in reverse press order, so a modifier held across a chord is the last thing to go up.
    /// The ledger is cleared whether or not the caller can transmit, because a key the client can no longer
    /// release is a key the client must stop believing it holds.
    /// </summary>
    public ReleaseAllResult ReleaseAll(ReleaseReason reason)
    {
        var events = new List<ReleaseEvent>(_pressedKeys.Count + _pressedButtons.Count);
        for (int i = _pressedButtons.Count - 1; i >= 0; i--)
            events.Add(ReleaseEvent.ForButton(_pressedButtons[i]));
        for (int i = _pressedKeys.Count - 1; i >= 0; i--)
            events.Add(ReleaseEvent.ForKey(_pressedKeys[i]));

        _pressedButtons.Clear();
        _pressedKeys.Clear();
        return new ReleaseAllResult(reason, events);
    }

    /// <summary>Forgets every held key without producing UP events. Only for a state the client cannot own any more.</summary>
    public void Forget()
    {
        _pressedButtons.Clear();
        _pressedKeys.Clear();
    }

    /// <summary>A short, secret-free line for the status UI: counts, never usages (TECH-046).</summary>
    public string Describe() =>
        $"{_pressedKeys.Count} key(s), {_pressedButtons.Count} button(s) held";
}
