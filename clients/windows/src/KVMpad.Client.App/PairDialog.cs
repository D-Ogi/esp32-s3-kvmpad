// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Drawing;
using System.Windows.Forms;
using KVMpad.Client.Core.Discovery;
using KVMpad.Client.Core.Pairing;

namespace KVMpad.Client.App;

/// <summary>
/// Asks for the six digits shown on the device, and nothing else.
/// </summary>
/// <remarks>
/// FR-721 is a hard requirement about this dialog specifically: in the normal path the user types exactly six
/// digits during the whole pairing process — no address, no fingerprint, no password, no name. The client name
/// is the Windows host name (FR-724), the address came from mDNS, and the certificate is pinned from the
/// handshake. So this form has one input, and it accepts only digits.
/// </remarks>
internal sealed class PairDialog : Form
{
    private readonly TextBox _code;
    private readonly Label _status;

    public PairDialog(DiscoveredDevice device)
    {
        Device = device ?? throw new ArgumentNullException(nameof(device));

        Text = "Pair with " + device.DisplayName;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MinimizeBox = false;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterParent;
        ClientSize = new Size(420, 230);

        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 5,
            Padding = new Padding(16),
        };
        for (int i = 0; i < 5; i++)
            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        var instruction = new Label
        {
            AutoSize = true,
            MaximumSize = new Size(380, 0),
            Text = "Enter the six-digit code shown on " + device.DisplayName + ".",
        };

        var hint = new Label
        {
            AutoSize = true,
            MaximumSize = new Size(380, 0),
            ForeColor = SystemColors.GrayText,
            Text = "The code is valid for two minutes. The device will then ask you to accept this computer, "
                + "named " + Environment.MachineName + ".",
        };

        _code = new TextBox
        {
            MaxLength = 16,
            Font = new Font(FontFamily.GenericMonospace, 22f, FontStyle.Bold),
            TextAlign = HorizontalAlignment.Center,
            Width = 380,
        };
        _code.TextChanged += (_, _) => UpdateState();

        _status = new Label
        {
            AutoSize = true,
            MaximumSize = new Size(380, 0),
            ForeColor = SystemColors.GrayText,
            Text = "Six digits.",
        };

        var buttons = new FlowLayoutPanel
        {
            FlowDirection = FlowDirection.RightToLeft,
            AutoSize = true,
            Dock = DockStyle.Fill,
        };
        var cancel = new Button { Text = "Cancel", DialogResult = DialogResult.Cancel, AutoSize = true };
        var pair = new Button { Text = "Pair", DialogResult = DialogResult.OK, AutoSize = true, Enabled = false };
        pair.Click += (_, _) => Code = PairingCode.Normalize(_code.Text);
        buttons.Controls.Add(pair);
        buttons.Controls.Add(cancel);

        layout.Controls.Add(instruction, 0, 0);
        layout.Controls.Add(_code, 0, 1);
        layout.Controls.Add(_status, 0, 2);
        layout.Controls.Add(hint, 0, 3);
        layout.Controls.Add(buttons, 0, 4);
        Controls.Add(layout);

        AcceptButton = pair;
        CancelButton = cancel;
        PairButton = pair;
    }

    public DiscoveredDevice Device { get; }

    /// <summary>The normalized six digits, valid only when the dialog returned OK.</summary>
    public string Code { get; private set; } = string.Empty;

    private Button PairButton { get; }

    private void UpdateState()
    {
        string normalized = PairingCode.Normalize(_code.Text);
        bool valid = PairingCode.IsValid(normalized);
        PairButton.Enabled = valid;
        _status.Text = valid
            ? "Ready."
            : normalized.Length > PairingCode.DigitCount
                ? "That is more than six digits."
                : $"{normalized.Length} of {PairingCode.DigitCount} digits.";
    }
}
