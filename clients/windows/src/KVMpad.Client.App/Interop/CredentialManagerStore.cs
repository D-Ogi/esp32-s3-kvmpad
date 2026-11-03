// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using KVMpad.Client.Core.Storage;

namespace KVMpad.Client.App.Interop;

/// <summary>
/// Paired devices in the Windows Credential Manager, each as a DPAPI-protected blob.
/// </summary>
/// <remarks>
/// <para>
/// ADR-022 SEC-105 requires the token to live in the Credential Manager or under DPAPI rather than in a
/// configuration file. This uses both: the record is sealed with <see cref="ProtectedData"/> scoped to the
/// current user before it is written as a generic credential. The Credential Manager gives the entry a name,
/// a lifetime and a place the user can inspect and delete it from; the DPAPI layer means that a backup or a
/// dump of the vault is still ciphertext outside this user's logon.
/// </para>
/// <para>
/// The persistence is <c>LOCAL_MACHINE</c>, so the credential does not travel with a roaming profile: a token
/// is tied to the machine the user paired from, and pairing on a second machine is a second deliberate act.
/// </para>
/// </remarks>
internal sealed class CredentialManagerStore : ICredentialStore
{
    private const string TargetPrefix = "KVMpad:device:";

    /// <summary>Entropy mixed into the DPAPI protection, so a blob from another application cannot be swapped in.</summary>
    private static readonly byte[] Entropy = System.Text.Encoding.ASCII.GetBytes("KVMpad-client-v1");

    public IReadOnlyList<PairedDevice> List()
    {
        var devices = new List<PairedDevice>();
        if (!NativeMethods.CredEnumerate(TargetPrefix + "*", 0, out uint count, out IntPtr array))
        {
            // 1168 = ERROR_NOT_FOUND: an empty vault, not a failure.
            int error = Marshal.GetLastWin32Error();
            if (error is 1168 or 0)
                return devices;
            throw new Win32Exception(error, "Could not read the credential store.");
        }

        try
        {
            for (uint i = 0; i < count; i++)
            {
                IntPtr entryPtr = Marshal.ReadIntPtr(array, (int)(i * (uint)IntPtr.Size));
                var entry = Marshal.PtrToStructure<NativeMethods.CREDENTIAL>(entryPtr);
                PairedDevice? device = ReadRecord(entry);
                if (device is not null)
                    devices.Add(device);
            }
        }
        finally
        {
            NativeMethods.CredFree(array);
        }

        devices.Sort(static (a, b) => string.Compare(a.DeviceName, b.DeviceName, StringComparison.OrdinalIgnoreCase));
        return devices;
    }

    public PairedDevice? Find(string deviceId)
    {
        if (!NativeMethods.CredRead(TargetPrefix + deviceId, NativeMethods.CRED_TYPE_GENERIC, 0, out IntPtr ptr))
            return null;
        try
        {
            var entry = Marshal.PtrToStructure<NativeMethods.CREDENTIAL>(ptr);
            return ReadRecord(entry);
        }
        finally
        {
            NativeMethods.CredFree(ptr);
        }
    }

    public void Save(PairedDevice device)
    {
        if (device is null)
            throw new ArgumentNullException(nameof(device));

        byte[] plaintext = device.ToJson();
        byte[] sealedBlob;
        try
        {
            sealedBlob = ProtectedData.Protect(plaintext, Entropy, DataProtectionScope.CurrentUser);
        }
        finally
        {
            CryptographicOperations.ZeroMemory(plaintext);
        }

        IntPtr blob = Marshal.AllocHGlobal(sealedBlob.Length);
        IntPtr target = Marshal.StringToCoTaskMemUni(TargetPrefix + device.DeviceId);
        IntPtr userName = Marshal.StringToCoTaskMemUni(device.DeviceName);
        IntPtr comment = Marshal.StringToCoTaskMemUni("KVMpad paired device");
        try
        {
            Marshal.Copy(sealedBlob, 0, blob, sealedBlob.Length);
            var credential = new NativeMethods.CREDENTIAL
            {
                Flags = 0,
                Type = NativeMethods.CRED_TYPE_GENERIC,
                TargetName = target,
                Comment = comment,
                CredentialBlobSize = (uint)sealedBlob.Length,
                CredentialBlob = blob,
                Persist = NativeMethods.CRED_PERSIST_LOCAL_MACHINE,
                AttributeCount = 0,
                Attributes = IntPtr.Zero,
                TargetAlias = IntPtr.Zero,
                UserName = userName,
            };

            if (!NativeMethods.CredWrite(ref credential, 0))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not store the pairing in the credential store.");
        }
        finally
        {
            CryptographicOperations.ZeroMemory(sealedBlob);
            Marshal.FreeHGlobal(blob);
            Marshal.FreeCoTaskMem(target);
            Marshal.FreeCoTaskMem(userName);
            Marshal.FreeCoTaskMem(comment);
        }
    }

    public bool Remove(string deviceId)
        => NativeMethods.CredDelete(TargetPrefix + deviceId, NativeMethods.CRED_TYPE_GENERIC, 0);

    public int RemoveAll()
    {
        int removed = 0;
        foreach (PairedDevice device in List())
        {
            if (Remove(device.DeviceId))
                removed++;
        }
        return removed;
    }

    private static PairedDevice? ReadRecord(NativeMethods.CREDENTIAL entry)
    {
        if (entry.CredentialBlob == IntPtr.Zero || entry.CredentialBlobSize == 0)
            return null;

        byte[] sealedBlob = new byte[entry.CredentialBlobSize];
        Marshal.Copy(entry.CredentialBlob, sealedBlob, 0, sealedBlob.Length);

        byte[]? plaintext = null;
        try
        {
            plaintext = ProtectedData.Unprotect(sealedBlob, Entropy, DataProtectionScope.CurrentUser);
            return PairedDevice.FromJson(plaintext);
        }
        catch (CryptographicException)
        {
            // Written by another user, or tampered with. Treated as absent: the user pairs again, which is
            // cheap, rather than the client acting on a record it cannot verify.
            return null;
        }
        finally
        {
            CryptographicOperations.ZeroMemory(sealedBlob);
            if (plaintext is not null)
                CryptographicOperations.ZeroMemory(plaintext);
        }
    }
}
