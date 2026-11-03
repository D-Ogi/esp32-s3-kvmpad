// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using KVMpad.Client.App.Interop;
using KVMpad.Client.Core.Discovery;
using KVMpad.Client.Core.Input;
using KVMpad.Client.Core.Pairing;
using KVMpad.Client.Core.Session;
using KVMpad.Client.Core.Storage;
using KVMpad.Client.Core.TypeText;

namespace KVMpad.Client.App;

/// <summary>
/// Wires the pieces together: the credential store, discovery, a session, the capture controller, the
/// keyboard hook and the Raw Input window. The forms talk to this and never to the interop layer.
/// </summary>
internal sealed class ClientController : IDisposable
{
    private readonly AppSettings _settings;
    private readonly ICredentialStore _store;
    private readonly SystemEventWindow _events;
    private readonly LowLevelKeyboardHook _hook;

    private CaptureLedger _ledger = new();
    private SessionLogic? _logic;
    private RemoteSession? _session;
    private CaptureController? _capture;
    private CancellationTokenSource? _sessionCts;
    private Task? _sessionTask;
    private CancellationTokenSource? _typeTextCts;

    public ClientController(AppSettings settings, ICredentialStore store)
    {
        _settings = settings ?? throw new ArgumentNullException(nameof(settings));
        _store = store ?? throw new ArgumentNullException(nameof(store));
        _events = new SystemEventWindow(OnRawMouse, OnSessionChanged, OnSuspend, OnKvmpadArrived);
        _hook = new LowLevelKeyboardHook(OnHookedKey);
    }

    public ICredentialStore Store => _store;

    public SessionLogic? Logic => _logic;

    public CaptureController? Capture => _capture;

    public bool IsCapturing => _capture?.IsCapturing ?? false;

    /// <summary>A one-line, secret-free description of where things stand, for the status bar.</summary>
    public string StatusLine { get; private set; } = "Not connected";

    public event Action? Changed;

    /// <summary>Raised with a sentence the user should read, for a refusal or a failure.</summary>
    public event Action<string>? Notice;

    // ---- discovery -------------------------------------------------------------------------------

    /// <summary>Browses the LAN for devices (ADR-022 FR-723).</summary>
    public async Task<IReadOnlyList<DiscoveredDevice>> DiscoverAsync(CancellationToken cancellationToken)
    {
        var browser = new MdnsBrowser();
        try
        {
            return await browser.BrowseAsync(TimeSpan.FromSeconds(3), cancellationToken).ConfigureAwait(false);
        }
        catch (System.Net.Sockets.SocketException ex)
        {
            Notice?.Invoke(
                "Could not browse the network for devices: " + ex.Message
                + " Use Advanced to enter the address shown on the device.");
            return Array.Empty<DiscoveredDevice>();
        }
    }

    // ---- pairing ---------------------------------------------------------------------------------

    /// <summary>
    /// Runs one pairing attempt against a discovered device (ADR-022, ADR-029) and, on success, stores the
    /// token and the pin.
    /// </summary>
    /// <remarks>
    /// One attempt, one code, nothing kept on failure. The fingerprint that gets pinned is the one the
    /// pairing handshake presented and the PAKE transcript confirmed — not one taken from a later
    /// handshake — which is what makes the pin mean anything (SEC-102). Nothing here logs or reports the
    /// code, and a failure is described by reason only, never by which half of the secret was wrong.
    /// </remarks>
    public async Task<bool> PairAsync(DiscoveredDevice device, string code)
    {
        if (device is null)
            throw new ArgumentNullException(nameof(device));

        IPakeHandshake pake;
        try
        {
            pake = new NativePakeHandshake();
        }
        catch (PairingUnavailableException ex)
        {
            Notice?.Invoke(ex.Message);
            return false;
        }

        var transport = new PairingWebSocketTransport(device.ConnectHost, device.Port);
        PairingContext? context = null;
        try
        {
            await transport.ConnectAsync(CancellationToken.None).ConfigureAwait(false);

            context = new PairingContext(code, Environment.MachineName, _settings.ClientId,
                transport.CapturedFingerprint!);

            var client = new PairingClient(transport, pake);
            PairingResult result = await client.RunAsync(context, CancellationToken.None).ConfigureAwait(false);

            var paired = new PairedDevice(
                device.InstanceName,
                result.DeviceName,
                device.ConnectHost,
                device.Port,
                result.PinnedFingerprint,
                result.Token,
                DateTimeOffset.UtcNow);
            _store.Save(paired);
            _settings.LastDeviceId = paired.DeviceId;
            _settings.Save();

            Notice?.Invoke($"Paired with {result.DeviceName}.");
            Changed?.Invoke();
            return true;
        }
        catch (PairingFailedException ex)
        {
            Notice?.Invoke(ex.Message);
            return false;
        }
        catch (Exception ex) when (ex is System.Net.WebSockets.WebSocketException
                                      or System.Net.Sockets.SocketException
                                      or InvalidOperationException
                                      or OperationCanceledException)
        {
            Notice?.Invoke("Could not reach the device's pairing endpoint. "
                + "Check that its pairing window is still open, then try again.");
            return false;
        }
        finally
        {
            // Both orderings matter: the code dies with the context and the keys die with the handshake,
            // whether the attempt succeeded or not (SEC-105).
            context?.Clear();
            pake.Dispose();
            try
            {
                await transport.CloseAsync(CancellationToken.None).ConfigureAwait(false);
            }
            catch (Exception ex) when (ex is System.Net.WebSockets.WebSocketException
                                          or InvalidOperationException)
            {
                // The socket is going away regardless.
            }
            await transport.DisposeAsync().ConfigureAwait(false);
        }
    }

