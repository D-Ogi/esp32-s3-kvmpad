// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.Drawing;
using System.Net;
using System.Windows.Forms;
using KVMpad.Client.Core.Storage;

namespace KVMpad.Client.App;

/// <summary>
/// The emergency fallback for a network where mDNS is blocked: the device's address typed by hand.
/// </summary>
/// <remarks>
/// FR-723 allows this and requires it to be marked as advanced, because it is a path outside FR-721's
/// "six digits and nothing else". It changes only how the client finds the device: pairing still runs, the
/// certificate is still pinned from the handshake, and the six digits are still the only secret.
/// </remarks>
internal sealed class AdvancedConnectDialog : Form
{
    private readonly TextBox _host;
    private readonly NumericUpDown _port;

    public AdvancedConnectDialog(IReadOnlyList<PairedDevice> devices, string? selectedId)
    {
        Text = "Advanced: connect by address";
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MinimizeBox = false;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterParent;
        ClientSize = new Size(440, 235);

        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 2,
            RowCount = 5,
            Padding = new Padding(16),
        };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        for (int i = 0; i < 5; i++)
            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        var warning = new Label
        {
            AutoSize = true,
            MaximumSize = new Size(400, 0),
            Text = "Choose a saved device or pair a new KVMpad using the PIN on its screen.",
        };

        var deviceList = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Fill };
        deviceList.Items.Add("Pair a new KVMpad");
        int selected = 0;
        foreach (var device in devices)
        {
            int index = deviceList.Items.Add($"{device.DeviceName} ({device.Host})");
            if (device.DeviceId == selectedId || (selectedId == null && devices.Count == 1)) selected = index;
        }
        _host = new TextBox { Dock = DockStyle.Fill, PlaceholderText = "192.168.1.50" };
        _port = new NumericUpDown { Minimum = 1, Maximum = 65535, Value = 8443, Dock = DockStyle.Fill };

        var buttons = new FlowLayoutPanel
        {
            FlowDirection = FlowDirection.RightToLeft,
            AutoSize = true,
            Dock = DockStyle.Fill,
        };
        var cancel = new Button { Text = "Cancel", DialogResult = DialogResult.Cancel, AutoSize = true };
        var ok = new Button { Text = "Continue", DialogResult = DialogResult.OK, AutoSize = true };
        deviceList.SelectedIndexChanged += (_, _) =>
        {
            Pairing = deviceList.SelectedIndex > 0 ? devices[deviceList.SelectedIndex - 1] : null;
            _host.Text = Pairing?.Host ?? "";
            _port.Value = Pairing?.Port is > 0 and <= 65535 ? Pairing.Port : 8443;
            ok.Text = Pairing == null ? "Continue to PIN" : "Connect";
        };
        _host.TextChanged += (_, _) => ok.Enabled = Host.Length != 0;
        deviceList.SelectedIndex = selected;
        ok.Enabled = Host.Length != 0;
        buttons.Controls.Add(ok);
        buttons.Controls.Add(cancel);

        layout.Controls.Add(warning, 0, 0);
        layout.SetColumnSpan(warning, 2);
        layout.Controls.Add(new Label { Text = "Device", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 1);
        layout.Controls.Add(deviceList, 1, 1);
        layout.Controls.Add(new Label { Text = "Address", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 2);
        layout.Controls.Add(_host, 1, 2);
        layout.Controls.Add(new Label { Text = "Port", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 3);
        layout.Controls.Add(_port, 1, 3);
        layout.Controls.Add(buttons, 0, 4);
        layout.SetColumnSpan(buttons, 2);
        Controls.Add(layout);

        AcceptButton = ok;
        CancelButton = cancel;
    }

    public PairedDevice? Pairing { get; private set; }

    public string Host
    {
        get
        {
            string value = _host.Text.Trim();
            if (IPAddress.TryParse(value, out var address)) return address.ToString();
            return Uri.CheckHostName(value) == UriHostNameType.Dns ? value : string.Empty;
        }
    }

    public int Port => (int)_port.Value;
}
