// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

using System;
using System.Collections.Generic;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;
using KVMpad.Client.Core.Protocol;

namespace KVMpad.Client.Core.Discovery;

/// <summary>
/// Browses <c>_kvmpad._tcp.local</c> over multicast DNS, in process, with no new system service (FR-723).
/// </summary>
/// <remarks>
/// The socket joins the multicast group with address reuse set, so running alongside the Windows DNS-SD stack
/// or another responder does not fail and does not interfere. A datagram that fails to parse is dropped and
/// counted: this is unauthenticated data from the LAN and the only thing it is allowed to do is put a name in
/// a list. Nothing here trusts a discovered device — trust comes from the pairing run and the pin.
/// </remarks>
public sealed class MdnsBrowser
{
    private readonly string _serviceType;

    public MdnsBrowser(string serviceType = ProtocolLimits.ServiceType)
        => _serviceType = serviceType;

    /// <summary>Datagrams received that could not be parsed. A diagnostic counter only.</summary>
    public int MalformedDatagramCount { get; private set; }

    /// <summary>
    /// Sends a query and collects answers for <paramref name="listenFor"/>. Two queries go out, half a second
    /// apart, because a single multicast packet is allowed to be lost.
    /// </summary>
    public async Task<IReadOnlyList<DiscoveredDevice>> BrowseAsync(
        TimeSpan listenFor,
        CancellationToken cancellationToken)
    {
        var responses = new List<MdnsResponse>();
        byte[] query = MdnsMessage.BuildPointerQuery(_serviceType);
        var group = new IPEndPoint(IPAddress.Parse(MdnsMessage.MulticastAddressV4), MdnsMessage.Port);

        using var socket = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
        socket.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
        socket.Bind(new IPEndPoint(IPAddress.Any, MdnsMessage.Port));

        // A PC with virtual adapters (Hyper-V, VMware, VPN) sends a multicast datagram only on its default
        // multicast interface, which is often not the LAN the device is on. So the group is joined and the query
        // sent on every IPv4 interface that is up, not on whichever one the stack happens to pick.
        IReadOnlyList<IPAddress> interfaces = MulticastInterfaces();
        IPAddress groupAddress = IPAddress.Parse(MdnsMessage.MulticastAddressV4);
        bool joined = false;
        foreach (IPAddress local in interfaces)
            joined |= TryJoin(socket, groupAddress, local);
        if (!joined)
            TryJoin(socket, groupAddress, IPAddress.Any);
        socket.SetSocketOption(SocketOptionLevel.IP, SocketOptionName.MulticastTimeToLive, 1);

        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        deadline.CancelAfter(listenFor);

        Task sender = SendQueriesAsync(socket, query, group, interfaces, deadline.Token);
        var buffer = new byte[4096];

        try
        {
            while (!deadline.Token.IsCancellationRequested)
            {
                SocketReceiveFromResult result = await socket
                    .ReceiveFromAsync(buffer, SocketFlags.None, new IPEndPoint(IPAddress.Any, 0), deadline.Token)
                    .ConfigureAwait(false);

                try
                {
                    responses.Add(MdnsMessage.Parse(buffer.AsSpan(0, result.ReceivedBytes)));
                }
                catch (MdnsFormatException)
                {
                    MalformedDatagramCount++;
                }
            }
        }
        catch (OperationCanceledException)
        {
            // The listen window closed; whatever arrived is the result.
        }
        catch (SocketException)
        {
            // The interface went away mid-browse; return what we have.
        }

        try
        {
            await sender.ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            // Expected once the window closes.
        }

        return DeviceCollator.Collate(responses, _serviceType);
    }

    /// <summary>IPv4 addresses of the interfaces that are up and multicast-capable, loopback excluded.</summary>
    public static IReadOnlyList<IPAddress> MulticastInterfaces()
    {
        var result = new List<IPAddress>();
        NetworkInterface[] all;
        try
        {
            all = NetworkInterface.GetAllNetworkInterfaces();
        }
        catch (NetworkInformationException)
        {
            return result;
        }
        foreach (NetworkInterface nic in all)
        {
            if (nic.OperationalStatus != OperationalStatus.Up
                || nic.NetworkInterfaceType == NetworkInterfaceType.Loopback
                || !nic.SupportsMulticast)
                continue;
            foreach (UnicastIPAddressInformation address in nic.GetIPProperties().UnicastAddresses)
            {
                if (address.Address.AddressFamily == AddressFamily.InterNetwork)
                    result.Add(address.Address);
            }
        }
        return result;
    }

    private static bool TryJoin(Socket socket, IPAddress group, IPAddress local)
    {
        try
        {
            socket.SetSocketOption(SocketOptionLevel.IP, SocketOptionName.AddMembership, new MulticastOption(group, local));
            return true;
        }
        catch (SocketException)
        {
            // This interface refuses the join; the others (or a unicast-to-multicast query) may still answer.
            return false;
        }
    }

    private static async Task SendQueriesAsync(
        Socket socket,
        byte[] query,
        IPEndPoint group,
        IReadOnlyList<IPAddress> interfaces,
        CancellationToken cancellationToken)
    {
        for (int attempt = 0; attempt < 2 && !cancellationToken.IsCancellationRequested; attempt++)
        {
            bool sent = false;
            foreach (IPAddress local in interfaces)
            {
                try
                {
                    socket.SetSocketOption(SocketOptionLevel.IP, SocketOptionName.MulticastInterface, local.GetAddressBytes());
                    await socket.SendToAsync(query, SocketFlags.None, group, cancellationToken).ConfigureAwait(false);
                    sent = true;
                }
                catch (SocketException)
                {
                    // An interface that cannot send (no route, adapter going down) must not stop the others.
                }
            }
            if (!sent)
            {
                try
                {
                    await socket.SendToAsync(query, SocketFlags.None, group, cancellationToken).ConfigureAwait(false);
                }
                catch (SocketException)
                {
                    return;
                }
            }
            await Task.Delay(TimeSpan.FromMilliseconds(500), cancellationToken).ConfigureAwait(false);
        }
    }
}
