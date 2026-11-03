// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Threading;
using System.Windows.Forms;
using KVMpad.Client.App.Interop;

namespace KVMpad.Client.App;

internal static class Program
{
    private const string SingleInstanceMutex = "Global\\KVMpad.Client.SingleInstance";

    [STAThread]
    private static int Main()
    {
        // Two instances would install two keyboard hooks and fight over the same credential entries, so only
        // one runs at a time.
        using var single = new Mutex(initiallyOwned: true, SingleInstanceMutex, out bool first);
        if (!first)
        {
            MessageBox.Show(
                "KVMpad Client is already running. Use its tray icon.",
                "KVMpad Client",
                MessageBoxButtons.OK,
                MessageBoxIcon.Information);
            return 0;
        }

        Application.SetHighDpiMode(HighDpiMode.PerMonitorV2);
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);

        AppSettings settings = AppSettings.Load();
        var store = new CredentialManagerStore();

        using var controller = new ClientController(settings, store);
        using var form = new MainForm(settings, controller);

        Application.ThreadException += (_, e) => ReportFault(e.Exception);
        AppDomain.CurrentDomain.UnhandledException += (_, e) =>
        {
            // A crash must not leave keys held on the host. The controller's dispose path releases them, and
            // the device runs RELEASE_ALL on the session loss regardless (AGENTS §2.3).
            controller.Capture?.ExitCapture(Core.Input.ReleaseReason.Shutdown);
            if (e.ExceptionObject is Exception exception)
                ReportFault(exception);
        };

        Application.Run(form);
        return 0;
    }

    private static void ReportFault(Exception exception)
    {
        // The message is shown, not written to a file: a log of this client must never risk containing
        // keystrokes or token material (TECH-046, SEC-105), and an exception message is the one thing here
        // whose contents are not under this code's control.
        MessageBox.Show(
            "KVMpad Client hit an unexpected error and released every key it was holding.\r\n\r\n"
            + exception.GetType().Name,
            "KVMpad Client",
            MessageBoxButtons.OK,
            MessageBoxIcon.Error);
    }
}
