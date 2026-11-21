// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;
using KVMpad.Client.Core.Display;
using KVMpad.Client.Core.Session;
using Timer = System.Windows.Forms.Timer;

namespace KVMpad.Client.App;

internal sealed class UsbDisplayDialog : Form
{
    private readonly UsbMonitorDisplayService _service = new();
    private readonly Label _device = new() { AutoSize = true };
    private readonly Label _current = new() { AutoSize = true };
    private readonly Label _status = new() { AutoSize = true, Dock = DockStyle.Top, MaximumSize = new Size(410, 0) };
    private readonly ComboBox _modes = new() { DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Fill,
                                              AccessibleName = "Desktop resolution", Enabled = false };
    private readonly Button _apply = new() { Text = "Apply", AutoSize = true, Enabled = false };
    private readonly Button _refresh = new() { Text = "Refresh", AutoSize = true };
    private readonly LinkLabel _settings = new() { Text = "Windows display settings", AutoSize = true, Anchor = AnchorStyles.Left };
    private readonly Timer _topologyChanged = new() { Interval = 400 };
    private UsbMonitorDisplay? _display;
    private readonly SessionLogic? _session;
    private readonly CancellationTokenSource _closed = new();
    private bool _busy;

    public UsbDisplayDialog(SessionLogic? session)
    {
        _session = session;
        Text = "KVMpad USB display";
        StartPosition = FormStartPosition.CenterParent;
        MinimizeBox = MaximizeBox = false;
        ShowInTaskbar = false;
        AutoScaleMode = AutoScaleMode.Dpi;
        ClientSize = new Size(460, 300);
        MinimumSize = new Size(400, 315);
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 7, Padding = new Padding(16) };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        for (int row = 0; row < 7; row++)
            layout.RowStyles.Add(row == 5 ? new RowStyle(SizeType.Percent, 100) : new RowStyle(SizeType.AutoSize));
        layout.Controls.Add(_device, 0, 0);
        layout.Controls.Add(_current, 0, 1);
        layout.Controls.Add(new Label { Text = "Desktop resolution", AutoSize = true, Margin = new Padding(3, 16, 3, 3) }, 0, 2);
        layout.Controls.Add(_modes, 0, 3);
        layout.Controls.Add(_status, 0, 4);

