/*
 * The node of the network inside one server: keeps the rooms the game wants, announces the local character in them, learns of the others
 * from their announcements, opens encrypted links to those the game wants to see, keeps the network clock, and delivers verified messages.
 * It knows nothing of the game: the 'application data' of announcements and the messages above type 15 are opaque to it.
 * Single threaded: every call is made by the module's thread; time is passed in (milliseconds of the machine's UTC clock).
 */
#pragma once
#include "Host.h"
#include "Link.h"
#include "Reliable.h"
#include "Rooms.h"
#include <array>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

namespace p2p
{
    struct IdHash { size_t operator()(Id128 const& id) const { size_t h; memcpy(&h, id.data(), sizeof h); return h; } };

    enum MessageType : uint16_t
    {
        MsgPing = 1, MsgPong = 2, MsgBye = 3, MsgKeepAlive = 4, MsgPresence = 5,
        MsgReliable = 6,    // the reliable channel's wrapper: u64 sequence, then the message (type and body)
        MsgAck = 7,         // u64 cumulative, u64 bitmap of the next 64
        MsgKeyed = 8,       // the keyed channel: u64 key, u64 version, then the message (type and body)
        MsgKeyedAck = 9,    // u16 count, then (u64 key, u64 version) pairs
        MsgFirstApplication = 16,
    };

    // What is measured of one route to a peer (see Node: probes)
    struct RouteStats
    {
        bool open = false;                     // direct only: the link is open
        int64_t heardAt = 0;                   // any frame, introduction or announcement that came through this route (machine ms)
        int64_t probeSentAt = 0, answeredAt = 0;
        std::deque<int32_t> results;           // the last 20 probes: the round trip in ms, or -1 when lost
        std::deque<std::pair<uint32_t, int64_t>> outstanding;   // (probe id, sent at)
        int better = 0;                        // consecutive evaluations in which this route was clearly better than the one in use
        int32_t Median() const;                // -1 when no probe was answered
        double Loss() const;                   // share of lost probes among the finished ones
        size_t Finished() const { return results.size(); }
    };

    // The string forms are for the status command and the log
    char const* DirectReasonName(uint8_t reason);

    struct Peer
    {
        Certificate cert;
        std::string inbox;
        Id128 session{};
        Bytes appData;              // its latest announcement
        std::set<std::string> rooms;   // the rooms we both are in
        int64_t lastSeen = 0, lastHeard = 0, wantedSince = 0, notWantedSince = 0;
        std::unique_ptr<Link> link;
        bool direct = false, helloSent = false, linkUp = false, wanted = false, offered = false;
        int64_t theta = 0, rtt = -1;   // the peer's network time minus ours, and the round trip, best of the recent samples
        std::deque<std::pair<int64_t, int64_t>> samples;
        int64_t lastPing = 0, linkUpAt = 0;
        std::string name;           // for logs
        // routes: [0] the direct link, [1 + k] a relay through node k of the host's list
        std::array<RouteStats, MaxNodes + 1> routes;
        int route = -1;             // the route in use, -1 when none
        int64_t routeSince = 0, lastSwitch = 0, lastEval = 0;
        uint8_t directReason = DirectNotTried;
        int64_t nextOffer = 0;
        int offers = 0;
        ReliableStream reliable;
        KeyedStream keyed;
        bool heardFrame = false;    // a frame of this link opened (until then our hello is repeated)
        bool answered = false;      // it answered us directly (a reply to an announcement): with a room in common that makes it "in touch"
        bool inTouch = false;       // the register: a link is up, or a room is shared and it answered (a peer we ignore never answers)
        bool lostLink = false;      // the link ended without being closed on purpose (the next one is "restored")
        int64_t lastHello = 0, lastReset = 0;
        unsigned helloRound = 0;
        uint32_t probeId = 0;
        uint64_t sent = 0, received = 0;
    };

