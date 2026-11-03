// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.Text.Json;
using KVMpad.Client.Core.Input;
using KVMpad.Client.Core.Protocol;

namespace KVMpad.Client.Core.Session;

/// <summary>Where the client is in the protocol v1 handshake and control flow.</summary>
public enum ClientSessionState
{
    /// <summary>No transport. Nothing may be sent and nothing held down is believed to be held.</summary>
    Disconnected,

    /// <summary><c>hello</c> is out, <c>hello.ack</c> has not come back.</summary>
    Handshaking,

    /// <summary>
    /// The device confirmed an authenticated session. Status is readable; input is not. Reaching this state is
    /// the only thing that may be shown as "Connected" (AT-CONN-04).
    /// </summary>
    Connected,

    /// <summary><c>control.request</c> is out, waiting for physical approval on the device (PROC-006 step 5).</summary>
    ControlPending,

    /// <summary>The lease is ours and <see cref="SessionLogic.LeaseEpoch"/> is set. Input may be sent.</summary>
    Controlling,
}

/// <summary>Why a session stopped being usable, for the status line and the reconnect decision.</summary>
public enum SessionEndReason
{
    None,
    TransportClosed,
    HeartbeatStale,
    ProtocolError,
    ClosedByClient,
}

/// <summary>Settings that do not change inside a session.</summary>
public sealed class SessionOptions
{
    /// <summary>The client name the device shows. Defaults to the Windows host name, nothing typed (FR-724).</summary>
    public string ClientName { get; init; } = "KVMpad client";

    /// <summary>Capabilities advertised in <c>hello</c> (03 §6.3).</summary>
    public string[] Capabilities { get; init; } = ["hid_v1"];

    /// <summary>The <c>reason</c> sent with <c>control.request</c>.</summary>
    public string ControlReason { get; init; } = "remote_support";

    /// <summary>The client's self-imposed input ceiling, below the device's 120/s (03 §6.5).</summary>
    public int InputMessagesPerSecond { get; init; } = ProtocolLimits.ClientInputMessagesPerSecond;
}

/// <summary>
/// The protocol v1 client state machine, with no I/O and no threads of its own: it takes parsed inbound
/// messages and clock ticks, and hands back encoded frames to send.
/// </summary>
/// <remarks>
/// <para>
/// Keeping the decisions here, separate from the socket in <see cref="RemoteSession"/>, is what makes the
/// safety-critical rules testable at all: sequence monotonicity, the ban on replaying an event from a previous
/// session, the heartbeat deadline, and the release of every held key the moment the lease or the link goes
/// away (03 §6.2, §6.4, §6.5, AGENTS §2.3).
/// </para>
/// <para>
/// Every mutating method takes one lock. Two threads legitimately drive this object — the keyboard hook,
/// which runs on the message loop and must return in microseconds, and the session's own pump — and the
/// sequence counter, the ledger and the rate limiter would all be corrupted by an unguarded interleaving.
/// The lock is held only for bookkeeping and encoding, never across I/O.
/// </para>
/// </remarks>
public sealed class SessionLogic
{
    private readonly object _gate = new();
    private readonly IClock _clock;
    private readonly SessionOptions _options;
    private readonly CaptureLedger _ledger;
    private readonly SequenceCounter _seq = new();
    private readonly InputRateLimiter _rateLimiter;

    private TimeSpan _lastPingSent = TimeSpan.MinValue;
    private TimeSpan _lastInboundAt = TimeSpan.MinValue;
    private TimeSpan _controlRequestedAt = TimeSpan.MinValue;
    private long _pingNonce;
    private long? _outstandingPingNonce;

    public SessionLogic(IClock clock, SessionOptions options, CaptureLedger ledger)
    {
        _clock = clock ?? throw new ArgumentNullException(nameof(clock));
        _options = options ?? throw new ArgumentNullException(nameof(options));
        _ledger = ledger ?? throw new ArgumentNullException(nameof(ledger));
        _rateLimiter = new InputRateLimiter(clock, options.InputMessagesPerSecond);
    }

