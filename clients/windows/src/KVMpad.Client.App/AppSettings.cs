// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.IO;
using System.Security.Cryptography;
using System.Text.Json;
using KVMpad.Client.Core.Input;
using KVMpad.Client.Core.Pairing;
using KVMpad.Client.Core.Protocol;

namespace KVMpad.Client.App;

/// <summary>
/// The client's own preferences.
/// </summary>
/// <remarks>
/// Nothing here is a security policy. The device's approval policies are settings on the device and only the
/// device, changed by a tap on its screen; the protocol has no <c>settings.*</c> command and this client never
/// tries to send one (ADR-023 SEC-107). What lives here is local convenience: which key toggles capture, how
/// fast to type text, which host layout to assume. Secrets never land in this file — tokens and pins are in
/// the credential store (SEC-105).
/// </remarks>
public sealed class AppSettings
{
    /// <summary>The capture toggle's virtual-key code. Scroll Lock by default (ADR-021).</summary>
    public int ToggleVirtualKey { get; set; } = VirtualKey.ScrollLock;

    public string ToggleKeyName { get; set; } = "Scroll Lock";

    /// <summary>Whether capture ends when the client's window loses activation.</summary>
    public bool ExitCaptureWhenDeactivated { get; set; } = true;

    public bool NaturalScroll { get; set; }

    /// <summary>The HOST layout "type text" encodes for. The user picks it; the device also reports its own.</summary>
    public string HostLayoutId { get; set; } = "us";

    /// <summary>Type-text rate; clamped to the ADR-019 ceiling when loaded.</summary>
    public int TypeTextMessagesPerSecond { get; set; } = ProtocolLimits.DefaultTypeTextMessagesPerSecond;

    /// <summary>The device id to offer first on startup.</summary>
    public string? LastDeviceId { get; set; }

    /// <summary>
    /// The stable per-installation identifier of ADR-029 §4, 32 hex characters, generated once and then kept.
    /// </summary>
    /// <remarks>
    /// It is not a secret — it travels in <c>pair.hello</c> in the clear — but it is bound into the pairing
    /// channel binding, so it has to be the same value on every attempt from this installation. A new value
    /// would simply mean the device treats this client as a different one.
    /// </remarks>
    public string ClientId { get; set; } = string.Empty;

    private static string FilePath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "KVMpad",
        "client-settings.json");

    public static AppSettings Load()
    {
        try
        {
            string path = FilePath;
            if (!File.Exists(path))
                return Validate(new AppSettings());
            AppSettings? settings = JsonSerializer.Deserialize<AppSettings>(File.ReadAllBytes(path));
            return Validate(settings ?? new AppSettings());
        }
        catch (Exception ex) when (ex is IOException or JsonException or UnauthorizedAccessException)
        {
            // An unreadable settings file falls back to the defaults rather than blocking startup; the
            // defaults are the safe values, so there is nothing to recover.
            return Validate(new AppSettings());
        }
    }

    public void Save()
    {
        try
        {
            string path = FilePath;
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllBytes(path, JsonSerializer.SerializeToUtf8Bytes(this, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            // Preferences are not worth failing an action over.
        }
    }

    /// <summary>Brings a loaded file back inside the limits, because a file on disk is not a trusted input.</summary>
    private static AppSettings Validate(AppSettings settings)
    {
        if (settings.TypeTextMessagesPerSecond < 1
            || settings.TypeTextMessagesPerSecond > ProtocolLimits.MaxTypeTextMessagesPerSecond)
            settings.TypeTextMessagesPerSecond = ProtocolLimits.DefaultTypeTextMessagesPerSecond;
        if (settings.ToggleVirtualKey is < 1 or > 0xFF)
        {
            settings.ToggleVirtualKey = VirtualKey.ScrollLock;
            settings.ToggleKeyName = "Scroll Lock";
        }
        if (!IsWellFormedClientId(settings.ClientId))
        {
            // Missing on a first run, or corrupt in the file. Either way a fresh one is drawn and saved, and
            // the only consequence is that already-paired devices see a new client.
            settings.ClientId = NewClientId();
            settings.Save();
        }
        return settings;
    }

    private static bool IsWellFormedClientId(string? value)
    {
        if (value is null || value.Length != PairingContext.ClientIdBytes * 2)
            return false;
        foreach (char c in value)
        {
            if (c is not ((>= '0' and <= '9') or (>= 'a' and <= 'f')))
                return false;
        }
        return true;
    }

    private static string NewClientId()
        => Convert.ToHexStringLower(RandomNumberGenerator.GetBytes(PairingContext.ClientIdBytes));

    public CaptureOptions ToCaptureOptions() => new()
    {
        ToggleVirtualKey = ToggleVirtualKey,
        ExitCaptureWhenDeactivated = ExitCaptureWhenDeactivated,
        NaturalScroll = NaturalScroll,
    };
}
