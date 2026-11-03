// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

namespace KVMpad.Client.Core;

/// <summary>
/// The license strings the shipped binary must carry (ADR-025 TECH-084, FR-751).
/// They are plain constants so that they land in the assembly's string heap and
/// <c>strings KVMpad.Client.exe</c> finds them, exactly as <c>strings app.bin</c> does for the firmware.
/// </summary>
public static class LicenseNotice
{
    public const string SpdxIdentifier = "PolyForm-Noncommercial-1.0.0";

    public const string Copyright = "Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.";

    public const string RequiredNotice =
        "Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)";

    public const string LicenseName = "PolyForm Noncommercial License 1.0.0";

    public const string ProjectUrl = "https://github.com/D-Ogi/esp32-s3-kvmpad";

    /// <summary>The three lines an About dialog shows verbatim (FR-751).</summary>
    public static string AboutBlock =>
        "SPDX-License-Identifier: " + SpdxIdentifier + "\r\n" + Copyright + "\r\n" + RequiredNotice;
}
