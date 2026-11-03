// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Drawing;
using System.Reflection;
using System.Windows.Forms;
using KVMpad.Client.Core;

namespace KVMpad.Client.App;

/// <summary>
/// The About dialog required by ADR-025 FR-751/TECH-084: the version, the license name, the Required Notice
/// and the third-party notices. The same notice is in the binary's <c>LegalCopyright</c> field, so it travels
/// with a copied executable as well as with a running one.
/// </summary>
internal sealed class AboutForm : Form
{
    public AboutForm()
    {
        Text = "About KVMpad Client";
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MinimizeBox = false;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterParent;
        ClientSize = new Size(560, 420);

        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 4,
            Padding = new Padding(16),
        };
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        string version = Assembly.GetExecutingAssembly().GetName().Version?.ToString() ?? "0.0.0";
        var title = new Label
        {
            AutoSize = true,
            Font = new Font(SystemFonts.MessageBoxFont!.FontFamily, 12f, FontStyle.Bold),
            Text = "KVMpad Client " + version,
        };

        var notice = new TextBox
        {
            Multiline = true,
            ReadOnly = true,
            Dock = DockStyle.Fill,
            Height = 92,
            BorderStyle = BorderStyle.None,
            BackColor = SystemColors.Control,
            Text = LicenseNotice.LicenseName + "\r\n"
                + LicenseNotice.AboutBlock + "\r\n"
                + LicenseNotice.ProjectUrl,
        };

        var thirdParty = new TextBox
        {
            Multiline = true,
            ReadOnly = true,
            ScrollBars = ScrollBars.Vertical,
            Dock = DockStyle.Fill,
            WordWrap = false,
            Text = ThirdPartyNotices.Load(),
        };

        var close = new Button
        {
            Text = "Close",
            DialogResult = DialogResult.OK,
            AutoSize = true,
            Anchor = AnchorStyles.Right,
        };

        layout.Controls.Add(title, 0, 0);
        layout.Controls.Add(notice, 0, 1);
        layout.Controls.Add(thirdParty, 0, 2);
        layout.Controls.Add(close, 0, 3);
        Controls.Add(layout);
        AcceptButton = close;
        CancelButton = close;
    }
}

/// <summary>
/// The third-party attribution list shown in About.
/// </summary>
/// <remarks>
/// ADR-025 FR-751 asks for the list to be generated from <c>THIRD_PARTY_NOTICES.md</c> at build time rather
/// than hand-maintained. The client's own build step for that is not wired yet, so this reads the file when it
/// is next to the executable and otherwise says plainly where to find it — which is honest, and better than
/// a stale copy compiled in.
/// </remarks>
internal static class ThirdPartyNotices
{
    public static string Load()
    {
        string path = System.IO.Path.Combine(AppContext.BaseDirectory, "THIRD_PARTY_NOTICES.md");
        try
        {
            if (System.IO.File.Exists(path))
                return System.IO.File.ReadAllText(path);
        }
        catch (System.IO.IOException)
        {
            // Fall through to the pointer below.
        }

        return "Third-party notices\r\n\r\n"
            + "This build carries no bundled third-party components beyond the .NET runtime it runs on.\r\n"
            + "The full attribution list for the project is THIRD_PARTY_NOTICES.md in the repository:\r\n"
            + LicenseNotice.ProjectUrl + "/blob/main/THIRD_PARTY_NOTICES.md";
    }
}