    public ClientSessionState State { get; private set; } = ClientSessionState.Disconnected;

    /// <summary>The device's non-secret session label from <c>hello.ack</c>. Never used as authorization (03 §6.2).</summary>
    public string? SessionId { get; private set; }

    /// <summary>The lease epoch from <c>control.granted</c>. Null means no input may be sent.</summary>
    public uint? LeaseEpoch { get; private set; }

    /// <summary>The last snapshot the device sent. The UI shows this, not its own guesses (AGENTS-UI §5).</summary>
    public StatusSnapshot? Snapshot { get; private set; }

    /// <summary>The device's capabilities from <c>hello.ack</c>.</summary>
    public IReadOnlyList<string> DeviceCapabilities { get; private set; } = Array.Empty<string>();

    public string? DeviceId { get; private set; }

    /// <summary>The last error code the device sent, for the status line.</summary>
    public string? LastErrorCode { get; private set; }

    public SessionEndReason EndReason { get; private set; } = SessionEndReason.None;

    /// <summary>The ledger of keys and buttons this client holds down.</summary>
    public CaptureLedger Ledger => _ledger;

    public InputRateLimiter RateLimiter => _rateLimiter;

    public SequenceCounter Sequence => _seq;

    /// <summary>True only when the lease is held, so the capture layer knows whether input may flow.</summary>
    public bool CanSendInput => State == ClientSessionState.Controlling && LeaseEpoch.HasValue;

    /// <summary>Raised when everything held down has been let go of, with the reason.</summary>
    public event Action<ReleaseAllResult>? Released;

    /// <summary>Raised when the state changes, so the UI can follow without polling.</summary>
    public event Action<ClientSessionState>? StateChanged;

    /// <summary>Raised for every device <c>error</c> envelope, and for an unknown envelope type.</summary>
    public event Action<string, bool>? ErrorReceived;

    /// <summary>
    /// A fresh transport has opened. The sequence restarts at 1 and every event queued under the previous
    /// session is discarded: re-sending an old event after a reconnect is forbidden (03 §6.4).
    /// </summary>
    public IReadOnlyList<byte[]> OnTransportOpened()
    {
        lock (_gate)
        {
            _seq.ResetForNewSession();
            _rateLimiter.DiscardPending();
            _ledger.Forget();
            SessionId = null;
            LeaseEpoch = null;
            Snapshot = null;
            LastErrorCode = null;
            EndReason = SessionEndReason.None;
            _outstandingPingNonce = null;
            _lastInboundAt = _clock.Elapsed;
            _lastPingSent = _clock.Elapsed;
            SetState(ClientSessionState.Handshaking);

            return
            [
                Encode(MessageTypes.Hello, null, new HelloPayload(_options.ClientName, _options.Capabilities)),
                Encode(MessageTypes.StatusGet, null, new EmptyPayload()),
            ];
        }
    }

    /// <summary>
    /// The transport went away. Nothing can be sent, so held keys are forgotten rather than released on the
    /// wire; the device runs <c>RELEASE_ALL</c> itself on loss of session (03 §4.2, VEC-005).
    /// </summary>
    public void OnTransportClosed(SessionEndReason reason = SessionEndReason.TransportClosed)
    {
        lock (_gate)
        {
            LeaseEpoch = null;
            SessionId = null;
            _rateLimiter.DiscardPending();
            _outstandingPingNonce = null;
            EndReason = reason;
            ReleaseLedger(ReleaseReason.ConnectionLost);
            SetState(ClientSessionState.Disconnected);
        }
    }