        _settings.LinkClicked += (_, _) =>
        {
            try { Process.Start(new ProcessStartInfo("ms-settings:display") { UseShellExecute = true }); }
            catch (Win32Exception e) { _status.Text = e.Message; }
        };
        layout.Controls.Add(_settings, 0, 5);
        var close = new Button { Text = "Close", DialogResult = DialogResult.Cancel, AutoSize = true };
        var buttons = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, FlowDirection = FlowDirection.RightToLeft };
        buttons.Controls.AddRange([close, _apply, _refresh]);
        foreach (Button button in buttons.Controls) {
            button.FlatStyle = FlatStyle.Flat;
            button.UseVisualStyleBackColor = false;
        }
        UpdateLinkColors();
        layout.Controls.Add(buttons, 0, 6);
        Controls.Add(layout);
        CancelButton = close;
        _modes.SelectedIndexChanged += (_, _) => UpdateApply();
        _refresh.Click += async (_, _) => await ReadOrApply();
        _apply.Click += async (_, _) => await ReadOrApply(_modes.SelectedItem as DisplayResolution?);
        Shown += async (_, _) => await ReadOrApply();
        _topologyChanged.Tick += async (_, _) =>
        {
            if (_busy) return;
            _topologyChanged.Stop();
            await ReadOrApply();
        };
    }

    private async Task ReadOrApply(DisplayResolution? requested = null)
    {
        if (_busy || IsDisposed || Disposing) return;
        _busy = true;
        _modes.Enabled = _apply.Enabled = _refresh.Enabled = false;
        var previous = _display;
        _status.Text = requested.HasValue ? "Applying resolution…" : "Checking the USB display…";
        try
        {
            var token = _closed.Token;
            var found = await Task.Run(async () => {
                if (requested is not { } mode || previous == null) return _service.Read();
                if (CanSwitch(previous)) return await _service.ApplyProfileAsync(previous, mode, _session!.RequestDisplayProfileAsync, token);
                return _service.Apply(previous, mode);
            }, token);
            if (IsDisposed || Disposing) return;
            ShowDisplay(found);
            _status.Text = found.UnavailableSignalMode ?? (found.Signal is { } signal && signal != found.Current
                ? "Match the Windows desktop to the USB signal to use the full resolution."
                : CanSwitch(found) ? "Changing the USB signal briefly reconnects this display."
                : found.Modes.Count == 0 ? "Windows currently offers no compatible desktop resolutions."
                : found.Modes.Count == 1 ? "Connect the paired CLIENT to select another USB signal size." : "");
        }
        catch (OperationCanceledException) when (IsDisposed || Disposing || _closed.IsCancellationRequested) { }
        catch (Exception e) when (e is InvalidOperationException or TimeoutException)
        {
            if (IsDisposed || Disposing) return;
            _display = null;
            _device.Text = "USB display";
            _current.Text = "";
            _modes.Items.Clear();
            _status.Text = e.Message;
            if (requested.HasValue)
            {
                try
                {
                    var actual = await Task.Run(_service.Read);
                    if (!IsDisposed && !Disposing)
                    {
                        _topologyChanged.Stop();
                        ShowDisplay(actual);
                    }
                }
                catch (InvalidOperationException) { /* Refresh can retry after reconnect. */ }
            }
        }
        finally
        {
            _busy = false;
            if (!IsDisposed && !Disposing) { _refresh.Enabled = true; UpdateApply(); }
        }
    }

    private bool CanSwitch(UsbMonitorDisplay display) =>
        _session?.State == ClientSessionState.Connected && _session.DisplaySerial == display.Serial;

    private void ShowDisplay(UsbMonitorDisplay found)
    {
        _display = found;
        _device.Text = $"KVMpad  ·  {found.Serial}";
        _current.Text = found.Signal is { } signal && signal != found.Current
            ? $"Desktop  {found.Current}     USB signal  {signal}" : $"Desktop  {found.Current}";
        _modes.Items.Clear();
        foreach (var available in UsbMonitorDisplayService.Choices(found, CanSwitch(found))) _modes.Items.Add(available);
        _modes.SelectedItem = found.Current;
        if (found.Signal != found.Current && found.Signal is { } signalSize && found.Modes.Contains(signalSize))
            _modes.SelectedItem = signalSize;
        if (_modes.SelectedIndex < 0 && _modes.Items.Count == 1) _modes.SelectedIndex = 0;
        _modes.Enabled = _modes.Items.Count > 0;
    }

    private void UpdateApply()
    {
        var choice = _modes.SelectedItem as DisplayResolution?;
        _apply.Text = _display != null && choice.HasValue && choice == _display.Signal && choice != _display.Current
            ? $"Use {choice}" : "Apply";
        _apply.Enabled = !_busy && _display != null && choice.HasValue &&
            (choice != _display.Current || (CanSwitch(_display) && choice != _display.Signal));
    }

    private void UpdateLinkColors()
    {
#pragma warning disable WFO5001
        bool dark = Application.IsDarkModeEnabled && !SystemInformation.HighContrast;
#pragma warning restore WFO5001
        _settings.LinkColor = dark ? Color.LightSkyBlue : SystemColors.HotTrack;
        _settings.ActiveLinkColor = dark ? Color.White : SystemColors.WindowText;
        _settings.VisitedLinkColor = _settings.LinkColor;
    }

    protected override void WndProc(ref Message m)
    {
        base.WndProc(ref m);
        if ((m.Msg is 0x001A or 0x031A) && IsHandleCreated && !Disposing)
            UpdateLinkColors();
        const int WmDisplayChange = 0x007e;
        if (m.Msg == WmDisplayChange && Visible && !Disposing && !IsDisposed)
        {
            _topologyChanged.Stop();
            _topologyChanged.Start();
        }
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing && !IsDisposed) { _closed.Cancel(); _closed.Dispose(); _topologyChanged.Dispose(); }
        base.Dispose(disposing);
    }
}
