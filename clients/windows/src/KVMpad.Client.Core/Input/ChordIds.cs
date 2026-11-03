// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;

namespace KVMpad.Client.Core.Input;

/// <summary>
/// The <c>command_id</c> allowlist for <c>input</c>/<c>shortcut</c>, mirroring the <c>name</c> column of
/// <c>k_chords[]</c> in <c>components/hid_core/src/hid_keymap.c</c>. The client sends an id and never a key
/// array, so it cannot assemble a chord the device has not approved (03 §3 rule 2, TECH-045).
/// </summary>
public static class ChordIds
{
    public const string CtrlAltDel = "ctrl_alt_del";          // FR-301
    public const string AltTab = "alt_tab";                   // FR-302
    public const string TaskManager = "task_manager";         // FR-303
    public const string ShowDesktop = "show_desktop";         // FR-304
    public const string CloseWindow = "close_window";         // FR-305
    public const string Screenshot = "screenshot";            // FR-306
    public const string LayoutSwitchWin = "layout_switch_win";   // FR-744
    public const string LayoutSwitchCtrl = "layout_switch_ctrl"; // FR-744
    public const string MediaPlayPause = "media_play_pause";  // ADR-018 / FR-311
    public const string MediaNext = "media_next";
    public const string MediaPrev = "media_prev";
    public const string MediaMute = "media_mute";
    public const string MediaVolumeUp = "media_volume_up";
    public const string MediaVolumeDown = "media_volume_down";

    private static readonly string[] AllIds =
    [
        CtrlAltDel, AltTab, TaskManager, ShowDesktop, CloseWindow, Screenshot,
        LayoutSwitchWin, LayoutSwitchCtrl,
        MediaPlayPause, MediaNext, MediaPrev, MediaMute, MediaVolumeUp, MediaVolumeDown,
    ];

    private static readonly HashSet<string> Allowlist = new(AllIds, StringComparer.Ordinal);

    /// <summary>Every allowlisted id, in the order the firmware enumerates them.</summary>
    public static IReadOnlyList<string> All => AllIds;

    /// <summary>True when the device's chord table contains this id.</summary>
    public static bool IsAllowlisted(string? commandId) => commandId is not null && Allowlist.Contains(commandId);

    /// <summary>An English label for the UI. Not sent on the wire; the id is what travels.</summary>
    public static string Label(string commandId) => commandId switch
    {
        CtrlAltDel => "Ctrl+Alt+Del",
        AltTab => "Alt+Tab",
        TaskManager => "Task Manager",
        ShowDesktop => "Show desktop",
        CloseWindow => "Close window",
        Screenshot => "Screenshot",
        LayoutSwitchWin => "Switch host layout (Win)",
        LayoutSwitchCtrl => "Switch host layout (Ctrl)",
        MediaPlayPause => "Play / Pause",
        MediaNext => "Next track",
        MediaPrev => "Previous track",
        MediaMute => "Mute",
        MediaVolumeUp => "Volume up",
        MediaVolumeDown => "Volume down",
        _ => commandId,
    };
}
