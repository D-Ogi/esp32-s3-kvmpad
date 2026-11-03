// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Drawing;
using System.Windows.Forms;

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

    public AdvancedConnectDialog()
    {
        Text = "Advanced: connect by address";
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MinimizeBox = false;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterParent;
        ClientSize = new Size(440, 210);

        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 2,
            RowCount = 4,
            Padding = new Padding(16),
        };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        for (int i = 0; i < 4; i++)
            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        var warning = new Label
        {
            AutoSize = true,
            MaximumSize = new Size(400, 0),
            ForeColor = SystemColors.GrayText,
            Text = "Use this only when the device does not appear in the list because multicast DNS is blocked "
                + "on this network. Pairing and certificate pinning work exactly as they do for a discovered device.",
        };

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
        ok.Click += (_, _) =>
        {
            Host = _host.Text.Trim();
            Port = (int)_port.Value;
        };
        buttons.Controls.Add(ok);
        buttons.Controls.Add(cancel);

        layout.Controls.Add(warning, 0, 0);
        layout.SetColumnSpan(warning, 2);
        layout.Controls.Add(new Label { Text = "Address", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 1);
        layout.Controls.Add(_host, 1, 1);
        layout.Controls.Add(new Label { Text = "Port", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 2);
        layout.Controls.Add(_port, 1, 2);
        layout.Controls.Add(buttons, 0, 3);
        layout.SetColumnSpan(buttons, 2);
        Controls.Add(layout);

        AcceptButton = ok;
        CancelButton = cancel;
    }

    public string Host { get; private set; } = string.Empty;

    public int Port { get; private set; } = 8443;
}
