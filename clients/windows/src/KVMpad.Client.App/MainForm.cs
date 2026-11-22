// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;
using KVMpad.Client.Core.Discovery;
using KVMpad.Client.Core.Input;
using KVMpad.Client.Core.Pairing;
using KVMpad.Client.Core.Storage;

namespace KVMpad.Client.App;

/// <summary>
/// The client's one window: the device list, the status the device reports, control and capture, a type-text
/// box and About.
/// </summary>
/// <remarks>
/// The window shows the device's own view of host state, client state and ownership. It never derives
/// "Connected" or "Controlling" from the fact that a socket is open: that distinction is what AT-CONN-04 and
/// AT-CONN-05 test, and getting it wrong would tell the user they have control when they do not.
/// </remarks>
internal sealed class MainForm : Form
{
    private readonly AppSettings _settings;
    private readonly ClientController _controller;
    private readonly ControlIcons _icons = new(Path.Combine(AppContext.BaseDirectory, "assets", "control-icons"));

    private readonly ListView _devices;
    private readonly TabControl _pages = new() { Dock = DockStyle.Fill, AccessibleName = "Client sections", Padding = new Point(14, 6) };
    private readonly Label _status;
    private readonly Button _discover;
    private readonly Button _pair;
    private readonly Button _connect;
    private readonly Button _advanced;
    private readonly Button _disconnect;
    private readonly Button _host;
    private readonly Button _requestControl;
    private readonly Button _ssh;
    private bool _preparingSsh;
    private readonly Button _releaseControl;
    private readonly Button _capture;
    private readonly TextBox _typeText;
    private readonly Button _typeSend;
    private readonly Button _typeCancel;
    private readonly ComboBox _shortcut;
    private readonly NotifyIcon _tray;

    private List<DiscoveredDevice> _discovered = new();

    public MainForm(AppSettings settings, ClientController controller)
    {
        _settings = settings;
        _controller = controller;

        Text = "KVMpad Client";
        ClientSize = new Size(760, 510);
        MinimumSize = new Size(480, 360);
        StartPosition = FormStartPosition.CenterScreen;

        _devices = new ListView
        {
            AccessibleName = "KVMpad devices",
            View = View.Details,
            FullRowSelect = true,
            MultiSelect = false,
            Dock = DockStyle.Fill,
            HideSelection = false,
        };
        _devices.Columns.Add("Device", 200);
        _devices.Columns.Add("Address", 160);
        _devices.Columns.Add("Paired", 70);
        _devices.Columns.Add("Pairing open", 90);
        _devices.Columns.Add("Protocol", 70);
        _devices.SizeChanged += (_, _) => {
            // Keep the endpoint and status fields visible as the compact desktop narrows.
            int available = _devices.ClientSize.Width - (_devices.Columns[2].Width +
                _devices.Columns[3].Width + _devices.Columns[4].Width) - LogicalToDeviceUnits(5);
            int address = Math.Max(LogicalToDeviceUnits(130), available * 4 / 9);
            int device = Math.Max(LogicalToDeviceUnits(100), available - address);
            if (_devices.Columns[0].Width != device) _devices.Columns[0].Width = device;
            if (_devices.Columns[1].Width != address) _devices.Columns[1].Width = address;
        };
        _devices.SelectedIndexChanged += async (_, _) => {
            if (_controller.ConnectedDeviceId is string connected && SelectedId() is string selected &&
                !string.Equals(selected, connected, StringComparison.OrdinalIgnoreCase))
                await _controller.DisconnectAsync();
            UpdateButtons();
        };

        _status = new Label
        {
            Dock = DockStyle.Fill,
            AutoSize = false,
            TextAlign = ContentAlignment.MiddleLeft,
            Padding = new Padding(8, 0, 0, 0),
            Height = 28,
            Text = _controller.StatusLine,
            AccessibleName = "Connection status",
        };

        _discover = MakeButton("Discover", async (_, _) => await DiscoverAsync().ConfigureAwait(true));
        _pair = MakeButton("Pair…", async (_, _) => await PairAsync().ConfigureAwait(true));
        _connect = MakeButton("Connect", async (_, _) => await ConnectAsync().ConfigureAwait(true));
        _disconnect = MakeButton("Disconnect", async (_, _) => await _controller.DisconnectAsync().ConfigureAwait(true));
        _host = MakeButton("Start HOST", async (_, _) => {
            if (_controller.HostRunning) _controller.StopHost();
            else if (SelectedId() is string id) await _controller.StartHostAsync(id);
        });
        _requestControl = MakeButton("Request control", async (_, _) => await _controller.RequestControlAsync().ConfigureAwait(true));
        _ssh = MakeButton("SSH command…", async (_, _) => await PrepareSshAsync());
        _releaseControl = MakeButton("Release control", async (_, _) => await _controller.ReleaseControlAsync().ConfigureAwait(true));
        _capture = MakeButton("Start capture", (_, _) => _controller.ToggleCapture());
        _advanced = MakeButton("Advanced…", async (_, _) => await ShowAdvancedAsync().ConfigureAwait(true));
        var studio = MakeButton("Extension Studio…", async (_, _) => await LaunchToolAsync(
            "studio", "extension_studio.py", "Extension Studio requires Python 3 with Tk, jsonschema and Pillow."));
        var video = MakeButton("Prepare card video…", async (_, _) => await LaunchToolAsync(
            "media", "card_video_import.py", "Card video preparation requires Python 3.11 or newer with Tk."));
        var display = MakeButton("USB display…", (_, _) =>
        {
            using var dialog = new UsbDisplayDialog(_controller.Logic);
            dialog.ShowDialog(this);
        });
        var about = MakeButton("About", (_, _) =>
        {
            using var dialog = new AboutForm();
            dialog.ShowDialog(this);
        });

        _shortcut = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 200 };
        foreach (string id in ChordIds.All)
            _shortcut.Items.Add(new ShortcutItem(id));
        _shortcut.SelectedIndex = 0;
        var shortcutSend = MakeButton("Send shortcut", (_, _) =>
        {
            if (_shortcut.SelectedItem is ShortcutItem item && !_controller.SendShortcut(item.Id))
                ShowNotice("The shortcut was not sent: control of the host is needed first.");
        });

