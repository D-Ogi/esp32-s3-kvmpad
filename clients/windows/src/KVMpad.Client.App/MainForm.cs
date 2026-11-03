// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.Drawing;
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

    private readonly ListView _devices;
    private readonly Label _status;
    private readonly Button _discover;
    private readonly Button _pair;
    private readonly Button _connect;
    private readonly Button _disconnect;
    private readonly Button _requestControl;
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
        ClientSize = new Size(760, 540);
        MinimumSize = new Size(640, 460);
        StartPosition = FormStartPosition.CenterScreen;

        _devices = new ListView
        {
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
        _devices.SelectedIndexChanged += (_, _) => UpdateButtons();

        _status = new Label
        {
            Dock = DockStyle.Fill,
            AutoSize = false,
            TextAlign = ContentAlignment.MiddleLeft,
            Padding = new Padding(8, 0, 0, 0),
            Height = 28,
            BorderStyle = BorderStyle.Fixed3D,
        };

        _discover = MakeButton("Discover", async (_, _) => await DiscoverAsync().ConfigureAwait(true));
        _pair = MakeButton("Pair…", async (_, _) => await PairAsync().ConfigureAwait(true));
        _connect = MakeButton("Connect", async (_, _) => await ConnectAsync().ConfigureAwait(true));
        _disconnect = MakeButton("Disconnect", async (_, _) => await _controller.DisconnectAsync().ConfigureAwait(true));
        _requestControl = MakeButton("Request control", async (_, _) => await _controller.RequestControlAsync().ConfigureAwait(true));
        _releaseControl = MakeButton("Release control", async (_, _) => await _controller.ReleaseControlAsync().ConfigureAwait(true));
        _capture = MakeButton("Start capture", (_, _) => _controller.ToggleCapture());
        var advanced = MakeButton("Advanced…", (_, _) => ShowAdvanced());
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
            Multiline = true,
            ScrollBars = ScrollBars.Vertical,
            Dock = DockStyle.Fill,
            Height = 90,
            // UseSystemPasswordChar stays off: the user needs to see what they are about to send. The text is
            // never logged or stored by this client (ADR-019 FR-705).
        };
        _typeSend = MakeButton("Type text", async (_, _) => await TypeTextAsync().ConfigureAwait(true));
        _typeCancel = MakeButton("Cancel typing", (_, _) => _controller.CancelTypeText());

        var deviceButtons = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill, WrapContents = true };
        deviceButtons.Controls.AddRange([_discover, _pair, _connect, _disconnect, advanced]);

        var controlButtons = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill, WrapContents = true };
        controlButtons.Controls.AddRange([_requestControl, _releaseControl, _capture, _shortcut, shortcutSend, about]);

        var typeButtons = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill, WrapContents = true };
        typeButtons.Controls.AddRange([_typeSend, _typeCancel]);

        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 7,
            Padding = new Padding(10),
        };
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 92));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 30));

        layout.Controls.Add(_devices, 0, 0);
        layout.Controls.Add(deviceButtons, 0, 1);
        layout.Controls.Add(controlButtons, 0, 2);
        layout.Controls.Add(new Label
        {
            Text = "Type text on the host (uses the host keyboard layout; press Cancel typing or Esc to stop)",
            AutoSize = true,
        }, 0, 3);
        layout.Controls.Add(_typeText, 0, 4);
        layout.Controls.Add(typeButtons, 0, 5);
        layout.Controls.Add(_status, 0, 6);
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
            RefreshDeviceList();
            await DiscoverAsync().ConfigureAwait(true);
        };
        FormClosing += OnFormClosing;
        UpdateButtons();
    }

    private static Button MakeButton(string text, EventHandler handler)
    {
        var button = new Button { Text = text, AutoSize = true, Margin = new Padding(0, 0, 8, 6) };
        button.Click += handler;
        return button;
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

        using var dialog = new PairDialog(device);
        if (dialog.ShowDialog(this) != DialogResult.OK)
            return;

        await _controller.PairAsync(device, dialog.Code).ConfigureAwait(true);
    }

    private async Task ConnectAsync()
    {
        PairedDevice? paired = SelectedPaired();
        if (paired is null)
        {
            ShowNotice("That device is not paired on this computer yet. Use Pair… first.");
            return;
        }

        DiscoveredDevice? seen = SelectedDiscovered();
        if (seen is not null && seen.Port > 0)
            paired = paired.WithEndpoint(seen.ConnectHost, seen.Port);

        await _controller.ConnectAsync(paired, CancellationToken.None).ConfigureAwait(true);
    }

    private void ShowAdvanced()
    {
        using var dialog = new AdvancedConnectDialog();
        if (dialog.ShowDialog(this) != DialogResult.OK || dialog.Host.Length == 0)
            return;

        ShowNotice(
            "Noted " + dialog.Host + ":" + dialog.Port + ". Pairing with a hand-entered address needs the PAKE "
            + "implementation from ADR-029; until it lands, pair over a network where the device is discovered.");
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

    private void RefreshDeviceList()
    {
        IReadOnlyList<PairedDevice> paired = _controller.Store.List();
        var byId = new Dictionary<string, PairedDevice>(StringComparer.OrdinalIgnoreCase);
        foreach (PairedDevice device in paired)
            byId[device.DeviceId] = device;

        _devices.BeginUpdate();
        _devices.Items.Clear();

        var listed = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (DiscoveredDevice device in _discovered)
        {
            string id = InstanceLabel(device.InstanceName);
            listed.Add(id);
            var item = new ListViewItem(device.DisplayName) { Tag = device };
            item.SubItems.Add(device.ConnectHost + ":" + device.Port);
            item.SubItems.Add(byId.ContainsKey(id) ? "yes" : "no");
            item.SubItems.Add(device.PairingOpen ? "yes" : "no");
            item.SubItems.Add(device.IsCompatible ? "v1" : "v" + device.ProtocolVersion);
            _devices.Items.Add(item);
        }

        foreach (PairedDevice device in paired)
        {
            if (listed.Contains(device.DeviceId))
                continue;
            var item = new ListViewItem(device.DeviceName) { Tag = device };
            item.SubItems.Add(device.Host + ":" + device.Port);
            item.SubItems.Add("yes");
            item.SubItems.Add("-");
            item.SubItems.Add("v1");
            _devices.Items.Add(item);
        }

        _devices.EndUpdate();
        UpdateButtons();
    }

    private static string InstanceLabel(string instanceName)
    {
        int dot = instanceName.IndexOf('.');
        return dot > 0 ? instanceName[..dot] : instanceName;
    }

    private DiscoveredDevice? SelectedDiscovered()
        => _devices.SelectedItems.Count > 0 ? _devices.SelectedItems[0].Tag as DiscoveredDevice : null;

    private PairedDevice? SelectedPaired()
    {
        if (_devices.SelectedItems.Count == 0)
            return null;
        object? tag = _devices.SelectedItems[0].Tag;
        if (tag is PairedDevice paired)
            return paired;
        if (tag is DiscoveredDevice discovered)
            return _controller.Store.Find(InstanceLabel(discovered.InstanceName));
        return null;
    }

    private void UpdateButtons()
    {
        bool hasSelection = _devices.SelectedItems.Count > 0;
        bool connected = _controller.Logic is not null;
        bool controlling = _controller.Logic?.CanSendInput ?? false;

        _pair.Enabled = SelectedDiscovered() is not null;
        _connect.Enabled = hasSelection && !connected;
        _disconnect.Enabled = connected;
        _requestControl.Enabled = _controller.Logic?.State == Core.Session.ClientSessionState.Connected;
        _releaseControl.Enabled = controlling;
        _capture.Enabled = controlling;
        _capture.Text = _controller.IsCapturing ? "Stop capture" : "Start capture";
        _typeSend.Enabled = controlling;
        _typeCancel.Enabled = controlling;
    }

    private void OnControllerChanged()
    {
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
        if (disposing)
            _tray.Dispose();
        base.Dispose(disposing);
    }

    private sealed class ShortcutItem
    {
        public ShortcutItem(string id) => Id = id;

        public string Id { get; }

        public override string ToString() => ChordIds.Label(Id);
    }
}
