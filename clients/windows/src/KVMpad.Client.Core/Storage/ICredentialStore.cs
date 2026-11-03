// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System.Collections.Generic;

namespace KVMpad.Client.Core.Storage;

/// <summary>
/// Where paired devices are kept. The implementation is the Windows Credential Manager, holding a
/// DPAPI-protected blob per device; a plain file is not an option (ADR-022 SEC-105).
/// </summary>
public interface ICredentialStore
{
    /// <summary>Every paired device on this machine, for the device list.</summary>
    IReadOnlyList<PairedDevice> List();

    PairedDevice? Find(string deviceId);

    /// <summary>Stores or replaces a device's record.</summary>
    void Save(PairedDevice device);

    /// <summary>
    /// Forgets a device. Also the recovery path when a pin stops matching: the user removes the entry and
    /// pairs again, which is the intended answer to a device whose certificate changed.
    /// </summary>
    bool Remove(string deviceId);

    /// <summary>Forgets every device.</summary>
    int RemoveAll();
}