        _typeText = new TextBox
        {
            AccessibleName = "Text to type on the host",
            Multiline = true,
            ScrollBars = ScrollBars.Vertical,
            Dock = DockStyle.Fill,
            Height = 90,
            // UseSystemPasswordChar stays off: the user needs to see what they are about to send. The text is
            // never logged or stored by this client (ADR-019 FR-705).
        };
        _typeSend = MakeButton("Type text", async (_, _) => await TypeTextAsync().ConfigureAwait(true));
        _typeCancel = MakeButton("Cancel typing", (_, _) => _controller.CancelTypeText());

        _shortcut.AccessibleName = "Host shortcut";
        _pages.DrawItem += (_, e) => {
            if (e.Index < 0) return;
            var page = _pages.TabPages[e.Index];
            var area = e.Bounds;
            using var fill = new SolidBrush(page.BackColor);
            e.Graphics.FillRectangle(fill, area);
            TextRenderer.DrawText(e.Graphics, page.Text, _pages.Font, area, page.ForeColor,
                                  TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
            if (e.Index != _pages.SelectedIndex) return;
            int line = Math.Max(2, LogicalToDeviceUnits(2));
            using var stroke = new SolidBrush(page.ForeColor);
            e.Graphics.FillRectangle(stroke, area.Left + 4, area.Bottom - line, area.Width - 8, line);
            if ((e.State & DrawItemState.Focus) != 0)
                ControlPaint.DrawFocusRectangle(e.Graphics, Rectangle.Inflate(area, -3, -3), page.ForeColor, page.BackColor);
        };

        _icons.Add(_connect, "connect", IconRole.Client);
        _icons.Add(_disconnect, "disconnect", IconRole.Client);
        _icons.Add(_host, "host", IconRole.Host);
        _icons.Add(_advanced, "settings", IconRole.Neutral);
        _icons.Add(studio, "extensions", IconRole.Neutral);
        _icons.Add(video, "video", IconRole.Neutral);
        _icons.Add(display, "host", IconRole.Neutral);
        _icons.Add(_capture, "mouse", IconRole.Client);
        _icons.Add(shortcutSend, "shortcuts", IconRole.Client);
        _icons.Add(_typeSend, "keyboard", IconRole.Client);
        HandleCreated += (_, _) => RefreshAppearance(DeviceDpi);
        DpiChanged += (_, e) => RefreshAppearance(e.DeviceDpiNew);
        SystemColorsChanged += (_, _) => RefreshAppearance(DeviceDpi);

        var devices = AddPage(_pages, "Devices");
        devices.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        devices.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        devices.Controls.Add(_devices, 0, 0);
        devices.Controls.Add(ActionRow(_discover, _pair, _connect, _disconnect, _host, _advanced), 0, 1);

        var input = AddPage(_pages, "Host input");
        input.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        input.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        input.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        input.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        input.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        input.Controls.Add(ActionRow(_requestControl, _releaseControl, _capture), 0, 0);
        input.Controls.Add(ActionRow(_shortcut, shortcutSend), 0, 1);
        input.Controls.Add(new Label { Text = "Type using the host keyboard layout. Press Esc to cancel.", AutoSize = true }, 0, 2);
        input.Controls.Add(_typeText, 0, 3);
        input.Controls.Add(ActionRow(_typeSend, _typeCancel, _ssh), 0, 4);

        var tools = AddPage(_pages, "Tools");
        tools.Controls.Add(ActionRow(display, studio, video, about), 0, 0);

        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, Padding = new Padding(10) };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.Controls.Add(_pages, 0, 0);
        _status.AutoSize = true;
        _status.MinimumSize = new Size(0, 26);
        layout.Controls.Add(_status, 0, 1);
        Controls.Add(layout);

