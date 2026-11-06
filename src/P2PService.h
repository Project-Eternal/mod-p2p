/*
 * The service of mod-p2p inside the server: owns the peer host, the thread that runs the network node, and the queues between that thread and
 * the game's world thread. Rule of threads: the p2p thread (the only one that touches the Node) never touches the game's objects; the world
 * thread (WorldScript::OnUpdate, where the maps are idle) applies what arrived and collects what is to be said.
 */
#pragma once
#include "DatabaseEnvFwd.h"
#include "ObjectGuid.h"
#include "P2PStore.h"
#include "Protocol.h"
#include "RingHost.h"
#include "core/Node.h"
#include "core/Quality.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>

class Player;

namespace p2p
{
    // what the p2p thread tells the world thread
    struct InEvent
    {
        enum Kind { Announced, LinkUp, LinkDown, Gone, Message, NodeState, InTouch, NetEvent } kind = Announced;
        Id128 peer{};
        Certificate cert;
        Bytes data;           // Announced: the presence; Message: the body
        uint16_t type = 0;    // Message
        Channel channel = Reliable;
        bool flag = false;    // Announced: new; NodeState: up; InTouch: in touch
        std::string text;     // LinkDown: why; NetEvent: the sentence
        std::string what;     // NetEvent: the kind (node-reached, node-lost, node-back, route, link-lost, link-restored, direct-failed)
        bool direct = false;
    };

    // what the world thread tells the p2p thread to decide links by (read under the lock by Listener::WantLink and Blocked)
    struct Interest
    {
        bool active = false;
        game::Presence mine;
        std::set<Id128> blocked;   // ignored characters
        std::set<Id128> needed;    // characters a link is wanted to for a while (a whisper, a mail, a trade), regardless of what is shared
    };

    // a peer as the world thread sees it
    struct PeerInfo
    {
        Certificate cert;
        game::Presence presence;
        bool linked = false, direct = false;
        bool inTouch = false;       // the register (core/Node.h Peer::inTouch): "online" for the friends list, the party frames, the channel lists
        bool compatible = true;     // its announcements are of this protocol version
        uint8_t version = 0;
        int64_t announcedAt = 0;
    };

    // What the p2p thread measures of a connection, copied to the world thread about once a second (the status command never waits for the p2p thread)
    struct RouteFigures
    {
        std::string name;           // "direct" or the node's address
        bool exists = false, inUse = false;
        int32_t rtt = -1;           // median round trip of the last probes, ms (-1: none answered)
        double loss = 0;
        uint32_t probes = 0;
    };
    struct LinkFigures
    {
        bool linked = false, direct = false, inTouch = false;
        std::string route;          // the route in use
        std::string routeLabel;     // the same as the chat names it: "direct" or "relay #2" (the address stays in the log)
        int32_t rtt = -1;
        double loss = 0;
        int64_t upForMs = 0, silentMs = 0;
        uint8_t directReason = 0;
        uint64_t sent = 0, received = 0;
        std::vector<RouteFigures> routes;
        std::string inbox;
        std::vector<std::string> rooms;
    };
    struct NodeFigures { std::string address; bool up = false, everUp = false, removed = false; int64_t sinceMs = 0; std::string reason; };   // sinceMs: machine time of the last change

    // The features of the game side plug in here: each handles the events of the world thread and says which rooms its state needs
    class Feature
    {
    public:
        virtual ~Feature() = default;
        virtual void OnLogin(Player* player) { }
        virtual void OnLogout(Player* player) { }
        virtual void OnEvent(InEvent const& e, Player* local) { }
        virtual void Update(uint32_t diff, Player* local) { }
        virtual void OnRealmUnbind() { }   // the server leaves its realm: forget what was read from the realm's characters database (nobody is in the world)
        virtual void OnStopping(Player* local) { }   // the server is stopped in order: hand over, cancel, say goodbye (before the player is logged out)
        virtual void Presence(Player* local, game::Presence& presence) { }   // what this feature adds to the announced presence
        virtual void Rooms(Player* local, std::vector<Room>& rooms) { }   // the rooms this feature wants (private ones included)
    };

