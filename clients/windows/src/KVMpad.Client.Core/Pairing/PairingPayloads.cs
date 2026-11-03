// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Text.Json;
using KVMpad.Client.Core.Protocol;

namespace KVMpad.Client.Core.Pairing;

/// <summary>
/// <c>pair.hello</c> (ADR-029 §4, message 1): the wire version, the algorithm, the client name, the client
/// identifier and the certificate hash the client saw in this handshake.
/// </summary>
/// <remarks>
/// It carries no token and no code. The code never appears on the wire in any form — it only ever enters the
/// password derivation inside the PAKE (SEC-105). The certificate hash is reported here so the device can
/// refuse a substituted certificate outright, before a run starts, and a probe cannot burn the legitimate
/// user's three attempts (ADR-029 §2).
/// </remarks>
public readonly struct PairHelloPayload : IClientPayload
{
    public PairHelloPayload(int wireVersion, string pakeAlgorithm, string clientName, string clientIdHex,
        string certificateSha256Hex)
    {
        WireVersion = wireVersion;
        PakeAlgorithm = pakeAlgorithm ?? throw new ArgumentNullException(nameof(pakeAlgorithm));
        ClientName = clientName ?? throw new ArgumentNullException(nameof(clientName));
        ClientIdHex = clientIdHex ?? throw new ArgumentNullException(nameof(clientIdHex));
        CertificateSha256Hex = certificateSha256Hex ?? throw new ArgumentNullException(nameof(certificateSha256Hex));
    }

    public int WireVersion { get; }
    public string PakeAlgorithm { get; }
    public string ClientName { get; }
    public string ClientIdHex { get; }
    public string CertificateSha256Hex { get; }

    public void WriteTo(Utf8JsonWriter writer)
    {
        writer.WriteStartObject();
        writer.WriteNumber("v", WireVersion);
        writer.WriteString("pake", PakeAlgorithm);
        writer.WriteString("client_name", ClientName);
        writer.WriteString("client_id", ClientIdHex);
        writer.WriteString("cert_sha256", CertificateSha256Hex);
        writer.WriteEndObject();
    }
}

/// <summary>
/// A round blob, base64 in a <c>blob</c> field (ADR-029 §4, messages 3 and 5). A P-256 round 1 is at most
/// 330 bytes, so its base64 form stays well inside the 1024-byte envelope limit of 03 §6.5.
/// </summary>
public readonly struct PairBlobPayload : IClientPayload
{
    public PairBlobPayload(byte[] blob) => Blob = blob ?? throw new ArgumentNullException(nameof(blob));

    public byte[] Blob { get; }

    public void WriteTo(Utf8JsonWriter writer)
    {
        writer.WriteStartObject();
        writer.WriteBase64String("blob", Blob);
        writer.WriteEndObject();
    }
}

/// <summary>
/// A key-confirmation tag, base64 in a <c>mac</c> field (ADR-029 §4, message 6). The client sends this first:
/// the device verifies before it reveals its own tag and before anything is stored, so a wrong code costs one
/// attempt and yields nothing.
/// </summary>
public readonly struct PairMacPayload : IClientPayload
{
    public PairMacPayload(byte[] mac) => Mac = mac ?? throw new ArgumentNullException(nameof(mac));

    public byte[] Mac { get; }

    public void WriteTo(Utf8JsonWriter writer)
    {
        writer.WriteStartObject();
        writer.WriteBase64String("mac", Mac);
        writer.WriteEndObject();
    }
}