    // ---- session ---------------------------------------------------------------------------------

    /// <summary>
    /// Opens a session to a paired device. A device that is not paired has no token and no pin, so there is
    /// nothing to connect with — pairing comes first, always (ADR-022).
    /// </summary>
    public async Task ConnectAsync(PairedDevice device, CancellationToken cancellationToken)
    {
        await DisconnectAsync().ConfigureAwait(false);

        _ledger = new CaptureLedger();
        var options = new SessionOptions
        {
            ClientName = Environment.MachineName,
            ControlReason = "remote_support",
        };
        _logic = new SessionLogic(SystemClock.Instance, options, _ledger);
        var transport = new ClientWebSocketTransport(device.ToEndpoint());
        _session = new RemoteSession(transport, _logic);
        _capture = new CaptureController(_logic, PostFrames, _settings.ToCaptureOptions());

        _logic.StateChanged += OnSessionStateChanged;
        _logic.ErrorReceived += OnSessionError;
        _capture.CaptureChanged += OnCaptureChanged;
        _capture.CaptureRefused += message => Notice?.Invoke(message);
        _session.FrameRejected += message => Notice?.Invoke("The device sent a frame this client refused: " + message);

        _sessionCts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        CancellationToken token = _sessionCts.Token;
        RemoteSession session = _session;

        _sessionTask = Task.Run(async () =>
        {
            try
            {
                await session.RunAsync(token).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                // Disconnect requested.
            }
            catch (Exception ex)
            {
                Notice?.Invoke("The connection to " + device.DeviceName + " failed: " + ex.Message);
            }
            finally
            {
                SetStatus();
            }
        }, CancellationToken.None);

        _settings.LastDeviceId = device.DeviceId;
        _settings.Save();
        SetStatus();
    }

    public async Task DisconnectAsync()
    {
        CaptureController? capture = _capture;
        capture?.ExitCapture(ReleaseReason.Shutdown);

        RemoteSession? session = _session;
        if (session is not null)
        {
            try
            {
                await session.CloseAsync(CancellationToken.None).ConfigureAwait(false);
            }
            catch (Exception)
            {
                // Closing politely is best effort; the release above already happened.
            }
        }

        _sessionCts?.Cancel();
        if (_sessionTask is not null)
        {
            try
            {
                await _sessionTask.ConfigureAwait(false);
            }
            catch (Exception)
            {
                // Already reported.
            }
        }

        if (session is not null)
            await session.DisposeAsync().ConfigureAwait(false);

        _sessionCts?.Dispose();
        _sessionCts = null;
        _sessionTask = null;
        _session = null;
        _capture = null;
        _logic = null;
        SetStatus();
    }

    public async Task RequestControlAsync()
    {
        RemoteSession? session = _session;
        if (session is null)
        {
            Notice?.Invoke("Connect to a device first.");
            return;
        }
        await session.RequestControlAsync(CancellationToken.None).ConfigureAwait(false);
        SetStatus();
    }