    class Service : public Listener
    {
    public:
        static Service& Get();
        bool Enabled() const { return _enabled; }
        void Startup();
        void Shutdown();
        void Update(uint32_t diff);
        void OnLogin(Player* player);
        void OnLogout(Player* player);
        // The server changes realm (World.RealmSwitch.Enable): the characters database, and so the account key, the characters met and the records of transfers,
        // are those of another realm, and so are the rooms (players meet only players of their realm). Nobody is in the world, so the node has been left already.
        void OnRealmUnbind(uint32_t realm);
        void OnRealmBind(uint32_t realm);

        void AddFeature(Feature* feature) { _features.push_back(feature); }
        // world thread -> p2p thread
        void Post(std::function<void(Node&)> f);
        // map threads (the unit hooks) -> the world thread, where the maps are idle
        void Defer(std::function<void()> f);
        void Say(Id128 const& peer, uint16_t type, Bytes const& body, Channel channel = Reliable);
        void SayAll(std::function<bool(PeerInfo const&)> const& filter, uint16_t type, Bytes const& body, Channel channel = Reliable);
        // The keyed channel: a newer message of the same key replaces an older one still waiting, so a lost
        // one never holds up the newest. For state: movement, snapshots, creature states, member state.
        void SayKeyed(Id128 const& peer, uint64_t key, uint16_t type, Bytes const& body);
        void SayAllKeyed(std::function<bool(PeerInfo const&)> const& filter, uint64_t key, uint16_t type, Bytes const& body);
        void Need(Id128 const& peer, bool needed);
        void SetBlocked(std::set<Id128> const& blocked);
        void WantRefresh();
        // The whole chain of saving, at once (world thread): the player's save with what the caller appended to 'trans', in one transaction committed
        // synchronously; the shim's save of the database file; the page's write of the kept files to IndexedDB. Used when a trade or a mail changes what a
        // player has, and by .save: a server that stops a second later still holds what it has confirmed. Without a transaction, only the player's save.
        void KeepNow(Player* player, CharacterDatabaseTransaction trans = nullptr);
        // the orderly stop (asked for by the page when the game exits): called from any thread; the world thread does it at its next update
        void RequestStop() { _stopRequested = true; }
        bool Stopped() const { return _stopped; }
        // whether the world thread has run an update (the world loop is going); before that the server is still starting and has nothing to stop in order
        bool Updating() const { return _updating; }
        // characters met in this session stay reachable (their pair room is kept until logout); also called by whoever addresses a character
        void Meet(Id128 const& peer);
        // what the p2p thread measured (a copy under a lock); a peer that is silent for long is "unresponsive"
        bool Figures(Id128 const& peer, LinkFigures& out);
        std::vector<NodeFigures> NodeList();
        // the nodes for this session only
        void RelayAdd(std::string const& address, bool tried);
        void RelayRemove(unsigned number);
        std::vector<std::string> DescribeRelays();   // .p2p relays
        bool Responsive(Id128 const& peer);   // false when nothing has come from it for 5 seconds although its probes ask every 2

        // world thread: the state of the network
        Player* Local() const;
        Account const& MyAccount() const { return _account; }
        Certificate const& Me() const { return _me; }
        Certificate const& MyCertificate() const { return _me; }
        std::map<Id128, PeerInfo>& Peers() { return _peers; }
        game::Presence const& Presence() const { return _myPresence; }   // what this server announces about its player (world thread)
        PeerInfo* FindPeer(Id128 const& id);
        bool NodeUp() const { return _nodeUp; }
        uint32_t RealmId() const { return _realm; }
        // where the names of rooms live: the realm, and in a private shard the shard's prefix and key; made at the login (the slow derivation)
        Scope const& RoomScope() const { return _scope; }
        bool InShard() const { return _scope.shard; }
        int64_t NetNow() const;   // the network time in milliseconds (UTC corrected by the offset learned from the peers)
        std::string Describe();                                  // the summary
        std::vector<std::string> DescribePlayers();              // the players this one is in touch with
        std::vector<std::string> DescribeDetails();              // everything, for a report
        std::string NameOf(Id128 const& peer);                   // the name this player knows the character by
        bool ChatEvents() const { return _chatEvents; }
        void Tell(Player* player, std::string const& text);   // a server message to the player, with the name of the server in front

