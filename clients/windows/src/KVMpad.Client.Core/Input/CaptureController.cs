// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using KVMpad.Client.Core.Protocol;
using KVMpad.Client.Core.Session;

namespace KVMpad.Client.Core.Input;

/// <summary>What the low-level hook should do with an event after the controller has seen it.</summary>
public enum CaptureDecision
{
    /// <summary>Let the event reach the CLIENT machine as usual.</summary>
    PassThrough,

    /// <summary>Consume it: the event belongs to the HOST, or it is the toggle key itself.</summary>
    Swallow,
}

/// <summary>Capture settings the user can change.</summary>
public sealed class CaptureOptions
{
    /// <summary>The toggle key's virtual-key code. Scroll Lock by default, configurable (ADR-021).</summary>
    public int ToggleVirtualKey { get; init; } = VirtualKey.ScrollLock;

    /// <summary>
    /// Whether losing activation of the client's own window leaves capture. On by default: a global hook that
    /// keeps swallowing keys after the user has clicked another local window would make the CLIENT machine
    /// look broken, and the toggle key is the deliberate way back in.
    /// </summary>
    public bool ExitCaptureWhenDeactivated { get; init; } = true;

    /// <summary>Natural scrolling, matching the device's own setting name (FR-502).</summary>
    public bool NaturalScroll { get; init; }
}

/// <summary>
/// The bridge from captured Windows input to typed protocol intents.
/// </summary>
/// <remarks>
/// <para>
/// Nothing here touches USB or HID: it produces <c>input</c> messages that the device's own arbiter turns
/// into reports, which is the only path a remote client has (TECH-045, AGENTS §2.2 read from this side).
/// </para>
/// <para>
/// Every exit from capture goes through <see cref="ExitCapture"/>, and every exit releases what is held. That
/// includes the case the ADR-021 test calls out: pressing the toggle while a key is down must not leave that
/// key stuck on the HOST. The toggle itself is always swallowed, so it reaches neither the CLIENT machine nor
/// the HOST.
/// </para>
/// <para>
/// No keystroke is logged, counted per usage or kept beyond the ledger of what is currently down (TECH-046).
/// </para>
/// </remarks>
public sealed class CaptureController
{
    private readonly SessionLogic _logic;
    private readonly Action<IReadOnlyList<byte[]>> _postFrames;
    private readonly CaptureOptions _options;

    public CaptureController(
        SessionLogic logic,
        Action<IReadOnlyList<byte[]>> postFrames,
        CaptureOptions? options = null)
    {
        _logic = logic ?? throw new ArgumentNullException(nameof(logic));
        _postFrames = postFrames ?? throw new ArgumentNullException(nameof(postFrames));
        _options = options ?? new CaptureOptions();
    }

    public bool IsCapturing { get; private set; }

    public CaptureOptions Options => _options;

    /// <summary>Raised whenever capture starts or stops, for the indicator in the UI.</summary>
    public event Action<bool>? CaptureChanged;

    /// <summary>Raised with the reason each time capture stops, so the status line can say why.</summary>
    public event Action<ReleaseReason>? CaptureExited;

    /// <summary>Raised when capture was refused, with an English sentence explaining what is missing.</summary>
    public event Action<string>? CaptureRefused;

    /// <summary>
    /// Starts capture, but only with the lease in hand: without it every event would be rejected by the
    /// device and the user would be typing into a void with a dead local keyboard (03 §6.5).
    /// </summary>
    public bool TryEnterCapture()
    {
        if (IsCapturing)
            return true;

        if (!_logic.CanSendInput)
        {
            CaptureRefused?.Invoke(
                "Capture needs control of the host. Request control first; the device may ask for a tap on its screen.");
            return false;
        }

        if (_logic.Snapshot is { IsHostMounted: false })
        {
            CaptureRefused?.Invoke("The host is not connected over USB, so the device would reject input.");
            return false;
        }

        IsCapturing = true;
        CaptureChanged?.Invoke(true);
        return true;
    }

