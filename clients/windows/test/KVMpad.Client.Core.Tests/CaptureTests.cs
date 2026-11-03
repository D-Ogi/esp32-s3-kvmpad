// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System.Collections.Generic;
using KVMpad.Client.Core.Input;
using KVMpad.Client.Core.Protocol;
using KVMpad.Client.Core.Session;
using Xunit;

namespace KVMpad.Client.Core.Tests;

/// <summary>
/// No stuck keys, under any exit from capture. ADR-021 names the toggle-with-a-key-held case specifically;
/// the same release path covers focus loss, a locked session, a suspend, a shutdown, a revoked lease and a
/// dropped link.
/// </summary>
public sealed class CaptureTests
{
    private sealed class Harness
    {
        public Harness(bool withLease = true)
        {
            Clock = new FakeClock();
            Ledger = new CaptureLedger();
            Logic = new SessionLogic(Clock, new SessionOptions(), Ledger);
            Logic.OnTransportOpened();
            Logic.OnMessage(ProtocolCodec.Decode(
                """{"v":1,"type":"hello.ack","seq":1,"session_id":"c-1","payload":{"protocol":1}}"""));
            if (withLease)
            {
                Logic.OnMessage(ProtocolCodec.Decode(
                    """{"v":1,"type":"control.granted","seq":2,"session_id":"c-1","lease_epoch":4,"payload":{}}"""));
            }
            Logic.OnMessage(ProtocolCodec.Decode(
                """{"v":1,"type":"status.snapshot","seq":3,"session_id":"c-1","payload":{"host":{"state":"USB_MOUNTED"},"client":{"state":"CLIENT_CONNECTED"},"control":{"owner":"REMOTE","lease_epoch":4},"device":{"wifi":"CONNECTED"}}}"""));

            Controller = new CaptureController(Logic, frames => Posted.AddRange(frames));
        }

        public FakeClock Clock { get; }
        public CaptureLedger Ledger { get; }
        public SessionLogic Logic { get; }
        public CaptureController Controller { get; }
        public List<byte[]> Posted { get; } = new();

        public IReadOnlyList<ServerMessage> PostedDecoded
        {
            get
            {
                var decoded = new List<ServerMessage>(Posted.Count);
                foreach (byte[] frame in Posted)
                    decoded.Add(ProtocolCodec.Decode(frame));
                return decoded;
            }
        }

        /// <summary>Presses the A key by its scan code, as the hook would report it.</summary>
        public CaptureDecision PressA(bool down) => Controller.OnKey(0x1E, 'A', false, down);

        /// <summary>Presses left Ctrl by its scan code.</summary>
        public CaptureDecision PressCtrl(bool down) => Controller.OnKey(0x1D, VirtualKey.LeftControl, false, down);

        /// <summary>Presses the configured toggle key, Scroll Lock by default.</summary>
        public CaptureDecision PressToggle(bool down) => Controller.OnKey(0x46, VirtualKey.ScrollLock, false, down);
    }