        // p2p thread (Listener)
        bool Blocked(Certificate const& cert) override;
        bool WantLink(Peer const& peer) override;
        void OnPeerAnnounced(Peer& peer, bool isNew) override;
        void OnLinkUp(Peer& peer) override;
        void OnLinkDown(Peer& peer, std::string const& why) override;
        void OnPeerGone(Peer& peer) override;
        void OnInTouch(Peer& peer, bool inTouch) override;
        void OnNetwork(std::string const& kind, Peer* peer, std::string const& text) override;
        void OnMessage(Peer& peer, uint16_t type, Reader& body, Channel channel) override;
        void OnNodeState(bool up) override;
        void OnLog(std::string const& line) override;

    private:
        void Run();
        void Command(std::function<void()> f);
        void PrepareScope();
        void Begin();
        void End();
        void Push(InEvent&& e);
        void RefreshInterest(Player* local);
        void Apply(InEvent const& e);
        static int64_t MachineNow();

        void RefreshFigures();
        void UpdateQuality(uint32_t diff, Player* local);   // world thread: the connection quality indicator, once a second
        void SendQuality(quality::Result const& r);          // any thread: queued to the p2p thread, which sends the host command
        void StopInOrder();
        bool _enabled = false, _running = false, _chatEvents = true;
        bool _orderlyDone = false;   // (the features have been stopped in order; the logout that follows does not do it again)
        std::atomic<bool> _stopRequested{false}, _stopped{false}, _updating{false};
        std::mutex _figuresMutex;
        std::map<Id128, LinkFigures> _figures;
        std::vector<NodeFigures> _nodeFigures;
        int64_t _figuresAt = 0;
        uint32_t _qualityTimer = 0;
        std::atomic<bool> _qualityShown{false};   // the page has been sent a quality and not yet the "none" of the stop
        std::map<Id128, int64_t> _met;                  // world thread: characters met in this session -> last used
        std::map<std::string, std::string> _lastChat;   // world thread: the last network message written into the chat for each player or node
        std::map<Id128, int64_t> _routeTold;            // world thread: when a route change of a peer was last written into the chat
        uint32_t _realm = 1;
        Scope _scope;                // world thread, set at the login
        Scope _scopeFor;             // (the scope is made once per realm and identifier)
        RingHost _host;
        std::thread _thread;
        std::atomic<bool> _stop{false};
        std::mutex _mutex;                      // the queues and the interest
        std::deque<std::function<void()>> _commands;
        std::vector<std::function<void()>> _deferred;
        std::deque<InEvent> _events;
        Interest _interest;
        std::unique_ptr<Node> _node;            // p2p thread only
        std::atomic<int64_t> _offset{0};        // mirror of the node's clock offset, for NetNow()
        std::atomic<bool> _nodeUp{false};
        // world thread
        Account _account;
        Certificate _me;
        ObjectGuid _local;
        std::map<Id128, PeerInfo> _peers;
        std::vector<Feature*> _features;
        game::Presence _myPresence;
        std::map<std::pair<uint32_t, uint32_t>, uint64_t> _instanceKeys;   // (map, local instance) -> the instance's network key
        uint32_t _refreshTimer = 0;
        bool _refresh = true;
        std::vector<std::string> _joined;
        Bytes _lastPresence;
        std::vector<std::string> _lastRoomNames;
        uint32_t _nodeReported = 0;
        bool _announcedDown = false;
    };
}