    public async Task ReleaseControlAsync()
    {
        _capture?.ExitCapture(ReleaseReason.CaptureToggledOff);
        RemoteSession? session = _session;
        if (session is not null)
            await session.ReleaseControlAsync(CancellationToken.None).ConfigureAwait(false);
        SetStatus();
    }

    public void ToggleCapture() => _capture?.Toggle();

    public bool SendShortcut(string commandId) => _capture?.SendShortcut(commandId) ?? false;

    // ---- type text -------------------------------------------------------------------------------

    /// <summary>
    /// Types text on the HOST as ordinary key presses (ADR-019). Cancellable at any moment; a cancel releases
    /// whatever was held mid-character.
    /// </summary>
    public async Task<TypeTextResult?> TypeTextAsync(string text)
    {
        SessionLogic? logic = _logic;
        RemoteSession? session = _session;
        if (logic is null || session is null || !logic.CanSendInput)
        {
            Notice?.Invoke("Typing text needs control of the host. Request control first.");
            return null;
        }

        var options = new TypeTextOptions
        {
            Layout = HostLayouts.ById(logic.Snapshot?.HostLayout ?? _settings.HostLayoutId),
            MessagesPerSecond = _settings.TypeTextMessagesPerSecond,
        };
        TypeTextPlan plan = TypeTextPlanner.Plan(text, options);

        _typeTextCts?.Cancel();
        _typeTextCts?.Dispose();
        _typeTextCts = new CancellationTokenSource();

        var run = new TypeTextRun(logic, frames =>
        {
            session.Post(frames);
            return Task.CompletedTask;
        });

        TypeTextResult result = await run
            .RunAsync(plan, options.MessagesPerSecond, _typeTextCts.Token)
            .ConfigureAwait(false);

        if (plan.UnmappedCharacterCount > 0)
        {
            Notice?.Invoke(
                $"{plan.UnmappedCharacterCount} character(s) are not available on the {plan.LayoutId} layout and "
                + "were not typed. Select the layout the host is using, or type those characters on the device.");
        }

        return result;
    }

    public void CancelTypeText() => _typeTextCts?.Cancel();

    // ---- capture plumbing ------------------------------------------------------------------------

    private void PostFrames(IReadOnlyList<byte[]> frames) => _session?.Post(frames);

    private CaptureDecision OnHookedKey(HookedKey key)
    {
        CaptureController? capture = _capture;
        if (capture is null)
            return CaptureDecision.PassThrough;
        return capture.OnKey(key.ScanCode, key.VirtualKey, key.Extended, key.Down);
    }

    private void OnRawMouse(RawMouseEvent mouse)
    {
        CaptureController? capture = _capture;
        if (capture is null || !capture.IsCapturing)
            return;

        if (mouse.Dx != 0 || mouse.Dy != 0)
            capture.OnMouseMove(mouse.Dx, mouse.Dy);

        ushort flags = mouse.ButtonFlags;
        if ((flags & NativeMethods.RI_MOUSE_LEFT_BUTTON_DOWN) != 0)
            capture.OnMouseButton(Core.Protocol.MouseButtons.Left, true);
        if ((flags & NativeMethods.RI_MOUSE_LEFT_BUTTON_UP) != 0)
            capture.OnMouseButton(Core.Protocol.MouseButtons.Left, false);
        if ((flags & NativeMethods.RI_MOUSE_RIGHT_BUTTON_DOWN) != 0)
            capture.OnMouseButton(Core.Protocol.MouseButtons.Right, true);
        if ((flags & NativeMethods.RI_MOUSE_RIGHT_BUTTON_UP) != 0)
            capture.OnMouseButton(Core.Protocol.MouseButtons.Right, false);
        if ((flags & NativeMethods.RI_MOUSE_MIDDLE_BUTTON_DOWN) != 0)
            capture.OnMouseButton(Core.Protocol.MouseButtons.Middle, true);
        if ((flags & NativeMethods.RI_MOUSE_MIDDLE_BUTTON_UP) != 0)
            capture.OnMouseButton(Core.Protocol.MouseButtons.Middle, false);

        if ((flags & NativeMethods.RI_MOUSE_WHEEL) != 0)
            capture.OnMouseWheel(NormalizeWheel(mouse.WheelDelta), 0);
        if ((flags & NativeMethods.RI_MOUSE_HWHEEL) != 0)
            capture.OnMouseWheel(0, NormalizeWheel(mouse.WheelDelta));
    }