    /// <summary>Handles one parsed inbound envelope and returns whatever must be sent in response.</summary>
    public IReadOnlyList<byte[]> OnMessage(ServerMessage message)
    {
        if (message is null)
            throw new ArgumentNullException(nameof(message));

        lock (_gate)
        {
            _lastInboundAt = _clock.Elapsed;

            switch (message.Type)
            {
                case MessageTypes.HelloAck:
                    SessionId = message.SessionId;
                    DeviceId = message.PayloadString("device_id");
                    DeviceCapabilities = ReadCapabilities(message);
                    SetState(ClientSessionState.Connected);
                    return Array.Empty<byte[]>();

                case MessageTypes.StatusSnapshot:
                    Snapshot = StatusSnapshot.Parse(message);
                    return Array.Empty<byte[]>();

                case MessageTypes.ControlPending:
                    _controlRequestedAt = _clock.Elapsed;
                    SetState(ClientSessionState.ControlPending);
                    return Array.Empty<byte[]>();

                case MessageTypes.ControlGranted:
                    if (!message.LeaseEpoch.HasValue)
                        return Array.Empty<byte[]>();
                    LeaseEpoch = message.LeaseEpoch;
                    SetState(ClientSessionState.Controlling);
                    return Array.Empty<byte[]>();

                case MessageTypes.ControlRevoked:
                    // Local priority, a timeout or a Remove on the device: the lease is gone, so the client
                    // drops its epoch and stops believing it holds anything (ADR-011, TECH-044).
                    LeaseEpoch = null;
                    _rateLimiter.DiscardPending();
                    ReleaseLedger(ReleaseReason.ControlRevoked);
                    if (State != ClientSessionState.Disconnected)
                        SetState(ClientSessionState.Connected);
                    return Array.Empty<byte[]>();

                case MessageTypes.Pong:
                    long? nonce = message.PayloadInt64("nonce");
                    if (!nonce.HasValue || (_outstandingPingNonce.HasValue && nonce.Value == _outstandingPingNonce.Value))
                        _outstandingPingNonce = null;
                    return Array.Empty<byte[]>();

                case MessageTypes.InputAck:
                    return Array.Empty<byte[]>();

                case MessageTypes.Error:
                    return OnError(message);

                default:
                    // 03 §6.2: an unknown type is not ignored silently. It is recorded; the connection stays
                    // up only because an unknown device-to-client type cannot by itself authorize anything.
                    LastErrorCode = "ERR_UNKNOWN_TYPE:" + message.Type;
                    ErrorReceived?.Invoke(LastErrorCode, false);
                    return Array.Empty<byte[]>();
            }
        }
    }

    private IReadOnlyList<byte[]> OnError(ServerMessage message)
    {
        string code = message.ErrorCode ?? ErrorCodes.BadFrame;
        LastErrorCode = code;
        ErrorReceived?.Invoke(code, message.ErrorRetryable);

        if (ErrorCodes.RequiresLocalRelease(code))
        {
            LeaseEpoch = null;
            _rateLimiter.DiscardPending();
            ReleaseLedger(ReleaseReason.ControlRevoked);
            if (State is ClientSessionState.Controlling or ClientSessionState.ControlPending)
                SetState(ClientSessionState.Connected);
        }

        if (ErrorCodes.IsFatalForConnection(code))
        {
            EndReason = SessionEndReason.ProtocolError;
            SetState(ClientSessionState.Disconnected);
        }

        return Array.Empty<byte[]>();
    }