        _tray = new NotifyIcon
        {
            Icon = SystemIcons.Application,
            Text = "KVMpad Client",
            Visible = true,
            ContextMenuStrip = BuildTrayMenu(),
        };
        _tray.DoubleClick += (_, _) => RestoreFromTray();

        _controller.Changed += OnControllerChanged;
        _controller.Notice += OnControllerNotice;

        Activated += (_, _) => _controller.OnActivationChanged(true);
        Deactivate += (_, _) => _controller.OnActivationChanged(false);
        KeyPreview = true;
        KeyDown += (_, e) =>
        {
            if (e.KeyCode == Keys.Escape)
                _controller.CancelTypeText();
        };

        Load += async (_, _) =>
        {
            var work = Screen.FromControl(this).WorkingArea;
            if (Width > work.Width || Height > work.Height) {
                Size = new Size(Math.Min(Width, work.Width), Math.Min(Height, work.Height));
                Location = new Point(Math.Clamp(Left, work.Left, Math.Max(work.Left, work.Right - Width)),
                                     Math.Clamp(Top, work.Top, Math.Max(work.Top, work.Bottom - Height)));
            }
            RefreshDeviceList();
            await DiscoverAsync().ConfigureAwait(true);
        };
        FormClosing += OnFormClosing;
        UpdateButtons();
        RefreshAppearance(DeviceDpi);
    }

    private void RefreshAppearance(int dpi)
    {
#pragma warning disable WFO5001
        bool dark = Application.IsDarkModeEnabled;
#pragma warning restore WFO5001
        bool contrast = SystemInformation.HighContrast;
        var canvas = contrast ? SystemColors.Window : ColorTranslator.FromHtml(dark ? "#16202b" : "#f4f6f8");
        var face = contrast ? SystemColors.Control : ColorTranslator.FromHtml(dark ? "#263442" : "#ffffff");
        var ink = contrast ? SystemColors.WindowText : ColorTranslator.FromHtml(dark ? "#ecf2f8" : "#172332");
        var edge = contrast ? SystemColors.ControlText : ColorTranslator.FromHtml(dark ? "#526174" : "#aebbc6");
        BackColor = canvas;
        ForeColor = ink;
        _pages.DrawMode = contrast ? TabDrawMode.Normal : TabDrawMode.OwnerDrawFixed;
        _icons.Apply(dpi, dark, contrast, SystemColors.ControlText);
        foreach (TabPage page in _pages.TabPages) {
            page.UseVisualStyleBackColor = false;
            page.BackColor = canvas;
            page.ForeColor = ink;
            foreach (Control row in page.Controls[0].Controls) {
                if (row is not FlowLayoutPanel) continue;
                foreach (Control action in row.Controls) {
                    if (action is not Button button) continue;
                    button.FlatStyle = FlatStyle.Flat;
                    button.BackColor = face;
                    button.ForeColor = contrast ? SystemColors.ControlText : ink;
                    button.FlatAppearance.BorderColor = edge;
                }
            }
        }
    }

    protected override void WndProc(ref Message m)
    {
        base.WndProc(ref m);
        const int SettingChanged = 0x001A, ThemeChanged = 0x031A;
        if (m.Msg is SettingChanged or ThemeChanged && IsHandleCreated && !Disposing)
            RefreshAppearance(DeviceDpi);
    }

    private static Button MakeButton(string text, EventHandler handler)
    {
        var button = new ActionButton { Text = text, AutoSize = true, Margin = new Padding(0, 0, 8, 6) };
        button.Click += handler;
        return button;
    }

    private sealed class ActionButton : Button
    {
        protected override void OnPaint(PaintEventArgs e)
        {
            if (Enabled || SystemInformation.HighContrast) {
                base.OnPaint(e);
                return;
            }
            // Native flat disabled buttons draw black captions even on a dark custom surface.
            e.Graphics.Clear(BackColor);
            ControlPaint.DrawBorder(e.Graphics, ClientRectangle, FlatAppearance.BorderColor, ButtonBorderStyle.Solid);
            var area = Rectangle.Inflate(ClientRectangle, -3, -1);
            if (Image is { } icon) {
                e.Graphics.DrawImageUnscaled(icon, area.Left, (Height - icon.Height) / 2);
                area.X += icon.Width;
                area.Width -= icon.Width;
            }
            var ink = ColorTranslator.FromHtml(BackColor.GetBrightness() < 0.5 ? "#b2c0cc" : "#526174");
            TextRenderer.DrawText(e.Graphics, Text, Font, area, ink, TextFormatFlags.HorizontalCenter |
                                  TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.NoPadding);
        }
    }

    private static TableLayoutPanel AddPage(TabControl pages, string title)
    {
        var page = new TabPage(title) { UseVisualStyleBackColor = true, Padding = new Padding(10) };
        var content = new TableLayoutPanel { ColumnCount = 1, Dock = DockStyle.Fill };
        content.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        page.Controls.Add(content);
        pages.TabPages.Add(page);
        return content;
    }

    private static FlowLayoutPanel ActionRow(params Control[] controls)
    {
        var actions = new FlowLayoutPanel {
            AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink,
            Dock = DockStyle.Fill, Margin = new Padding(0, 4, 0, 0),
        };
        actions.Controls.AddRange(controls);
        return actions;
    }

    private ContextMenuStrip BuildTrayMenu()
    {
        var menu = new ContextMenuStrip();
        menu.Items.Add("Show window", null, (_, _) => RestoreFromTray());
        menu.Items.Add("Toggle capture", null, (_, _) => _controller.ToggleCapture());
        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add("Exit", null, (_, _) => Close());
        return menu;
    }

    private void RestoreFromTray()
    {
        Show();
        WindowState = FormWindowState.Normal;
        Activate();
    }

    // ---- actions ---------------------------------------------------------------------------------

    private async Task DiscoverAsync()
    {
        _discover.Enabled = false;
        try
        {
            IReadOnlyList<DiscoveredDevice> found = await _controller
                .DiscoverAsync(CancellationToken.None)
                .ConfigureAwait(true);
            _discovered = new List<DiscoveredDevice>(found);
            RefreshDeviceList();
        }
        finally
        {
            _discover.Enabled = true;
        }
    }

    private async Task PairAsync()
    {
        DiscoveredDevice? device = SelectedDiscovered();
        if (device is null)
        {
            ShowNotice("Select a discovered device first.");
            return;
        }

        if (!device.PairingOpen)
        {
            // The TXT record says pair=0, so the device's window is shut and the endpoint is not listening
            // (ADR-029 §4, SEC-103). Saying so beats a connection refusal the user has to interpret.
            ShowNotice("That device is not in pairing mode. Tap “Pair client” on it, then try again.");
            return;
        }

        await PairAtAddressAsync(device.DisplayName, device.ConnectHost, device.Port,
            SelectedEntry?.Paired?.DeviceId ?? device.InstanceName).ConfigureAwait(true);
    }

    private async Task PairAtAddressAsync(string displayName, string host, int port, string? deviceId)
    {
        using var dialog = new PairDialog(displayName);
        if (dialog.ShowDialog(this) != DialogResult.OK)
            return;

        if (await _controller.PairAsync(host, port, dialog.Code, deviceId).ConfigureAwait(true))
            RefreshDeviceList(_settings.LastDeviceId);
    }

    private async Task ConnectAsync()
    {
        PairedDevice? paired = SelectedEntry?.PairedForConnection;
        if (paired is null)
        {
            ShowNotice("That device is not paired on this computer yet. Use Pair… first.");
            return;
        }

        await _controller.ConnectAsync(paired, CancellationToken.None).ConfigureAwait(true);
    }

    private async Task ShowAdvancedAsync()
    {
        var paired = DeviceListEntry.Merge([], _controller.Store.List()).Select(row => row.Paired!).ToArray();
        using var dialog = new AdvancedConnectDialog(paired, SelectedId());
        if (dialog.ShowDialog(this) != DialogResult.OK)
            return;
        if (dialog.Pairing is { } saved)
        {
            await _controller.ConnectAsync(saved.WithEndpoint(dialog.Host, dialog.Port), CancellationToken.None, refreshAddress: false)
                .ConfigureAwait(true);
            RefreshDeviceList(saved.DeviceId);
        }
        else
            await PairAtAddressAsync(dialog.Host, dialog.Host, dialog.Port, null).ConfigureAwait(true);
    }

    private async Task TypeTextAsync()
    {
        string text = _typeText.Text;
        if (text.Length == 0)
            return;

        if (text.Length > Core.Protocol.ProtocolLimits.DefaultTypeTextMaxCharacters)
        {
            DialogResult answer = MessageBox.Show(
                this,
                $"This text is {text.Length} characters. Only the first "
                + $"{Core.Protocol.ProtocolLimits.DefaultTypeTextMaxCharacters} will be typed. Continue?",
                "Type text",
                MessageBoxButtons.OKCancel,
                MessageBoxIcon.Warning);
            if (answer != DialogResult.OK)
                return;
        }

        _typeSend.Enabled = false;
        try
        {
            Core.TypeText.TypeTextResult? result = await _controller.TypeTextAsync(text).ConfigureAwait(true);
            if (result is not null)
                SetStatusText(_controller.StatusLine + "  ·  type text " + result.Outcome);
        }
        finally
        {
            _typeSend.Enabled = true;
        }
    }

    // ---- view ------------------------------------------------------------------------------------

    private void RefreshDeviceList(string? preferredId = null)
    {
        string? selected = preferredId ?? SelectedId() ?? _controller.ConnectedDeviceId ?? _settings.LastDeviceId;
        var rows = DeviceListEntry.Merge(_discovered, _controller.Store.List());
        _devices.BeginUpdate();
        _devices.Items.Clear();
        foreach (var row in rows)
        {
            var seen = row.Discovered;
            var paired = row.Paired;
            var item = new ListViewItem(seen?.DisplayName ?? paired!.DeviceName) { Tag = row };
            item.SubItems.Add(seen != null ? $"{seen.ConnectHost}:{seen.Port}" : $"{paired!.Host}:{paired.Port}");
            item.SubItems.Add(paired == null ? "no" : "yes");
            item.SubItems.Add(seen == null ? "-" : seen.PairingOpen ? "yes" : "no");
            item.SubItems.Add("v" + (seen?.ProtocolVersion ?? 1));
            _devices.Items.Add(item);
            if (selected != null && string.Equals(DeviceListEntry.InstanceKey(selected),
                    DeviceListEntry.InstanceKey(row.DeviceId), StringComparison.OrdinalIgnoreCase))
                item.Selected = true;
        }
        _devices.EndUpdate();
        UpdateButtons();
    }

    private DeviceListEntry? SelectedEntry => _devices.SelectedItems.Count == 0
        ? null : _devices.SelectedItems[0].Tag as DeviceListEntry;

    private DiscoveredDevice? SelectedDiscovered() => SelectedEntry?.Discovered;

    private string? SelectedId() => SelectedEntry?.DeviceId;

    private async Task PrepareSshAsync()
    {
        var setup = _controller.Ssh;
        if (_preparingSsh || setup?.Available != true) return;
        using var pick = new OpenFileDialog { Title = "Select your OpenSSH public key",
            Filter = "OpenSSH public keys (*.pub)|*.pub", CheckFileExists = true };
        if (pick.ShowDialog(this) != DialogResult.OK || setup != _controller.Ssh || !setup.Available) return;
        _preparingSsh = true;
        UpdateButtons();
        try
        {
            string command = await setup.PrepareAsync(pick.FileName, CancellationToken.None);
            if (IsDisposed || Disposing || setup != _controller.Ssh || !setup.Available) return;
            using var dialog = new SshCommandDialog(command);
            dialog.ShowDialog(this);
        }
        catch (Exception ex) when (ex is InvalidOperationException or InvalidDataException or TimeoutException)
        { if (!IsDisposed && !Disposing) ShowNotice(ex.Message); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or Win32Exception or OperationCanceledException)
        { if (!IsDisposed && !Disposing) ShowNotice(ex.GetType() == typeof(IOException) ? ex.Message :
            "Could not prepare the SSH command. Check the public key file, Windows OpenSSH Client and the paired connection."); }
        finally { _preparingSsh = false; if (!IsDisposed && !Disposing) UpdateButtons(); }
    }

    private void UpdateButtons()
    {
        bool hasSelection = _devices.SelectedItems.Count > 0;
        bool connected = _controller.Logic is { } logic && (logic.State != Core.Session.ClientSessionState.Disconnected
            || logic.EndReason == Core.Session.SessionEndReason.None);
        bool controlling = _controller.Logic?.CanSendInput ?? false;

        _pair.Enabled = SelectedDiscovered() is not null;
        _connect.Enabled = hasSelection && !connected;
        _advanced.Enabled = !connected;
        _disconnect.Enabled = connected;
        _host.Enabled = _controller.Logic?.HasAuthenticatedSession == true &&
            string.Equals(SelectedId(), _controller.ConnectedDeviceId, StringComparison.OrdinalIgnoreCase);
        _host.Text = _controller.HostRunning ? "Stop HOST" : "Start HOST";
        _requestControl.Enabled = _controller.Logic?.State == Core.Session.ClientSessionState.Connected;
        _ssh.Enabled = !_preparingSsh && _controller.Ssh?.Available == true;
        _releaseControl.Enabled = controlling;
        _capture.Enabled = controlling;
        _capture.Text = _controller.IsCapturing ? "Stop capture" : "Start capture";
        _typeSend.Enabled = controlling;
        _typeCancel.Enabled = controlling;
    }

    private void OnControllerChanged()
    {
        if (IsDisposed || Disposing) return;
        if (InvokeRequired)
        {
            BeginInvoke(OnControllerChanged);
            return;
        }
        SetStatusText(_controller.StatusLine);
        UpdateButtons();
        _tray.Text = Truncate("KVMpad Client — " + _controller.StatusLine, 63);
    }

    private void OnControllerNotice(string message)
    {
        if (InvokeRequired)
        {
            BeginInvoke(() => OnControllerNotice(message));
            return;
        }
        ShowNotice(message);
    }

    private void SetStatusText(string text) => _status.Text = text;

    private async Task LaunchToolAsync(string folder, string script, string requirement)
    {
        string directory = Path.Combine(AppContext.BaseDirectory, folder);
        string launcher = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "py.exe");
        if (!File.Exists(launcher))
            launcher = "py.exe";
        var start = new ProcessStartInfo(launcher)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            WorkingDirectory = directory
        };
        start.ArgumentList.Add("-3");
        start.ArgumentList.Add("-B");
        start.ArgumentList.Add(Path.Combine(directory, script));
        try
        {
            using var process = Process.Start(start);
            if (process == null)
                throw new InvalidOperationException();
            await process.WaitForExitAsync().ConfigureAwait(true);
            if (process.ExitCode == 0 || IsDisposed) return;
        }
        catch (Exception e) when (e is Win32Exception or InvalidOperationException or IOException)
        {
            if (IsDisposed) return;
        }
        ShowNotice("The desktop tool could not start. Check the Python Windows launcher. " + requirement);
    }

    private void ShowNotice(string message)
        => MessageBox.Show(this, message, "KVMpad Client", MessageBoxButtons.OK, MessageBoxIcon.Information);

    private static string Truncate(string value, int max)
        => value.Length <= max ? value : value[..max];

    private void OnFormClosing(object? sender, FormClosingEventArgs e)
    {
        // Leaving capture and releasing every held key is not optional on the way out: a process that exits
        // while the host believes Ctrl is down has left the host in a broken state (AGENTS §2.3).
        _controller.Capture?.ExitCapture(ReleaseReason.Shutdown);
        _settings.Save();
        _tray.Visible = false;
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) {
            _controller.Changed -= OnControllerChanged;
            _controller.Notice -= OnControllerNotice;
            _icons.Dispose();
            _tray.Dispose();
        }
        base.Dispose(disposing);
    }

    private sealed class ShortcutItem
    {
        public ShortcutItem(string id) => Id = id;

        public string Id { get; }

        public override string ToString() => ChordIds.Label(Id);
    }
}