    class Listener
    {
    public:
        virtual ~Listener() = default;
        // the game's answer to "may this character be seen/heard": false for the ignored, other realms' rules, phases that do not intersect...
        virtual bool Blocked(Certificate const& cert) { return false; }
        // does the game have a reason for a link to this peer (what it announced, the rooms in common)?
        virtual bool WantLink(Peer const& peer) = 0;
        virtual void OnPeerAnnounced(Peer& peer, bool isNew) { }   // an announcement arrived (new, or changed)
        virtual void OnLinkUp(Peer& peer) { }
        virtual void OnLinkDown(Peer& peer, std::string const& why) { }
        virtual void OnPeerGone(Peer& peer) { }
        virtual void OnInTouch(Peer& peer, bool inTouch) { }   // the one register: each change is raised once
        virtual void OnMessage(Peer& peer, uint16_t type, Reader& body, Channel channel) { }
        virtual void OnNodeState(bool up) { }     // true when at least one relay node is reached
        virtual void OnLog(std::string const& line) { }
        // An occasional change of the network, for the chat and the log. kind: node-reached, node-lost, node-back, route, link-lost, link-restored,
        // direct-failed. 'peer' is null for the nodes; 'text' is the sentence without the name of the player (the listener names players as it knows them)
        virtual void OnNetwork(std::string const& kind, Peer* peer, std::string const& text) { }
    };

    class Node
    {
    public:
        struct Config
        {
            uint32_t realm = 1;
            bool shard = false;                  // a private shard: the key derived from the identifier
            Key32 shardKey{};
            int64_t linkWantedGraceMs = 5000;    // how long nothing in common is tolerated before a link is closed
            int64_t announceEveryMs = 30000;
            int64_t maxSkewMs = 120000;
            int64_t maxSlewMs = 100;            // the clock moves at most this much per correction (every clockEveryMs)
            int64_t clockEveryMs = 2000;         // freshness window of signed announcements (network time)
            int64_t peerTimeoutMs = 60000;      // a link that was silent this long is dropped (probes keep a live one from being silent)
            int64_t probeEveryMs = 2000;        // on the route in use
            int64_t probeOtherEveryMs = 10000;  // on the other routes
            int64_t probeLostAfterMs = 4000;
            int64_t routeDeadAfterMs = 5000;    // the route in use that was silent this long is left at once
            int64_t routePauseMs = 30000;       // between two moves of one peer's connection
            int evalsNeeded = 3;                // a route must be clearly better in so many evaluations in a row
            int64_t nodeStableMs = 120000;      // a node that came back is "usable again" in the chat after it stayed up this long
            size_t maxRooms = 600;
        };

        Node(Config const& config, Account const& account, Certificate const& character, PeerHost& host, Listener& listener);

        // the character this server announces; the game calls this when the character or its position changes
        void SetPresence(Bytes const& appData);
        // the rooms to be in; the node diffs against the current ones
        void SetRooms(std::vector<Room> const& rooms);
        // starts/stops the connection to the node
        void Start(int64_t now);
        void Stop(int64_t now);
        // work: host events, timers, clock
        void Tick(int64_t now);
        // the game's wishes changed (who is wanted): look at every peer again
        void Reevaluate(int64_t now);

        // messages over links (false when no link is up). Reliable messages arrive once and in order whatever route carries them.
        bool Send(Id128 const& peer, uint16_t type, Bytes const& body, Channel channel = Reliable);
        size_t Broadcast(std::function<bool(Peer const&)> const& filter, uint16_t type, Bytes const& body, Channel channel = Reliable);
        // The keyed channel: a newer message of the same key replaces an older one that is still waiting; never waits behind the
        // reliable channel and is not ordered with it. For state: movement, snapshots, creature states. A key is any number the caller makes up.
        bool SendKeyed(Id128 const& peer, uint64_t key, uint16_t type, Bytes const& body);
        size_t BroadcastKeyed(std::function<bool(Peer const&)> const& filter, uint64_t key, uint16_t type, Bytes const& body);
        // a signed, not encrypted message into one of our rooms (the game's public announcements); 'node' = RouteAny for every node
        void Say(std::string const& room, Bytes const& payload, uint8_t node = RouteAny);
        // the route (see Host.h) the connection to the peer takes now, -1 if none; the routes that exist
        bool RouteExists(Peer const& peer, int route) const;
        std::string RouteName(int route) const;      // "direct", or the address of node k
        struct NodeInfo { std::string address; bool up = false, everUp = false, lostTold = false, failedTold = false, removed = false, tried = false; int64_t upSince = 0, since = 0; std::string reason; };
        std::vector<NodeInfo> const& Nodes() const { return _nodes; }
        size_t NodesUp() const;
        // the nodes for this session only: the host connects or leaves one; 'tried' nodes are reported in the chat as soon as they are reached or refused
        void AddRelay(std::string const& address, bool tried);
        void RemoveRelay(unsigned number);