    /// <summary>
    /// Clock-driven work: the heartbeat, the stale check and the control-approval timeout. The caller invokes
    /// it often — every ~50 ms is plenty — and sends whatever comes back.
    /// </summary>
    public IReadOnlyList<byte[]> OnTick()
    {
        lock (_gate)
        {
            if (State == ClientSessionState.Disconnected)
                return Array.Empty<byte[]>();

            TimeSpan now = _clock.Elapsed;

            if (_outstandingPingNonce.HasValue && now - _lastInboundAt >= ProtocolLimits.PongTimeout)
            {
                // The device revokes a stale session after 3 s and zeroes its pressed ledger (AT-CONN-08).
                // The deadline is measured from the last inbound frame, not from the last ping, so the client
                // always reaches the conclusion first and stops pretending to hold anything.
                LeaseEpoch = null;
                SessionId = null;
                _rateLimiter.DiscardPending();
                _outstandingPingNonce = null;
                EndReason = SessionEndReason.HeartbeatStale;
                ReleaseLedger(ReleaseReason.ConnectionLost);
                SetState(ClientSessionState.Disconnected);
                return Array.Empty<byte[]>();
            }

            if (State == ClientSessionState.ControlPending
                && _controlRequestedAt != TimeSpan.MinValue
                && now - _controlRequestedAt > ProtocolLimits.ControlApprovalTimeout)
            {
                LastErrorCode = ErrorCodes.ControlDenied;
                ErrorReceived?.Invoke(ErrorCodes.ControlDenied, false);
                SetState(ClientSessionState.Connected);
            }

            if (now - _lastPingSent >= ProtocolLimits.PingInterval)
            {
                _lastPingSent = now;
                long nonce = ++_pingNonce;
                _outstandingPingNonce = nonce;
                return [Encode(MessageTypes.Ping, null, new PingPayload(nonce))];
            }

            return Array.Empty<byte[]>();
        }
    }

    /// <summary>Asks for the lease. The device may need a physical tap first (PROC-006 step 5).</summary>
    public byte[]? BuildControlRequest()
    {
        lock (_gate)
        {
            if (State != ClientSessionState.Connected)
                return null;
            _controlRequestedAt = _clock.Elapsed;
            return Encode(MessageTypes.ControlRequest, null, new ControlRequestPayload(_options.ControlReason));
        }
    }

    /// <summary>
    /// Gives the lease back. Everything held down is released first, in the same frame batch, so the host never
    /// sees a key that outlives the lease.
    /// </summary>
    public IReadOnlyList<byte[]> BuildControlRelease()
    {
        lock (_gate)
        {
            if (State is not (ClientSessionState.Controlling or ClientSessionState.ControlPending))
                return Array.Empty<byte[]>();

            var frames = new List<byte[]>();
            frames.AddRange(BuildReleaseAllFrames(ReleaseReason.CaptureToggledOff));
            frames.Add(Encode(MessageTypes.ControlRelease, null, new EmptyPayload()));
            LeaseEpoch = null;
            _rateLimiter.DiscardPending();
            SetState(ClientSessionState.Connected);
            return frames;
        }
    }

    /// <summary>Asks for a fresh snapshot (03 §6.3).</summary>
    public byte[]? BuildStatusGet()
    {
        lock (_gate)
        {
            return State == ClientSessionState.Disconnected
                ? null
                : Encode(MessageTypes.StatusGet, null, new EmptyPayload());
        }
    }

    /// <summary>Ends the session politely, releasing everything first.</summary>
    public IReadOnlyList<byte[]> BuildSessionClose()
    {
        lock (_gate)
        {
            var frames = new List<byte[]>();
            frames.AddRange(BuildReleaseAllFrames(ReleaseReason.Shutdown));
            if (State != ClientSessionState.Disconnected)
                frames.Add(Encode(MessageTypes.SessionClose, null, new EmptyPayload()));
            EndReason = SessionEndReason.ClosedByClient;
            return frames;
        }
    }

    /// <summary>
    /// Queues relative mouse motion. Refused when the lease is not held, because without an epoch the device
    /// would answer <c>ERR_STALE_EPOCH</c> and send no HID anyway.
    /// </summary>
    public bool QueueMouseMove(int dx, int dy)
    {
        lock (_gate)
        {
            return CanSendInput && _rateLimiter.EnqueueMove(dx, dy);
        }
    }