    [Fact]
    public void CaptureNeedsTheLease()
    {
        var harness = new Harness(withLease: false);
        string? refusal = null;
        harness.Controller.CaptureRefused += message => refusal = message;

        Assert.False(harness.Controller.TryEnterCapture());
        Assert.False(harness.Controller.IsCapturing);
        Assert.NotNull(refusal);
        Assert.Contains("control", refusal, System.StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void WithoutCaptureKeysReachTheClientMachine()
    {
        var harness = new Harness();
        Assert.Equal(CaptureDecision.PassThrough, harness.PressA(true));
        Assert.Empty(harness.Ledger.PressedKeys);
    }

    [Fact]
    public void TheToggleKeyIsNeverForwardedToEitherMachine()
    {
        var harness = new Harness();

        Assert.Equal(CaptureDecision.Swallow, harness.PressToggle(true));
        Assert.True(harness.Controller.IsCapturing);
        Assert.Equal(CaptureDecision.Swallow, harness.PressToggle(false));

        // The toggle produced no input message at all: the host must not see a Scroll Lock press.
        Assert.Empty(harness.Logic.DrainInput());
        Assert.Empty(harness.Ledger.PressedKeys);
    }

    [Fact]
    public void TogglingOffWithKeysHeldSendsAnUpForEachOfThem()
    {
        // The test ADR-021 calls for by name: "the capture toggle with a key pressed → no stuck keys".
        var harness = new Harness();
        harness.PressToggle(true);
        harness.PressToggle(false);

        harness.PressCtrl(true);
        harness.PressA(true);
        Assert.Equal(2, harness.Ledger.PressedKeys.Count);

        harness.PressToggle(true); // leaves capture while Ctrl and A are still down

        Assert.False(harness.Controller.IsCapturing);
        Assert.Empty(harness.Ledger.PressedKeys);

        IReadOnlyList<ServerMessage> posted = harness.PostedDecoded;
        Assert.Equal(2, posted.Count);
        foreach (ServerMessage message in posted)
        {
            Assert.Equal(MessageTypes.Input, message.Type);
            Assert.Equal(InputKinds.KeyboardKey, message.PayloadString("kind"));
            Assert.False(message.PayloadBool("down"));
            Assert.Equal(4u, message.LeaseEpoch);
        }
        // Reverse press order, so A goes up before the modifier that qualified it (02 §7.3).
        Assert.Equal(HidUsage.A, (byte)posted[0].PayloadInt64("usage_id")!.Value);
        Assert.Equal(HidUsage.LeftCtrl, (byte)posted[1].PayloadInt64("usage_id")!.Value);
    }

    [Fact]
    public void HeldMouseButtonsAreReleasedBeforeHeldKeys()
    {
        var harness = new Harness();
        harness.Controller.TryEnterCapture();
        harness.PressCtrl(true);
        harness.Controller.OnMouseButton(MouseButtons.Left, true);

        harness.Controller.ExitCapture(ReleaseReason.CaptureToggledOff);

        IReadOnlyList<ServerMessage> posted = harness.PostedDecoded;
        Assert.Equal(2, posted.Count);
        Assert.Equal(InputKinds.MouseButton, posted[0].PayloadString("kind"));
        Assert.Equal(InputKinds.KeyboardKey, posted[1].PayloadString("kind"));
    }

    [Fact]
    public void LosingFocusALockedSessionOrASuspendAllReleaseEverything()
    {
        foreach (string trigger in new[] { "focus", "session", "suspend" })
        {
            var harness = new Harness();
            harness.Controller.TryEnterCapture();
            harness.PressCtrl(true);
            Assert.Single(harness.Ledger.PressedKeys);

            ReleaseReason? reason = null;
            harness.Controller.CaptureExited += r => reason = r;

            switch (trigger)
            {
                case "focus":
                    harness.Controller.OnActivationChanged(false);
                    break;
                case "session":
                    harness.Controller.OnSessionChanged();
                    break;
                default:
                    harness.Controller.OnSuspend();
                    break;
            }

            Assert.False(harness.Controller.IsCapturing);
            Assert.Empty(harness.Ledger.PressedKeys);
            Assert.Single(harness.Posted);
            Assert.NotNull(reason);
        }
    }

    [Fact]
    public void ADisconnectWithKeysHeldClearsTheLedgerAndSendsNothing()
    {
        var harness = new Harness();
        harness.Controller.TryEnterCapture();
        harness.PressCtrl(true);
        harness.PressA(true);

        harness.Logic.OnTransportClosed();
        harness.Controller.OnSessionStateChanged(harness.Logic.State);

        Assert.False(harness.Controller.IsCapturing);
        Assert.Empty(harness.Ledger.PressedKeys);
        // Nothing can be sent on a dead link; the device runs RELEASE_ALL itself (VEC-005, 03 §4.2).
        Assert.Empty(harness.Posted);
    }

    [Fact]
    public void AutoRepeatDoesNotProduceASecondDownMessage()
    {
        // 03 §6.5: "keyboard.key already down → idempotent no-op, do not repeat to USB."
        var harness = new Harness();
        harness.Controller.TryEnterCapture();

        harness.PressA(true);
        for (int i = 0; i < 20; i++)
            harness.PressA(true);

        Assert.Single(harness.Ledger.PressedKeys);
        Assert.Equal(20, harness.Ledger.SuppressedRepeatCount);
        Assert.Single(harness.Logic.DrainInput());
    }

    [Fact]
    public void AnUpWithoutADownIsCountedAndDropped()
    {
        // 03 §6.5: "keyboard.key up without down → no-op + metric; no uncontrolled reset of the keyboard."
        var harness = new Harness();
        harness.Controller.TryEnterCapture();

        harness.PressA(false);

        Assert.Equal(1, harness.Ledger.OrphanUpCount);
        Assert.Empty(harness.Ledger.PressedKeys);
        Assert.Empty(harness.Logic.DrainInput());
    }

    [Fact]
    public void WhileCapturingAnUnmappableKeyIsStillSwallowed()
    {
        // The CLIENT machine must not act on the user's keystrokes while capture is on, even for a key that
        // has no HID usage the device accepts.
        var harness = new Harness();
        harness.Controller.TryEnterCapture();

        Assert.Equal(CaptureDecision.Swallow, harness.Controller.OnKey(0x00, 0xFF, false, true));
        Assert.Empty(harness.Ledger.PressedKeys);
    }

    [Fact]
    public void OnlyAnAllowlistedShortcutIsSent()
    {
        var harness = new Harness();
        Assert.True(harness.Controller.SendShortcut(ChordIds.CtrlAltDel));
        Assert.False(harness.Controller.SendShortcut("sudo_rm"));

        ServerMessage sent = ProtocolCodec.Decode(harness.Logic.DrainInput()[0]);
        Assert.Equal(InputKinds.Shortcut, sent.PayloadString("kind"));
        Assert.Equal(ChordIds.CtrlAltDel, sent.PayloadString("command_id"));
    }

    [Fact]
    public void NaturalScrollInvertsBothAxes()
    {
        var clock = new FakeClock();
        var ledger = new CaptureLedger();
        var logic = new SessionLogic(clock, new SessionOptions(), ledger);
        logic.OnTransportOpened();
        logic.OnMessage(ProtocolCodec.Decode(
            """{"v":1,"type":"hello.ack","seq":1,"session_id":"c-1","payload":{}}"""));
        logic.OnMessage(ProtocolCodec.Decode(
            """{"v":1,"type":"control.granted","seq":2,"session_id":"c-1","lease_epoch":1,"payload":{}}"""));

        var controller = new CaptureController(logic, _ => { }, new CaptureOptions { NaturalScroll = true });
        controller.TryEnterCapture();
        controller.OnMouseWheel(3, -2);

        ServerMessage sent = ProtocolCodec.Decode(logic.DrainInput()[0]);
        Assert.Equal(-3, sent.PayloadInt64("vertical"));
        Assert.Equal(2, sent.PayloadInt64("horizontal"));
    }

    [Fact]
    public void TheLedgerDescriptionCarriesCountsAndNoUsages()
    {
        // TECH-046: nothing the user typed may end up where it could be read back.
        var ledger = new CaptureLedger();
        ledger.OnKey(HidUsage.A, true);
        ledger.OnKey(HidUsage.LeftShift, true);

        string description = ledger.Describe();

        Assert.Contains("2 key(s)", description);
        Assert.DoesNotContain("0x04", description);
        Assert.DoesNotContain("4", description.Replace("key(s)", string.Empty).Replace("button(s)", string.Empty));
    }

    [Fact]
    public void ReleaseReasonsThatCannotTransmitAreExactlyTheTwoWithoutALink()
    {
        Assert.False(ReleaseAllResult.RequiresTransmit(ReleaseReason.ConnectionLost));
        Assert.False(ReleaseAllResult.RequiresTransmit(ReleaseReason.ControlRevoked));
        Assert.True(ReleaseAllResult.RequiresTransmit(ReleaseReason.CaptureToggledOff));
        Assert.True(ReleaseAllResult.RequiresTransmit(ReleaseReason.FocusLost));
        Assert.True(ReleaseAllResult.RequiresTransmit(ReleaseReason.SessionChanged));
        Assert.True(ReleaseAllResult.RequiresTransmit(ReleaseReason.Suspend));
        Assert.True(ReleaseAllResult.RequiresTransmit(ReleaseReason.Shutdown));
        Assert.True(ReleaseAllResult.RequiresTransmit(ReleaseReason.TypeTextCancelled));
    }
}
