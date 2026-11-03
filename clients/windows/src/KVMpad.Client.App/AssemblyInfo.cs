// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System.Reflection;
using System.Runtime.InteropServices;
using KVMpad.Client.Core;

// ADR-025 TECH-084. <Copyright> in Directory.Build.props becomes AssemblyCopyrightAttribute and the Win32
// version-resource field LegalCopyright. The Required Notice has no standard resource field, so it goes in as
// assembly metadata and as a string constant, which puts it in the string heap where `strings` finds it —
// the same guarantee the firmware gets from its retained .rodata string.
[assembly: AssemblyMetadata("RequiredNotice", LicenseNotice.RequiredNotice)]
[assembly: AssemblyMetadata("SPDX-License-Identifier", LicenseNotice.SpdxIdentifier)]
[assembly: AssemblyMetadata("ProjectUrl", LicenseNotice.ProjectUrl)]

[assembly: ComVisible(false)]