    /// <summary>Windows reports 120 units per notch; the protocol carries notches.</summary>
    private static int NormalizeWheel(short delta)
    {
        int notches = delta / 120;
        if (notches == 0 && delta != 0)
            notches = delta > 0 ? 1 : -1;
        return notches;
    }

    private const string LoopGuardNotice =
        "A KVMpad is plugged into this computer by USB. Controlling it from the same computer would send its own keys "
        + "and mouse movements back to it in a loop, so capture stays off. Use the client on another computer, or "
        + "unplug the KVMpad from this one.";

    private void OnCaptureChanged(bool capturing)
    {
        if (capturing)
        {
            // Feedback-loop guard (KvmpadUsbIdentity): a keystroke the KVMpad types on this machine would reach the
            // keyboard hook as local input and be forwarded again. The hook cannot tell devices apart, so capture is
            // refused while any KVMpad is attached here.
            if (SystemEventWindow.IsKvmpadAttached())
            {
                Notice?.Invoke(LoopGuardNotice);
                _capture?.ExitCapture(ReleaseReason.LoopGuard);
                return;
            }

            try
            {
                _hook.Install();
            }
            catch (System.ComponentModel.Win32Exception ex)
            {
                Notice?.Invoke("Could not install the keyboard hook: " + ex.Message);
                _capture?.ExitCapture(ReleaseReason.Shutdown);
                return;
            }

            if (!_events.EnableRawMouse())
                Notice?.Invoke("Could not register for raw mouse input; mouse control is unavailable this session.");
        }
        else
        {
            _hook.Remove();
            _events.DisableRawMouse();
        }

        SetStatus();
    }

    private void OnSessionStateChanged(ClientSessionState state)
    {
        _capture?.OnSessionStateChanged(state);
        SetStatus();
    }

    private void OnSessionError(string code, bool retryable)
    {
        Notice?.Invoke("The device reported " + code + ".");
        SetStatus();
    }

    private void OnSessionChanged() => _capture?.OnSessionChanged();

    private void OnSuspend() => _capture?.OnSuspend();

    /// <summary>A KVMpad was plugged into this machine while capturing: stop before the first key loops.</summary>
    private void OnKvmpadArrived()
    {
        CaptureController? capture = _capture;
        if (capture is null || !capture.IsCapturing)
            return;
        Notice?.Invoke(LoopGuardNotice);
        capture.ExitCapture(ReleaseReason.LoopGuard);
    }

    /// <summary>The client's own window gained or lost activation.</summary>
    public void OnActivationChanged(bool active) => _capture?.OnActivationChanged(active);

    /// <summary>
    /// Builds the status line from the device's snapshot, never from the client's own optimism: "Connected"
    /// appears only when the device says <c>CLIENT_CONNECTED</c> (AT-CONN-04, AGENTS-UI §5).
    /// </summary>
    private void SetStatus()
    {
        SessionLogic? logic = _logic;
        if (logic is null)
        {
            StatusLine = "Not connected";
            Changed?.Invoke();
            return;
        }

        string line = logic.State switch
        {
            ClientSessionState.Disconnected => "Disconnected",
            ClientSessionState.Handshaking => "Authenticating",
            ClientSessionState.Connected => "Connected (no control)",
            ClientSessionState.ControlPending => "Waiting for approval on the device",
            ClientSessionState.Controlling => "Controlling",
            _ => "Unknown",
        };

        Core.Protocol.StatusSnapshot? snapshot = logic.Snapshot;
        if (snapshot is not null)
        {
            line += "  ·  host " + snapshot.HostState;
            if (snapshot.DeviceWifi.Length > 0)
                line += "  ·  wifi " + snapshot.DeviceWifi;
            // ADR-023 FR-735: a relaxed approval policy must be visible, so the user always knows a remote
            // client can take over without being asked.
            if (snapshot.ControlApproval is { Length: > 0 } approval)
                line += "  ·  approval " + approval;
        }

        if (IsCapturing)
            line += "  ·  CAPTURING (" + _settings.ToggleKeyName + " to stop)";

        if (logic.LastErrorCode is { Length: > 0 } error)
            line += "  ·  last error " + error;

        StatusLine = line;
        Changed?.Invoke();
    }

    public void Dispose()
    {
        try
        {
            DisconnectAsync().GetAwaiter().GetResult();
        }
        catch (Exception)
        {
            // Shutting down.
        }
        _hook.Dispose();
        _events.Dispose();
        _typeTextCts?.Dispose();
    }
}
