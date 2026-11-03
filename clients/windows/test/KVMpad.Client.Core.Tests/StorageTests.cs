// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Text;
using KVMpad.Client.Core;
using KVMpad.Client.Core.Session;
using KVMpad.Client.Core.Storage;
using Xunit;

namespace KVMpad.Client.Core.Tests;

/// <summary>
/// The paired-device record. The Windows credential store itself is not exercised here — these tests must not
/// write to the user's vault — but the blob that goes into it is, including that a malformed one is treated as
/// absent rather than half-trusted.
/// </summary>
public sealed class StorageTests
{
    private const string Pin = "3b7f1c90ad42e85f6c1b09d7e3a4f2851907cbd64e2a3f58b0c7d19e46af8253";

    private static PairedDevice Sample() => new(
        "KVMpad-7F3A",
        "Lab device",
        "192.168.1.50",
        8443,
        Pin,
        "dGhpcy1pcy1hLXRlc3QtdG9rZW4tdmFsdWUtMzJi",
        new DateTimeOffset(2026, 11, 2, 12, 0, 0, TimeSpan.Zero));

    [Fact]
    public void ARecordRoundTripsThroughItsBlob()
    {
        PairedDevice original = Sample();

        PairedDevice? restored = PairedDevice.FromJson(original.ToJson());

        Assert.NotNull(restored);
        Assert.Equal(original.DeviceId, restored!.DeviceId);
        Assert.Equal(original.DeviceName, restored.DeviceName);
        Assert.Equal(original.Host, restored.Host);
        Assert.Equal(original.Port, restored.Port);
        Assert.Equal(original.PinnedFingerprint, restored.PinnedFingerprint);
        Assert.Equal(original.Token, restored.Token);
        Assert.Equal(original.PairedAt, restored.PairedAt);
    }

    [Fact]
    public void ARecordCannotExistWithoutAPinOrAToken()
    {
        Assert.Throws<ArgumentException>(() =>
            new PairedDevice("id", "name", "host", 1, "nope", "token", DateTimeOffset.UtcNow));
        Assert.Throws<ArgumentException>(() =>
            new PairedDevice("id", "name", "host", 1, Pin, "", DateTimeOffset.UtcNow));
        Assert.Throws<ArgumentException>(() =>
            new PairedDevice("", "name", "host", 1, Pin, "token", DateTimeOffset.UtcNow));
    }

    [Theory]
    [InlineData("not json at all")]
    [InlineData("{}")]
    [InlineData("""{"device_id":"a"}""")]
    [InlineData("""{"device_id":"a","pin_sha256":"short","token":"t"}""")]
    [InlineData("[]")]
    public void AMalformedBlobIsTreatedAsAbsent(string json)
    {
        // A record the client cannot verify is not a record it half-uses: the user pairs again, which is cheap.
        Assert.Null(PairedDevice.FromJson(Encoding.UTF8.GetBytes(json)));
    }

    [Fact]
    public void TheDescriptionCarriesNoSecret()
    {
        PairedDevice device = Sample();

        string description = device.ToString();

        Assert.Contains("Lab device", description);
        Assert.DoesNotContain(device.Token, description);
        Assert.DoesNotContain(device.PinnedFingerprint, description);
    }

    [Fact]
    public void MovingToANewAddressKeepsThePinAndTheToken()
    {
        // mDNS may report a new address after a DHCP lease change; that must not require pairing again, and it
        // must not loosen the pin.
        PairedDevice moved = Sample().WithEndpoint("192.168.1.77", 9443);

        Assert.Equal("192.168.1.77", moved.Host);
        Assert.Equal(9443, moved.Port);
        Assert.Equal(Pin, moved.PinnedFingerprint);
        Assert.Equal(Sample().Token, moved.Token);
    }

    [Fact]
    public void TheEndpointBuiltFromARecordIsPinned()
    {
        SessionEndpoint endpoint = Sample().ToEndpoint();

        Assert.Equal(Pin, endpoint.PinnedFingerprint);
        Assert.StartsWith("wss://", endpoint.SessionUri.ToString());
    }

    [Fact]
    public void TheLicenseNoticeStringsAreTheExactTextTheProjectRequires()
    {
        // ADR-025: the three lines are fixed text; a near-miss in a binary is a licensing problem, not a typo.
        Assert.Equal("PolyForm-Noncommercial-1.0.0", LicenseNotice.SpdxIdentifier);
        Assert.Equal(
            "Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.",
            LicenseNotice.Copyright);
        Assert.Equal(
            "Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)",
            LicenseNotice.RequiredNotice);
        Assert.Contains(LicenseNotice.RequiredNotice, LicenseNotice.AboutBlock);
        Assert.Contains(LicenseNotice.SpdxIdentifier, LicenseNotice.AboutBlock);
    }
}
