// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Net;
using System.Threading.Tasks;
using System.Windows.Forms;
using KVMpad.Client.App;
using KVMpad.Client.Core.Display;
using KVMpad.Client.Core.Discovery;
using KVMpad.Client.Core.Storage;

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        if (args is ["--usb-reattach"]) { CheckUsbReattachment(); return 0; }
        if (args.Length is < 1 or > 3 || args[0] is not ("light" or "dark") ||
            (args.Length == 3 && args[2] != "--native-display")) return 2;
        Application.EnableVisualStyles();
#pragma warning disable WFO5001
        Application.SetColorMode(args[0] == "dark" ? SystemColorMode.Dark : SystemColorMode.Classic);
        bool dark = Application.IsDarkModeEnabled;
#pragma warning restore WFO5001
        bool highContrast = SystemInformation.HighContrast;
        var device = new DiscoveredDevice("fixture", "Test board", "fixture.local", 8443, Array.Empty<IPAddress>(), 1, true);
        using var pair = Open("PairDialog", device.DisplayName);
        var saved = new PairedDevice("fixture", "Test board", "192.0.2.30", 9443,
            new string('a', 64), "fixture-token", DateTimeOffset.UnixEpoch);
        using var advanced = Open("AdvancedConnectDialog", new[] { saved }, saved.DeviceId);
        using var about = Open("AboutForm");
        using var ssh = Open("SshCommandDialog", "fixture command");
        Require(Descendants(ssh).OfType<TextBox>().Single() is { ReadOnly: true, Text: "fixture command" } &&
                Descendants(ssh).OfType<Button>().Any(b => b.Text == "Copy command"), "Selectable SSH command");
        using var usb = Open("UsbDisplayDialog", (object)null!);
        Require(Descendants(usb).OfType<ComboBox>().Single() is { Enabled: false, AccessibleName: "Desktop resolution" } &&
                Descendants(usb).OfType<Button>().Single(b => b.Text == "Apply").Enabled == false,
                "USB mode changes wait for a detected display");
        if (args.Length == 3) CheckNativeUsbOffer(usb, args[1], args[0]);
        foreach (var form in new[] { pair, advanced, about, usb, ssh }) {
            Control[] controls = Descendants(form).ToArray();
            foreach (var control in controls) {
                var properties = TypeDescriptor.GetProperties(control);
                Require(!properties["ForeColor"]!.ShouldSerializeValue(control) &&
                        !properties["BackColor"]!.ShouldSerializeValue(control),
                        $"{form.GetType().Name} overrides the native colors for {control.GetType().Name}.");
            }
            // Change the inherited foreground/background in place, including custom contrast-theme colors.
            foreach (var (ink, paper) in new[] { (Color.Yellow, Color.Navy), (Color.White, Color.Black) }) {
                form.ForeColor = ink;
                form.BackColor = paper;
                foreach (Label label in controls.OfType<Label>())
                    Require(label.ForeColor == ink && label.BackColor == paper, "An active label lost its palette.");
            }
            Require(!form.Visible, "Dialogs must stay hidden during this test.");
        }
        var code = Descendants(pair).OfType<TextBox>().Single();
        var submit = (Button)pair.AcceptButton!;
        Require(!submit.Enabled && pair.CancelButton is Button { Enabled: true }, "Initial pairing buttons");
        foreach (var (value, valid, status) in new[] {
            ("1234", false, "4 of 6 digits."), ("123456", true, "Ready."),
            ("1234567", false, "That is more than six digits.") }) {
            code.Text = value;
            Require(submit.Enabled == valid && Descendants(pair).OfType<Label>().Any(l => l.Text == status),
                    "Pairing input state");
        }
        var address = Descendants(advanced).OfType<TextBox>().Single();
        var port = Descendants(advanced).OfType<NumericUpDown>().Single();
        var target = Descendants(advanced).OfType<ComboBox>().Single();
        var action = (Button)advanced.AcceptButton!;
        Require(address.Text == saved.Host && port.Value == saved.Port && action.Text == "Connect" && action.Enabled,
            "Saved device uses its existing pairing");
        address.Text = "[2001:db8::22]";
        Require((string?)advanced.GetType().GetProperty("Host")!.GetValue(advanced) == "2001:db8::22" &&
                ReferenceEquals(advanced.GetType().GetProperty("Pairing")!.GetValue(advanced), saved),
                "Manual IPv6 override keeps the selected credential");
        target.SelectedIndex = 0;
        Require(!action.Enabled && action.Text == "Continue to PIN" && port.Value == 8443 &&
                advanced.GetType().GetProperty("Pairing")!.GetValue(advanced) == null, "New device goes to PIN entry");
        foreach (string invalid in new[] { "https://example.test", "name@host", "board.local/path", "192.0.2.1:8443" }) {
            address.Text = invalid;
            Require(!action.Enabled, "Invalid address accepted");
        }
        address.Text = " board.local ";
        Require(action.Enabled && (string?)advanced.GetType().GetProperty("Host")!.GetValue(advanced) == "board.local",
            "Manual hostname");
        var notices = Descendants(about).OfType<TextBox>().ToArray();
        Require(notices.Length == 2 && notices.All(t => t.ReadOnly && t.Enabled && t.TextLength > 0),
                "Selectable About notices");
        CheckMainWindow(args.Length >= 2 ? args[1] : null, args[0]);
        Console.WriteLine($"PASS requested={args[0]} effectiveDark={dark} systemHighContrast={highContrast}; " +
                          "five hidden dialogs, three workspace pages, compact and desktop layouts.");
        return 0;
    }

    private static Form Open(string name, params object[] args)
    {
        var type = typeof(AppSettings).Assembly.GetType($"KVMpad.Client.App.{name}", throwOnError: true)!;
        return (Form)Activator.CreateInstance(type, args)!;
    }

    private sealed class ReattachingDisplay : IDisplayApi
    {
        internal static readonly DisplayResolution Vga = new(640,480), Native = new(1024,600);
        internal volatile int Stage; // Native, applied VGA, temporarily absent, restored native.
        internal int Changes;
        private DisplayResolution Size => Stage == 1 ? Vga : Native;

        public DisplayPath[] ActivePaths()
        {
            var pc = new DisplayPath(new(1,0,0),new(1,0,1),@"\\.\DISPLAY1",null,0,0,
                new(1920,1080),new(1920,1080),1,1,60000,1000,5,1);
            if (Stage == 2) return [pc];
            return [pc, pc with {Source=new(9,1,0), Target=new(9,1,1), X=1920,
                Name=Stage == 3 ? @"\\.\DISPLAY123" : @"\\.\DISPLAY122", Size=Size, Signal=Size,
                UsbId=@"USB\VID_303A&PID_2987\ABCDEF012345"}];
        }

        public SourceMode Current(string name) => new(Size,60,0,32,0);
        public IEnumerable<SourceMode> Offered(string name) => [new(Vga,60,0,32,0),new(Native,60,0,32,0)];
        public void Change(string name, SourceMode mode, bool test, DisplayPosition? position)
        {
            Require(name == @"\\.\DISPLAY122" && mode.Size == Vga, "Unexpected USB display change");
            System.Threading.Interlocked.Increment(ref Changes);
            if (!test) Stage = 1;
        }
    }

    private static void CheckUsbReattachment()
    {
        Application.EnableVisualStyles();
        var api = new ReattachingDisplay();
        using var dialog = Open("UsbDisplayDialog", null!, new UsbMonitorDisplayService(api));
        dialog.Opacity = 0;
        dialog.ShowInTaskbar = false;
        var modes = Descendants(dialog).OfType<ComboBox>().Single();
        var apply = Descendants(dialog).OfType<Button>().Single(button => button.Text == "Apply");
        var labels = Descendants(dialog).OfType<Label>().ToArray();
        async Task WaitFor(Func<bool> ready, string description)
        {
            long limit = Environment.TickCount64 + 8000;
            while (!ready() && Environment.TickCount64 < limit) await Task.Delay(15);
            Require(ready(), $"{description}: {string.Join(" / ", labels.Select(label => label.Text))}");
        }
        Exception? failure = null;
        dialog.Shown += async (_, _) => {
            try {
                await WaitFor(() => modes.Enabled, "Initial native display");
                modes.SelectedItem = ReattachingDisplay.Vga;
                apply.PerformClick();
                await WaitFor(() => modes.Enabled && labels.Any(l => l.Text == "Desktop  640 × 480"), "Initial VGA success");
                api.Stage = 2;
                await WaitFor(() => !modes.Enabled && labels.Any(l => l.Text.Contains("No active USB")), "Source disappeared");
                api.Stage = 3;
                await WaitFor(() => modes.Enabled && labels.Any(l => l.Text.Contains("Windows reconnected") &&
                    l.Text.Contains("640 × 480") && l.Text.Contains("1024 × 600")), "Reattached source and reverted mode");
                Require(labels.Any(l => l.Text == "Desktop  1024 × 600") && Equals(modes.SelectedItem, ReattachingDisplay.Native),
                        "Current desktop and selection show the recovered source");
                Descendants(dialog).OfType<Button>().Single(b => b.Text == "Refresh").PerformClick();
                await WaitFor(() => modes.Enabled && labels.Any(l => l.Text.Contains("Windows reconnected")), "Notice survives refresh");
                Require(api.Changes == 2, "A reattachment replayed the mode change");
            } catch (Exception e) { failure = e; }
            finally { dialog.Close(); }
        };
        Application.Run(dialog);
        if (failure != null) throw failure;
        Console.WriteLine("PASS hidden USB dialog: apply VGA, disappear, reattach at native size, refresh, one validated write");
    }

    // Hardware trial with a 640x480 desktop on the 7B native signal.
    // Raise the normal Shown event while keeping the form off the user's desktop.
    private static void CheckNativeUsbOffer(Form dialog, string directory, string palette)
    {
        _ = dialog.Handle;
        typeof(Form).GetMethod("OnShown", System.Reflection.BindingFlags.Instance |
            System.Reflection.BindingFlags.NonPublic)!.Invoke(dialog, [EventArgs.Empty]);
        var deadline = Environment.TickCount64 + 5000;
        var modes = Descendants(dialog).OfType<ComboBox>().Single();
        while (!modes.Enabled && Environment.TickCount64 < deadline) {
            Application.DoEvents();
            System.Threading.Thread.Sleep(10);
        }
        var action = Descendants(dialog).OfType<Button>().SingleOrDefault(b => b.Text == "Use 1024 × 600");
        Require(modes.SelectedItem?.ToString() == "1024 × 600" && action?.Enabled == true, "Native USB action");
        var labels = Descendants(dialog).OfType<Label>().Select(l => l.Text).ToArray();
        Require(labels.Any(label => label.Contains("Desktop  640 × 480") && label.Contains("USB signal  1024 × 600")) &&
                labels.Contains("Match the Windows desktop to the USB signal to use the full resolution."), "Desktop and signal labels");
        Directory.CreateDirectory(directory);
        using var surface = new LayoutSurface {
            Opacity = 0, ShowInTaskbar = false, FormBorderStyle = FormBorderStyle.None, ClientSize = dialog.ClientSize,
            Font = dialog.Font, BackColor = dialog.BackColor, ForeColor = dialog.ForeColor
        };
        var content = dialog.Controls[0];
        surface.Controls.Add(content);
        surface.Show();
        surface.PerformLayout();
        Require(surface.ClientRectangle.Contains(surface.RectangleToClient(action!.RectangleToScreen(action.ClientRectangle))),
                "Native USB action is clipped");
        using var bitmap = new Bitmap(surface.ClientSize.Width, surface.ClientSize.Height);
        surface.DrawToBitmap(bitmap, surface.ClientRectangle);
        bitmap.Save(Path.Combine(directory, $"{palette}-native-usb.png"), ImageFormat.Png);
        dialog.Controls.Add(content);
        Console.WriteLine("PASS live USB desktop 640x480, signal 1024x600, native size selected and action enabled");
    }

    private static void CheckMainWindow(string? imageDirectory, string mode)
    {
        var settings = new AppSettings();
        var controllerType = typeof(AppSettings).Assembly.GetType("KVMpad.Client.App.ClientController", true)!;
        using var controller = (IDisposable)Activator.CreateInstance(controllerType, settings, new EmptyStore())!;
        using var form = Open("MainForm", settings, controller);
        // Lay out the production workspace with native handles without starting MainForm.Load (discovery).
        using var surface = new LayoutSurface {
            Opacity = 0, ShowInTaskbar = false, FormBorderStyle = FormBorderStyle.None,
            Font = form.Font, BackColor = form.BackColor, ForeColor = form.ForeColor
        };
        surface.Controls.Add(form.Controls[0]);
        surface.Show();
        var pages = Descendants(surface).OfType<TabControl>().Single();
        Require(pages.TabPages.Cast<TabPage>().Select(page => page.Text)
                .SequenceEqual(new[] { "Devices", "Host input", "Tools" }), "Client navigation");
        Require(Descendants(surface).OfType<Label>().Any(label => label.Text == "Not connected"), "Initial status");
        foreach (var size in new[] { new Size(464, 320), new Size(620, 380), new Size(760, 510) }) {
            surface.ClientSize = size;
            foreach (TabPage page in pages.TabPages) {
                pages.SelectedTab = page;
                _ = page.Handle;
                page.PerformLayout();
                var content = (TableLayoutPanel)page.Controls[0];
                content.PerformLayout();
                Require(surface.ClientRectangle.Contains(surface.RectangleToClient(
                            content.RectangleToScreen(content.ClientRectangle))), $"Page clipped: {size}/{page.Text}");
                string[] actions = page.Text switch {
                    "Devices" => ["Discover", "Pair…", "Connect", "Disconnect", "Start HOST", "Advanced…"],
                    "Host input" => ["Request control", "Release control", "Start capture", "Send shortcut", "Type text", "Cancel typing", "SSH command…"],
                    _ => ["USB display…", "Extension Studio…", "Prepare card video…", "About"]
                };
                var buttons = Descendants(page).OfType<Button>().ToArray();
                Require(buttons.Select(button => button.Text).SequenceEqual(actions), "Client action order");
                if (page.Text == "Host input")
                    Require(!buttons.Single(b => b.Text == "SSH command…").Enabled, "SSH waits for paired TLS");
                foreach (var button in buttons) {
                    var bounds = content.RectangleToClient(button.Parent!.RectangleToScreen(button.Bounds));
                    Require(bounds.Width > 0 && bounds.Height > 0 && content.ClientRectangle.Contains(bounds),
                            $"{page.Text}/{button.Text} clipped at {size}: {bounds} within {content.ClientRectangle}");
                }
                if (page.Text == "Devices")
                    Require(Descendants(page).OfType<ListView>().Single().Height >= 70, "Device rows clipped");
                if (page.Text == "Host input")
                    Require(Descendants(page).OfType<TextBox>().Single().Height >= 32, "Text area clipped");
                if (imageDirectory != null && size.Width == 620) {
                    Directory.CreateDirectory(imageDirectory);
                    using var bitmap = new Bitmap(surface.Width, surface.Height);
                    surface.DrawToBitmap(bitmap, surface.ClientRectangle);
                    bitmap.Save(Path.Combine(imageDirectory, $"{mode}-{page.Text}.png"), ImageFormat.Png);
                }
            }
        }
        Require(!form.Visible, "Client layout tests must not start discovery or capture.");
    }

    private sealed class LayoutSurface : Form
    {
        protected override bool ShowWithoutActivation => true;
    }

    private sealed class EmptyStore : ICredentialStore
    {
        public IReadOnlyList<PairedDevice> List() => Array.Empty<PairedDevice>();
        public PairedDevice? Find(string deviceId) => null;
        public void Save(PairedDevice device) => throw new InvalidOperationException();
        public bool Remove(string deviceId) => throw new InvalidOperationException();
        public int RemoveAll() => throw new InvalidOperationException();
    }

    private static IEnumerable<Control> Descendants(Control root)
    {
        foreach (Control child in root.Controls) {
            yield return child;
            // Native composite controls own the colors of their internal children.
            if (child is Panel)
                foreach (Control descendant in Descendants(child)) yield return descendant;
        }
    }

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