    /// <summary>
    /// Queues an event that must arrive exactly once, updating the ledger for keys and buttons so that a
    /// release-all can balance it. Returns false when the event was not accepted — an auto-repeat, an UP
    /// without a DOWN, no lease, or back pressure.
    /// </summary>
    public bool QueueInput(IInputPayload payload)
    {
        if (payload is null)
            throw new ArgumentNullException(nameof(payload));

        lock (_gate)
        {
            if (!CanSendInput)
                return false;

            switch (payload)
            {
                case KeyboardKeyPayload key:
                    if (!_ledger.OnKey(key.UsageId, key.Down))
                        return false;
                    break;
                case MouseButtonPayload button:
                    if (!_ledger.OnMouseButton(button.Button, button.Down))
                        return false;
                    break;
            }

            if (_rateLimiter.Enqueue(payload))
                return true;

            // The queue is full: the only safe answer is to stop holding things down, not to discard the
            // event, so the ledger is released and the caller learns the event did not take.
            ReleaseLedger(ReleaseReason.CaptureToggledOff);
            return false;
        }
    }

    /// <summary>Encodes as much queued input as the rate budget allows (03 §6.5).</summary>
    public IReadOnlyList<byte[]> DrainInput()
    {
        lock (_gate)
        {
            if (!CanSendInput)
                return Array.Empty<byte[]>();

            IReadOnlyList<IInputPayload> payloads = _rateLimiter.Drain();
            if (payloads.Count == 0)
                return Array.Empty<byte[]>();

            var frames = new List<byte[]>(payloads.Count);
            foreach (IInputPayload payload in payloads)
                frames.Add(Encode(MessageTypes.Input, LeaseEpoch, payload));
            return frames;
        }
    }

    /// <summary>
    /// Releases every held key and button. The frames come back when the lease and the link allow it; the
    /// ledger is emptied either way, so nothing can stay stuck in the client's own belief about the host.
    /// </summary>
    public IReadOnlyList<byte[]> BuildReleaseAllFrames(ReleaseReason reason)
    {
        lock (_gate)
        {
            bool canTransmit = ReleaseAllResult.RequiresTransmit(reason) && CanSendInput;
            ReleaseAllResult result = _ledger.ReleaseAll(reason);

            List<byte[]>? frames = null;
            if (canTransmit && result.Events.Count > 0)
            {
                frames = new List<byte[]>(result.Events.Count);
                foreach (ReleaseEvent release in result.Events)
                {
                    IClientPayload payload = release.IsKey
                        ? new KeyboardKeyPayload(release.KeyUsage, false)
                        : new MouseButtonPayload(release.Button!, false);
                    frames.Add(Encode(MessageTypes.Input, LeaseEpoch, payload));
                }
            }

            Released?.Invoke(result);
            return frames ?? (IReadOnlyList<byte[]>)Array.Empty<byte[]>();
        }
    }

    /// <summary>Empties the ledger without producing frames, for the paths where nothing can be sent.</summary>
    private void ReleaseLedger(ReleaseReason reason)
    {
        ReleaseAllResult result = _ledger.ReleaseAll(reason);
        Released?.Invoke(result);
    }

    private byte[] Encode(string type, uint? leaseEpoch, IClientPayload payload)
        => ProtocolCodec.Encode(type, _seq.Next(), SessionId, leaseEpoch, payload);

    private void SetState(ClientSessionState state)
    {
        if (State == state)
            return;
        State = state;
        StateChanged?.Invoke(state);
    }

    private static string[] ReadCapabilities(ServerMessage message)
    {
        if (!message.Payload.HasValue
            || message.Payload.Value.ValueKind != JsonValueKind.Object
            || !message.Payload.Value.TryGetProperty("capabilities", out JsonElement element)
            || element.ValueKind != JsonValueKind.Array)
            return [];

        var capabilities = new List<string>();
        foreach (JsonElement item in element.EnumerateArray())
        {
            if (item.ValueKind == JsonValueKind.String)
                capabilities.Add(item.GetString()!);
        }
        return capabilities.ToArray();
    }
}
