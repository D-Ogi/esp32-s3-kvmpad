// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
using System.Drawing;
using System.Windows.Forms;

namespace KVMpad.Client.App;

internal sealed class SshCommandDialog : Form
{
    public SshCommandDialog(string command)
    {
        Text = "KVMpad SSH command";
        StartPosition = FormStartPosition.CenterParent;
        ClientSize = new Size(620, 230);
        MinimumSize = new Size(420, 220);
        ShowInTaskbar = false;
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 3, Padding = new Padding(12) };
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.Controls.Add(new Label { Dock = DockStyle.Fill, AutoSize = true,
            Text = "Keep CLIENT connected. Run this command in PowerShell, then approve control on KVMpad." }, 0, 0);
        var text = new TextBox { Multiline = true, ReadOnly = true, Dock = DockStyle.Fill,
            ScrollBars = ScrollBars.Vertical, AccessibleName = "PowerShell SSH command", Text = command };
        layout.Controls.Add(text, 0, 1);
        var close = new Button { Text = "Close", AutoSize = true, DialogResult = DialogResult.Cancel };
        var copy = new Button { Text = "Copy command", AutoSize = true };
        copy.Click += (_, _) => { text.SelectAll(); text.Copy(); };
        var buttons = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, FlowDirection = FlowDirection.RightToLeft };
        buttons.Controls.AddRange([close, copy]);
        layout.Controls.Add(buttons, 0, 2);
        Controls.Add(layout);
        CancelButton = close;
        Shown += (_, _) => { text.Focus(); text.SelectAll(); };
    }
}