    /// <summary>
    /// Stops capture and releases every held key and button. Safe to call when not capturing: the release
    /// still runs, which is what makes it usable as the single answer to every loss of focus, session, lease
    /// or link.
    /// </summary>
    public void ExitCapture(ReleaseReason reason)
    {
        bool was = IsCapturing;
        IsCapturing = false;

        IReadOnlyList<byte[]> frames = _logic.BuildReleaseAllFrames(reason);
        if (frames.Count > 0)
            _postFrames(frames);

        if (was)
            CaptureChanged?.Invoke(false);
        CaptureExited?.Invoke(reason);
    }

    /// <summary>Flips capture with the toggle key's semantics.</summary>
    public void Toggle()
    {
        if (IsCapturing)
            ExitCapture(ReleaseReason.CaptureToggledOff);
        else
            TryEnterCapture();
    }

    /// <summary>
    /// Handles one keyboard event from the low-level hook.
    /// </summary>
    /// <returns>Whether the hook should swallow the event.</returns>
    public CaptureDecision OnKey(int scanCode, int virtualKey, bool extended, bool down)
    {
        if (virtualKey == _options.ToggleVirtualKey)
        {
            // Act on the press, swallow both halves. Forwarding either one would toggle the CLIENT machine's
            // own Scroll Lock state or send a key the user meant as a local command.
            if (down)
                Toggle();
            return CaptureDecision.Swallow;
        }

        if (!IsCapturing)
            return CaptureDecision.PassThrough;

        if (!WindowsKeyMap.TryMap(scanCode, virtualKey, extended, out byte usage))
        {
            // A key with no usage the device accepts. Swallowed rather than passed through, because while
            // capture is on the CLIENT machine must not act on the user's keystrokes either.
            return CaptureDecision.Swallow;
        }

        _logic.QueueInput(new KeyboardKeyPayload(usage, down));
        return CaptureDecision.Swallow;
    }

    /// <summary>Handles relative mouse motion from Raw Input, which carries no pointer acceleration.</summary>
    public void OnMouseMove(int dx, int dy)
    {
        if (!IsCapturing)
            return;
        _logic.QueueMouseMove(dx, dy);
    }

    public CaptureDecision OnMouseButton(string button, bool down)
    {
        if (!IsCapturing)
            return CaptureDecision.PassThrough;
        _logic.QueueInput(new MouseButtonPayload(button, down));
        return CaptureDecision.Swallow;
    }

    public CaptureDecision OnMouseWheel(int vertical, int horizontal)
    {
        if (!IsCapturing)
            return CaptureDecision.PassThrough;
        int direction = _options.NaturalScroll ? -1 : 1;
        _logic.QueueInput(new MouseWheelPayload(vertical * direction, horizontal * direction));
        return CaptureDecision.Swallow;
    }

    /// <summary>
    /// Sends an allowlisted chord by id. Available outside capture too, because a shortcut is a deliberate
    /// single action and does not need the keyboard redirected to issue it.
    /// </summary>
    public bool SendShortcut(string commandId)
    {
        if (!ChordIds.IsAllowlisted(commandId))
            return false;
        return _logic.QueueInput(new ShortcutPayload(commandId));
    }

    /// <summary>The client's own activation changed; leaves capture if that is the configured behavior.</summary>
    public void OnActivationChanged(bool active)
    {
        if (!active && IsCapturing && _options.ExitCaptureWhenDeactivated)
            ExitCapture(ReleaseReason.FocusLost);
    }

    /// <summary>The Windows session locked, switched or logged off.</summary>
    public void OnSessionChanged()
    {
        if (IsCapturing || _logic.Ledger.HasPressed)
            ExitCapture(ReleaseReason.SessionChanged);
    }

    /// <summary>The CLIENT machine is suspending.</summary>
    public void OnSuspend()
    {
        if (IsCapturing || _logic.Ledger.HasPressed)
            ExitCapture(ReleaseReason.Suspend);
    }

    /// <summary>The session state changed; capture cannot survive losing the lease or the link.</summary>
    public void OnSessionStateChanged(ClientSessionState state)
    {
        if (state == ClientSessionState.Controlling)
            return;
        if (!IsCapturing)
            return;
        ExitCapture(state == ClientSessionState.Disconnected
            ? ReleaseReason.ConnectionLost
            : ReleaseReason.ControlRevoked);
    }
}