        // directory of the peers
        Peer* Find(Id128 const& id);
        template <class F> void ForEachPeer(F&& f) { for (auto& [id, p] : _peers) f(*p); }
        size_t PeerCount() const { return _peers.size(); }
        size_t LinkCount() const;
        bool NodeUp() const { return _nodeUp; }
        std::string const& Inbox() const { return _inboxHex; }
        Certificate const& Me() const { return _me; }
        Scope const& GetScope() const { return _scope; }
        Account const& MyAccount() const { return _account; }

        // the network time: UTC corrected by the offset learned from the peers
        int64_t NetNow(int64_t machineNow) const { return machineNow + _offset; }
        int64_t Offset() const { return _offset; }

    private:
        enum AnnounceType : uint8_t { Here = 1, Leaving = 2, Gone = 3 };

        void OnHostEvent(HostEvent const& e, int64_t now);
        void OnRoomMessage(std::string const& room, Bytes const& data, unsigned node, int64_t now);
        void OnInboxMessage(std::string const& fromInbox, Bytes const& data, int route, int64_t now);
        void OnEnvelope(Envelope const& env, std::string const& room, bool directReply, int route, int64_t now);
        Bytes AnnouncePayload(AnnounceType type, std::string const& room) const;
        void AnnounceIn(Room const& room, int64_t now, uint8_t node = RouteAny);
        void ReplyTo(Peer& peer, std::string const& room, int route, int64_t now);
        Peer& Touch(Certificate const& cert, std::string const& inbox, int64_t now);
        void Consider(Peer& peer, int64_t now);
        void UpdateTouch(Peer& peer);
        void OpenLinkTo(Peer& peer, int64_t now);
        void RestartLink(Peer& peer, std::string const& why, int64_t now);
        void SendReset(std::string const& toInbox, int route, int64_t now);
        void CloseLink(Peer& peer, std::string const& why, bool tell, int64_t now);
        void DropPeer(Peer& peer, bool fireGone);
        OpenResult OnLinkFrame(Peer& peer, Bytes const& frame, int route, int64_t now);
        void Deliver(Peer& peer, Bytes const& message, Channel channel, int route, int64_t now);
        void SendMessage(Peer& peer, uint16_t type, Bytes const& body, Channel channel);
        bool SendKeyedMessage(Peer& peer, uint64_t key, uint16_t type, Bytes const& body);
        void Transmit(Peer& peer, Bytes const& frame, Channel channel, int route);
        void PumpReliable(Peer& peer, int64_t now);
        void SendHello(Peer& peer, int64_t now);
        void BringUp(Peer& peer, int64_t now);
        void HandleInternal(Peer& peer, uint16_t type, Reader& body, int route, int64_t now);
        // routes and quality
        int PickRoute(Peer const& peer) const;
        void Heard(Peer& peer, int route, int64_t now);
        void Probe(Peer& peer, int route, int64_t now);
        void Upkeep(Peer& peer, int64_t now);
        void EvaluateRoutes(Peer& peer, int64_t now);
        void SwitchRoute(Peer& peer, int to, std::string const& why, int64_t now);
        void MaybeOffer(Peer& peer, int64_t now);
        void NetEvent(std::string const& kind, Peer* peer, std::string const& text);
        std::string RouteLabel(int route) const;
        void UpdateClock(int64_t now);
        Peer* PeerByInbox(std::string const& inbox);
        bool Fresh(Envelope const& env, int64_t now, bool& duplicate);
        bool CertOk(Certificate const& cert);
        void Maintain(int64_t now);

        Config _config;
        Scope _scope;
        Account _account;
        Certificate _me;
        PeerHost& _host;
        Listener& _listener;
        Id128 _inbox{}, _session{};
        std::string _inboxHex;
        bool _started = false, _nodeUp = false;
        std::vector<NodeInfo> _nodes;
        std::string _tryAddress;
        std::map<std::string, int64_t> _resetSent;   // by inbox: when a reset was last sent (limits the answers to a flood of bad frames)
        Bytes _presence;
        std::map<std::string, Room> _rooms;
        std::unordered_map<Id128, std::unique_ptr<Peer>, IdHash> _peers;
        std::unordered_map<std::string, Id128> _byInbox;
        std::set<Key32> _seen;   // hashes of recent envelopes, against replays
        std::deque<std::pair<int64_t, Key32>> _seenOrder;
        std::set<Key32> _verifiedCerts;   // hashes of the certificates whose signature was verified (it is expensive)
        int64_t _now = 0, _offset = 0, _lastAnnounce = 0, _lastClock = 0, _lastMaintenance = 0;
    };
}
