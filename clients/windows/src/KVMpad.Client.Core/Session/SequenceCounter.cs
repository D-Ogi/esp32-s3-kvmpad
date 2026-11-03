// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;

namespace KVMpad.Client.Core.Session;

/// <summary>
/// The client's <c>seq</c>: a uint64 that only ever increases within one session and resets only when a new
/// session begins (03 §6.2). The device answers <c>seq &lt;= last_seq</c> with <c>ERR_REPLAY</c> and sends no
/// HID, so a counter that repeats a number costs the session.
/// </summary>
public sealed class SequenceCounter
{
    private ulong _last;

    /// <summary>The highest number handed out so far; 0 before the first call.</summary>
    public ulong Last => _last;

    /// <summary>The next number. The first call in a session returns 1.</summary>
    public ulong Next()
    {
        if (_last >= (1UL << 53) - 1)
            throw new InvalidOperationException("seq exhausted the safe JSON integer range; the session must be renewed.");
        return ++_last;
    }

    /// <summary>
    /// Starts a new session's numbering. This is the only legitimate way back to 1, and it must be paired with
    /// discarding every event queued under the old session: re-sending an old event after a reconnect is
    /// forbidden (03 §6.4).
    /// </summary>
    public void ResetForNewSession() => _last = 0;
}
