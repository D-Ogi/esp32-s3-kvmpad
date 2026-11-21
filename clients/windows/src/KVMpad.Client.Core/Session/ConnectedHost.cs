// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
using System;
using System.Threading;
using System.Threading.Tasks;

namespace KVMpad.Client.Core.Session;

/// <summary>Owns the GUI's optional HOST process for one authenticated device connection.</summary>
public sealed class ConnectedHost
{
    private readonly SessionLogic _session;
    private readonly Func<Action, CancellationToken, Task> _run;
    private readonly Action _changed;
    private readonly object _gate = new();
    private CancellationTokenSource? _lifetime;
    private Task _task = Task.CompletedTask;
    private bool _attempted, _closed;
    private string? _status;

    public ConnectedHost(SessionLogic session, Func<Action, CancellationToken, Task> run, Action changed)
    {
        _session = session;
        _run = run;
        _changed = changed;
        session.StateChanged += OnState;
    }

    public bool Running { get { lock (_gate) return _lifetime != null; } }
    public string? Status { get { lock (_gate) return _status; } }

    private void OnState(ClientSessionState state)
    {
        if (state == ClientSessionState.Connected) _ = StartAsync(automatic: true);
        if (state == ClientSessionState.Disconnected) _ = CloseAsync();
    }

    public Task StartAsync() => StartAsync(automatic: false);

    private Task StartAsync(bool automatic)
    {
        Task task;
        lock (_gate)
        {
            if (_closed || !_session.HasAuthenticatedSession || _lifetime != null || (automatic && _attempted))
                return _task;
            _attempted = true;
            var lifetime = new CancellationTokenSource();
            _lifetime = lifetime;
            _status = "Starting HOST";
            // StateChanged runs inside the protocol receive loop, which must deliver host.attached.
            task = _task = Task.Run(() => RunAsync(lifetime));
        }
        _changed();
        return task;
    }

    private async Task RunAsync(CancellationTokenSource lifetime)
    {
        string? failure = null;
        try
        {
            lifetime.Token.ThrowIfCancellationRequested();
            await _run(() => {
                lock (_gate)
                {
                    if (lifetime.IsCancellationRequested) return;
                    _status = "HOST ready";
                }
                _changed();
            }, lifetime.Token).ConfigureAwait(false);
        }
        catch (Exception ex) { failure = HostDiagnostics.Describe(ex); }
        finally
        {
            lock (_gate)
            {
                _status = _closed ? null : lifetime.IsCancellationRequested || failure == null
                    ? "HOST stopped" : "HOST unavailable · " + failure;
                _lifetime = null;
                lifetime.Dispose();
            }
            _changed();
        }
    }

    public void Stop()
    {
        lock (_gate) _lifetime?.Cancel();
    }

    public Task CloseAsync()
    {
        lock (_gate)
        {
            _closed = true;
            _session.StateChanged -= OnState;
            _status = null;
            _lifetime?.Cancel();
            return _task;
        }
    }
}
