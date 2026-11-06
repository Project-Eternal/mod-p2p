/*
 * The peer host: what the module needs from the platform under it, and nothing about cryptography or the game. In the browser it is
 * web/p2p.js (an MQTT client over WebSocket for rooms and introductions, WebRTC data channels for direct links); a native server plugs in a
 * C MQTT client and libdatachannel; tests plug in an exchange inside the process (TestNet.h). It moves opaque bytes.
 *
 * A host is not thread-safe: the module calls it from its one thread. Commands and events also have a byte form (Encode/Decode below)
 * for hosts that live in another place, such as the page's JavaScript behind shared-memory rings; web/p2p.js implements the same format.
 */
#pragma once
#include "Codec.h"

namespace p2p
{
    struct HostEvent
    {
        enum Kind : uint8_t
        {
            NodeUp = 1,        // the relay node is connected (rooms joined before are joined again by the host)
            NodeDown = 2,
            RoomMessage = 3,   // 'name' = the room (or the gone topic), 'data' = what was published there
            InboxMessage = 4,  // 'name' = the sender's inbox, 'data' = a frame sent to ours (relayed, or from a direct link)
            LinkUp = 5,        // 'name' = the peer's inbox; 'direct' = a direct link, else relayed through the node
            LinkDown = 6,
            DirectFailed = 7,  // 'name' = the peer's inbox; 'reason' = why the direct connection did not open (DirectReason)
            NodeList = 9,      // the host's list of nodes after a change (and at the open): 'data' = the addresses, one per line, an empty line for a removed node
            NodeFailed = 8,    // a node could not be reached: 'node' = its index, 'name' = its address, 'data' = the reason in words
        } kind = NodeUp;
        std::string name;
        Bytes data;
        bool direct = false;
        // several relay nodes (protocol version 2): NodeUp/NodeDown name the node by its address in 'name' and by 'node' (its index in the host's list);
        // RoomMessage says which node delivered it in 'node'; InboxMessage says which route brought the frame in 'route'
        uint8_t node = 0, route = 0, reason = 0;
    };

    // Routes to one peer: the direct WebRTC link, or a relay through node k of the host's list
    constexpr uint8_t RouteDirect = 0;
    constexpr uint8_t RouteAny = 255;    // the host chooses (direct if open, else the first node that is up)
    constexpr uint8_t MaxNodes = 8;
    inline uint8_t RouteOfNode(unsigned node) { return static_cast<uint8_t>(node + 1); }
    inline bool RouteIsNode(uint8_t route) { return route >= 1 && route <= MaxNodes; }

    // Why a direct connection to a peer is not open
    enum DirectReason : uint8_t { DirectNotTried = 0, DirectNoPath = 1, DirectTimedOut = 2, DirectDisabled = 3, DirectOpen = 4 };

    struct HostCommand
    {
        enum Kind : uint8_t
        {
            Open = 1,        // connect to the node: 'name' = my inbox (hex), 'text' = realm prefix, 'data' = the last will (published on the gone topic)
            Close = 2,
            Join = 3,        // 'name' = room
            Leave = 4,
            Publish = 5,     // 'name' = room, 'data'
            Watch = 6,       // 'name' = a topic to listen to (the gone topic of a peer)
            Unwatch = 7,
            Send = 8,        // 'name' = peer inbox, 'data' = frame, 'channel' = 0 reliable / 1 unreliable
            Connect = 9,     // try a direct link to the peer 'name' (this side makes the offer)
            Disconnect = 10,
            Will = 11,       // publish the last will now, on every node (before a clean close: a broker does not publish the will of a clean disconnect)
            NodeAdd = 13,    // for this session: 'name' = the address of a node to connect to (it gets the next number); the host answers with a NodeList event
            NodeRemove = 14, // for this session: 'route' = the number (index) of the node to leave; the numbers of the others do not change
            Quality = 12,    // for the page's shell, not for a node: 'text' = "state latency-ms loss count" (core/Quality.h; state around / everyone / none)
        } kind = Open;
        std::string name, text;
        Bytes data;
        uint8_t channel = 0;
        uint8_t route = RouteAny;   // Send: the route that carries the frame
    };

    Bytes Encode(HostEvent const& e);
    bool Decode(uint8_t const* p, size_t n, HostEvent& e);
    Bytes Encode(HostCommand const& c);
    bool Decode(uint8_t const* p, size_t n, HostCommand& c);

    class PeerHost
    {
    public:
        virtual ~PeerHost() = default;
        virtual void Command(HostCommand const& c) = 0;
        virtual bool Poll(HostEvent& e) = 0;   // false when there is none now
    };
}
