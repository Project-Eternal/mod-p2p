#include "Node.h"
#include <algorithm>
#include <cmath>

namespace p2p
{
    // ---------------------------------------------------------------- route statistics

    int32_t RouteStats::Median() const
    {
        std::vector<int32_t> v;
        for (int32_t r : results)
            if (r >= 0)
                v.push_back(r);
        if (v.empty())
            return -1;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }

    double RouteStats::Loss() const
    {
        if (results.empty())
            return 0;
        size_t lost = 0;
        for (int32_t r : results)
            lost += r < 0 ? 1 : 0;
        return static_cast<double>(lost) / results.size();
    }

    char const* DirectReasonName(uint8_t reason)
    {
        switch (reason)
        {
            case DirectNotTried: return "not tried";
            case DirectNoPath: return "no network path";
            case DirectTimedOut: return "timed out";
            case DirectDisabled: return "disabled in the settings";
            case DirectOpen: return "open";
        }
        return "unknown";
    }

    Node::Node(Config const& config, Account const& account, Certificate const& character, PeerHost& host, Listener& listener)
        : _config(config), _scope(config.shard ? Scope::Shard(config.realm, config.shardKey) : Scope(config.realm)), _account(account), _me(character), _host(host), _listener(listener)
    {
        _inbox = crypto::RandomId();
        _session = crypto::RandomId();
        _inboxHex = Hex(_inbox);
    }

    size_t Node::LinkCount() const
    {
        size_t n = 0;
        for (auto const& [id, p] : _peers)
            n += p->linkUp ? 1 : 0;
        return n;
    }

    size_t Node::NodesUp() const
    {
        size_t n = 0;
        for (NodeInfo const& i : _nodes)
            n += i.up ? 1 : 0;
        return n;
    }

    void Node::AddRelay(std::string const& address, bool tried)
    {
        if (tried)
            _tryAddress = address;
        HostCommand c;
        c.kind = HostCommand::NodeAdd;
        c.name = address;
        _host.Command(c);
    }

    void Node::RemoveRelay(unsigned number)
    {
        HostCommand c;
        c.kind = HostCommand::NodeRemove;
        c.route = static_cast<uint8_t>(number);
        _host.Command(c);
    }

    Peer* Node::Find(Id128 const& id)
    {
        auto it = _peers.find(id);
        return it == _peers.end() ? nullptr : it->second.get();
    }

    Peer* Node::PeerByInbox(std::string const& inbox)
    {
        auto it = _byInbox.find(inbox);
        return it == _byInbox.end() ? nullptr : Find(it->second);
    }

    bool Node::CertOk(Certificate const& cert)
    {
        Key32 h = crypto::Sha256(cert.Encode());
        if (_verifiedCerts.count(h))
            return true;
        if (!cert.Verify())
            return false;
        if (_verifiedCerts.size() > 4096)
            _verifiedCerts.clear();
        _verifiedCerts.insert(h);
        return true;
    }

    void Node::NetEvent(std::string const& kind, Peer* peer, std::string const& text)
    {
        _listener.OnNetwork(kind, peer, text);
    }

    void Node::UpdateTouch(Peer& peer)
    {
        bool touch = !_listener.Blocked(peer.cert) && (peer.linkUp || (!peer.rooms.empty() && peer.answered));
        if (touch == peer.inTouch)
            return;
        peer.inTouch = touch;
        _listener.OnInTouch(peer, touch);
    }

    // In the chat and in the log a node is "relay #N" (its place in the list), never its address. A text may name the other player as {player}.
    std::string Node::RouteLabel(int route) const
    {
        if (route == 0)
            return "direct";
        if (route >= 1 && static_cast<size_t>(route - 1) < _nodes.size())
            return "relay #" + std::to_string(route);
        return "none";
    }

    std::string Node::RouteName(int route) const
    {
        if (route == 0)
            return "direct";
        if (route >= 1 && static_cast<size_t>(route - 1) < _nodes.size())
            return _nodes[route - 1].address;
        return "none";
    }

    bool Node::RouteExists(Peer const& peer, int route) const
    {
        if (route < 0 || route > MaxNodes)
            return false;
        if (route == 0)
            return peer.routes[0].open;
        size_t k = route - 1;
        if (k >= _nodes.size() || !_nodes[k].up)
            return false;
        int64_t heard = peer.routes[route].heardAt;
        return heard && _now - heard < 3 * _config.announceEveryMs;
    }

    // ---------------------------------------------------------------- start, stop, rooms

    void Node::Start(int64_t now)
    {
        _now = now;
        if (_started)
            return;
        _started = true;
        Room gone;
        gone.name = GoneTopic(_scope, _inboxHex);
        Writer w;
        w.U8(Gone).Str(_inboxHex).Id(_session).Str(gone.name).Blob(nullptr, 0);
        HostCommand open;
        open.kind = HostCommand::Open;
        open.name = _inboxHex;
        open.text = RealmPrefix(_scope);
        open.data = Envelope::Make(_account, _me, gone, now, w.data);
        _host.Command(open);
    }

    void Node::Stop(int64_t now)
    {
        if (!_started)
            return;
        _now = now;
        if (_nodeUp)
            for (auto const& [name, room] : _rooms)
                Say(name, AnnouncePayload(Leaving, name));
        for (auto& [id, p] : _peers)
            if (p->linkUp)
                CloseLink(*p, "stopped", true, now);
        { HostCommand c; c.kind = HostCommand::Will; _host.Command(c); }   // (a clean disconnect would not make a broker publish it)
        { HostCommand c; c.kind = HostCommand::Close; _host.Command(c); }
        _started = _nodeUp = false;
        for (NodeInfo& n : _nodes)
            n.up = false;
    }

    void Node::SetRooms(std::vector<Room> const& rooms)
    {
        std::map<std::string, Room> wanted;
        for (Room const& r : rooms)
            if (wanted.size() < _config.maxRooms)
                wanted[r.name] = r;
        for (auto it = _rooms.begin(); it != _rooms.end();)
        {
            if (!wanted.count(it->first))
            {
                if (_nodeUp)
                    Say(it->first, AnnouncePayload(Leaving, it->first));
                HostCommand c;
                c.kind = HostCommand::Leave;
                c.name = it->first;
                _host.Command(c);
                for (auto& [id, p] : _peers)
                    p->rooms.erase(it->first);
                it = _rooms.erase(it);
            }
            else
                ++it;
        }
        for (auto const& [name, room] : wanted)
        {
            if (_rooms.count(name))
                continue;
            _rooms[name] = room;
            HostCommand c;
            c.kind = HostCommand::Join;
            c.name = name;
            _host.Command(c);
            if (_nodeUp)
                Say(name, AnnouncePayload(Here, name));
        }
    }

    void Node::SetPresence(Bytes const& appData)
    {
        if (appData == _presence)
            return;
        _presence = appData;
        // linked peers hear it over their encrypted link at once; the rooms hear it at the next announcement
        Writer w;
        w.Blob(_presence);
        for (auto& [id, p] : _peers)
            if (p->linkUp)
                SendKeyedMessage(*p, 0xFFFF000000000001ull, MsgPresence, w.data);
    }

    Bytes Node::AnnouncePayload(AnnounceType type, std::string const& room) const
    {
        Writer w;
        w.U8(type).Str(_inboxHex).Id(_session).Str(room).Blob(_presence);
        return w.data;
    }

    void Node::Say(std::string const& roomName, Bytes const& payload, uint8_t node)
    {
        auto it = _rooms.find(roomName);
        if (it == _rooms.end())
            return;
        HostCommand c;
        c.kind = HostCommand::Publish;
        c.name = roomName;
        c.route = node;
        c.data = Envelope::Make(_account, _me, it->second, NetNow(_now), payload);
        _host.Command(c);
    }

    void Node::AnnounceIn(Room const& room, int64_t now, uint8_t node)
    {
        Say(room.name, AnnouncePayload(Here, room.name), node);
    }

    // ---------------------------------------------------------------- the host's events

    void Node::Tick(int64_t now)
    {
        _now = now;
        HostEvent e;
        for (int budget = 0; budget < 4096 && _host.Poll(e); ++budget)
            OnHostEvent(e, now);
        for (auto& [id, p] : _peers)
            if (p->linkUp)
                PumpReliable(*p, now);
        if (now - _lastMaintenance >= 1000 || _lastMaintenance == 0)
        {
            _lastMaintenance = now;
            Maintain(now);
        }
    }

    void Node::OnHostEvent(HostEvent const& e, int64_t now)
    {
        _lastMaintenance = _lastMaintenance ? _lastMaintenance : now;
        switch (e.kind)
        {
            case HostEvent::NodeUp:
            {
                unsigned k = std::min<unsigned>(e.node, MaxNodes - 1);
                if (_nodes.size() <= k)
                    _nodes.resize(k + 1);
                bool again = _nodes[k].everUp;
                _nodes[k].address = e.name.empty() ? "node " + std::to_string(k + 1) : e.name;
                _nodes[k].up = true;
                _nodes[k].everUp = true;
                bool was = _nodeUp;
                _nodeUp = true;
                if (!was)
                    _listener.OnNodeState(true);
                _nodes[k].upSince = _nodes[k].since = now;
                _nodes[k].reason.clear();
                // the chat rule: news only. The first time it is reached is told; a node that came back is told once, after it has
                // stayed up for two minutes (Maintain), and only if its loss was told; every change is in the log
                if (!again || _nodes[k].tried)
                {
                    _nodes[k].tried = false;
                    NetEvent("node-reached", nullptr, "Connected to relay #" + std::to_string(k + 1) + ".");
                }
                else
                    NetEvent("node-quiet", nullptr, "Connected again to relay #" + std::to_string(k + 1) + ".");
                for (auto const& [name, room] : _rooms)
                {
                    HostCommand c;
                    c.kind = HostCommand::Join;
                    c.name = name;
                    c.route = RouteOfNode(k);
                    _host.Command(c);
                    AnnounceIn(room, now, RouteOfNode(k));
                }
                for (auto& [id, p] : _peers)
                {
                    HostCommand c;
                    c.kind = HostCommand::Watch;
                    c.name = GoneTopic(_scope, p->inbox);
                    c.route = RouteOfNode(k);
                    _host.Command(c);
                }
                break;
            }
            case HostEvent::NodeDown:
            {
                unsigned k = std::min<unsigned>(e.node, MaxNodes - 1);
                if (_nodes.size() <= k)
                    _nodes.resize(k + 1);
                if (!e.name.empty())
                    _nodes[k].address = e.name;
                bool wasUp = _nodes[k].up;
                _nodes[k].up = false;
                _nodeUp = NodesUp() > 0;
                if (wasUp)
                {
                    _nodes[k].since = now;
                    if (!_nodes[k].lostTold)
                    {
                        _nodes[k].lostTold = true;
                        NetEvent("node-lost", nullptr, "The connection to relay #" + std::to_string(k + 1) + " was lost.");
                    }
                    else
                        NetEvent("node-quiet", nullptr, "The connection to relay #" + std::to_string(k + 1) + " was lost again.");
                }
                if (!_nodeUp)
                    _listener.OnNodeState(false);
                // the connections that were carried by that node move at once
                for (auto& [id, p] : _peers)
                    if (p->linkUp && p->route == static_cast<int>(RouteOfNode(k)))
                    {
                        int to = PickRoute(*p);
                        if (to >= 0 && to != p->route)
                            SwitchRoute(*p, to, "node " + _nodes[k].address + " lost", now);
                    }
                break;
            }
            case HostEvent::NodeList:
            {
                // the host's list: addresses by line, an empty line for a node that was removed (the numbers of the others do not change)
                std::vector<std::string> lines;
                std::string text = ToString(e.data), line;
                for (char ch : text)
                {
                    if (ch == '\n') { lines.push_back(line); line.clear(); }
                    else line.push_back(ch);
                }
                if (!line.empty())
                    lines.push_back(line);
                size_t before = _nodes.size();
                if (_nodes.size() < lines.size())
                    _nodes.resize(std::min<size_t>(lines.size(), MaxNodes));
                for (size_t k = 0; k < lines.size() && k < _nodes.size(); ++k)
                {
                    NodeInfo& n = _nodes[k];
                    if (lines[k].empty())
                    {
                        if (!n.removed)
                        {
                            n.removed = true;
                            if (n.up)
                                NetEvent("node-quiet", nullptr, "Removed relay #" + std::to_string(k + 1) + ".");
                            n.up = false;
                            _nodeUp = NodesUp() > 0;
                            if (!_nodeUp)
                                _listener.OnNodeState(false);
                        }
                        continue;
                    }
                    bool isNew = k >= before || n.address != lines[k];
                    n.address = lines[k];
                    n.removed = false;
                    if (isNew && !_tryAddress.empty() && n.address == _tryAddress)
                    {
                        n.tried = true;
                        _tryAddress.clear();
                    }
                }
                break;
            }
            case HostEvent::NodeFailed:
            {
                unsigned k = std::min<unsigned>(e.node, MaxNodes - 1);
                if (_nodes.size() <= k)
                    _nodes.resize(k + 1);
                if (!e.name.empty())
                    _nodes[k].address = e.name;
                _nodes[k].reason = ToString(e.data);
                if (_nodes[k].up)
                    break;
                std::string text = "The connection to relay #" + std::to_string(k + 1) + " failed" + (_nodes[k].reason.empty() ? "" : " (" + _nodes[k].reason + ")") + ".";
                if ((!_nodes[k].everUp && !_nodes[k].failedTold) || _nodes[k].tried)
                {
                    _nodes[k].failedTold = true;
                    _nodes[k].tried = false;
                    NetEvent("node-failed", nullptr, text);
                }
                else
                    NetEvent("node-quiet", nullptr, text);
                break;
            }
            case HostEvent::RoomMessage:
                OnRoomMessage(e.name, e.data, e.node, now);
                break;
            case HostEvent::InboxMessage:
                OnInboxMessage(e.name, e.data, e.route, now);
                break;
            case HostEvent::LinkUp:
                if (Peer* p = PeerByInbox(e.name))
                {
                    p->routes[0].open = true;
                    p->routes[0].heardAt = now;
                    p->directReason = DirectOpen;
                    if (p->linkUp)
                    {
                        Probe(*p, 0, now);   // (measured now; it is moved to when it is as good)
                        // a link that has just come up has nothing measured to compare with, and a direct route is preferred at equal quality
                        if (p->route != 0 && p->routes[std::max(p->route, 0)].Finished() < 3)
                            SwitchRoute(*p, 0, "the direct link opened", now);
                    }
                    else
                    {
                        p->route = 0;
                        p->direct = true;
                    }
                }
                break;
            case HostEvent::LinkDown:
                if (Peer* p = PeerByInbox(e.name))
                {
                    p->routes[0].open = false;
                    p->routes[0].results.clear();
                    p->routes[0].outstanding.clear();
                    p->routes[0].better = 0;
                    if (p->directReason == DirectOpen)
                        p->directReason = DirectNotTried;
                    if (p->route == 0)
                    {
                        int to = PickRoute(*p);
                        if (p->linkUp && to >= 0)
                            SwitchRoute(*p, to, "the direct link closed", now);
                        else
                        {
                            p->route = to;
                            p->direct = false;
                        }
                    }
                }
                break;
            case HostEvent::DirectFailed:
                if (Peer* p = PeerByInbox(e.name))
                {
                    bool changed = p->directReason != e.reason;
                    p->directReason = e.reason;
                    if (changed || e.reason != DirectDisabled)
                        NetEvent("direct-failed", p, std::string("There is no direct link to {player} (") + DirectReasonName(e.reason) + ").");
                }
                break;
        }
    }

    bool Node::Fresh(Envelope const& env, int64_t now, bool& duplicate)
    {
        int64_t net = NetNow(now);
        duplicate = false;
        if (env.time < net - _config.maxSkewMs || env.time > net + _config.maxSkewMs)
            return false;
        Writer w;
        w.Id(env.sender.id).I64(env.time).Raw(env.signature);
        Key32 h = crypto::Sha256(w.data);
        if (_seen.count(h))
        {
            duplicate = true;
            return true;
        }
        _seen.insert(h);
        _seenOrder.emplace_back(now, h);
        return true;
    }

    void Node::OnRoomMessage(std::string const& room, Bytes const& data, unsigned node, int64_t now)
    {
        Envelope env;
        if (!Envelope::Decode(data, env) || env.room != room)
            return;
        std::string gonePrefix = RealmPrefix(_scope) + "/gone/";
        if (room.compare(0, gonePrefix.size(), gonePrefix) == 0)
        {
            // the last will of a peer: pre-signed with its session, so it cannot be replayed against another session
            if (!CertOk(env.sender) || !env.Verify(nullptr, true))
                return;
            Reader r(env.payload);
            if (r.U8() != Gone)
                return;
            std::string inbox = r.Str(128);
            Id128 session = r.Id();
            if (!r.ok())
                return;
            Peer* p = PeerByInbox(inbox);
            if (!p || p->session != session || p->cert.id != env.sender.id)
                return;
            // a will means "gone from that node": the peer is gone when no other node carries it and no direct link is open
            int route = RouteOfNode(std::min<unsigned>(node, MaxNodes - 1));
            p->routes[route].heardAt = 0;
            p->routes[route].results.clear();
            p->routes[route].outstanding.clear();
            bool carried = p->routes[0].open;
            for (int r2 = 1; r2 <= MaxNodes && !carried; ++r2)
                carried = RouteExists(*p, r2);
            if (!carried)
                DropPeer(*p, true);
            else if (p->linkUp && p->route == route)
            {
                int to = PickRoute(*p);
                if (to >= 0 && to != p->route)
                    SwitchRoute(*p, to, "the peer left " + RouteLabel(route), now);
            }
            return;
        }
        auto it = _rooms.find(room);
        if (it == _rooms.end())
            return;
        if (!CertOk(env.sender))
            return;
        if (env.sender.id == _me.id || env.sender.accountKey == _me.accountKey)
            return;
        int route = RouteOfNode(std::min<unsigned>(node, MaxNodes - 1));
        {
            // a copy that came by another node: it only tells that this peer is there too (the signature was checked on the first copy)
            Writer w;
            w.Id(env.sender.id).I64(env.time).Raw(env.signature);
            if (_seen.count(crypto::Sha256(w.data)))
            {
                if (Peer* p = Find(env.sender.id))
                    Heard(*p, route, now);
                return;
            }
        }
        if (!env.Verify(it->second.priv ? &it->second : nullptr, true))
            return;
        bool duplicate = false;
        if (!Fresh(env, now, duplicate) || duplicate)
            return;
        OnEnvelope(env, room, false, route, now);
    }

    void Node::OnEnvelope(Envelope const& env, std::string const& room, bool directReply, int route, int64_t now)
    {
        Reader r(env.payload);
        uint8_t type = r.U8();
        std::string inbox = r.Str(128);
        Id128 session = r.Id();
        std::string inRoom = r.Str(512);
        Bytes app = r.Blob(1 << 14);
        if (!r.ok() || inbox.empty())
            return;
        if (_listener.Blocked(env.sender))
            return;
        if (type == Leaving)
        {
            if (Peer* p = Find(env.sender.id))
            {
                p->rooms.erase(inRoom);
                Consider(*p, now);
                UpdateTouch(*p);
            }
            return;
        }
        if (type != Here)
            return;
        if (!_rooms.count(inRoom))
            return;   // (a reply about a room we left)
        Peer* existing = Find(env.sender.id);
        bool isNew = !existing || existing->inbox != inbox || existing->session != session;
        Peer& peer = Touch(env.sender, inbox, now);
        peer.session = session;
        peer.lastSeen = now;
        Heard(peer, route, now);
        bool changed = peer.appData != app;
        peer.appData = app;
        bool roomIsNew = peer.rooms.insert(inRoom).second;
        if (isNew || roomIsNew)
            if (!directReply)
                ReplyTo(peer, inRoom, route, now);
        if (directReply && !peer.answered)
        {
            // it answered us: it does not ignore us. Our answer to its answer (once) lets it know the same of us, so that "in touch" is the same on both sides
            peer.answered = true;
            ReplyTo(peer, inRoom, route, now);
        }
        if (isNew || changed || roomIsNew)
            _listener.OnPeerAnnounced(peer, isNew);
        Consider(peer, now);
        UpdateTouch(peer);
    }

    void Node::ReplyTo(Peer& peer, std::string const& room, int route, int64_t now)
    {
        // an introduction straight to the peer's inbox: signed, for that inbox only
        Room target;
        target.name = InboxTopic(_scope, peer.inbox);
        Bytes envelope = Envelope::Make(_account, _me, target, NetNow(now), AnnouncePayload(Here, room));
        Writer w;
        w.U8('E').Raw(envelope);
        HostCommand c;
        c.kind = HostCommand::Send;
        c.name = peer.inbox;
        c.data = w.data;
        c.channel = Reliable;
        c.route = static_cast<uint8_t>(route);   // (the node that carried the announcement is known to work)
        _host.Command(c);
    }

    Peer& Node::Touch(Certificate const& cert, std::string const& inbox, int64_t now)
    {
        auto it = _peers.find(cert.id);
        if (it != _peers.end())
        {
            Peer& p = *it->second;
            if (p.inbox != inbox)
            {
                // a new session of the same character: the old link is dead
                if (p.linkUp)
                    CloseLink(p, "new session", false, now);
                p.link.reset();
                p.helloSent = false;
                p.routes = {};
                p.route = -1;
                p.answered = false;
                UpdateTouch(p);
                _byInbox.erase(p.inbox);
                HostCommand c;
                c.kind = HostCommand::Unwatch;
                c.name = GoneTopic(_scope, p.inbox);
                _host.Command(c);
                p.inbox = inbox;
                _byInbox[inbox] = cert.id;
                c.kind = HostCommand::Watch;
                c.name = GoneTopic(_scope, inbox);
                _host.Command(c);
            }
            return p;
        }
        auto peer = std::make_unique<Peer>();
        peer->cert = cert;
        peer->inbox = inbox;
        peer->lastSeen = peer->lastHeard = now;
        peer->name = Hex(cert.id).substr(0, 8);
        Peer& ref = *peer;
        _peers[cert.id] = std::move(peer);
        _byInbox[inbox] = cert.id;
        HostCommand c;
        c.kind = HostCommand::Watch;
        c.name = GoneTopic(_scope, inbox);
        _host.Command(c);
        return ref;
    }

    void Node::DropPeer(Peer& peer, bool fireGone)
    {
        Id128 id = peer.cert.id;
        std::string inbox = peer.inbox;
        if (peer.linkUp)
        {
            peer.linkUp = false;
            _listener.OnLinkDown(peer, "gone");
        }
        peer.rooms.clear();
        peer.answered = false;
        UpdateTouch(peer);
        if (fireGone)
            _listener.OnPeerGone(peer);
        HostCommand c;
        c.kind = HostCommand::Unwatch;
        c.name = GoneTopic(_scope, inbox);
        _host.Command(c);
        c.kind = HostCommand::Disconnect;
        c.name = inbox;
        _host.Command(c);
        _byInbox.erase(inbox);
        _peers.erase(id);
    }

    // ---------------------------------------------------------------- routes

    // The route to use for a peer that has no route in use (or lost it): the direct link, else the node that carried the latest news of it
    int Node::PickRoute(Peer const& peer) const
    {
        if (peer.routes[0].open)
            return 0;
        int best = -1;
        int64_t bestHeard = 0;
        for (int r = 1; r <= MaxNodes; ++r)
            if (RouteExists(peer, r) && peer.routes[r].heardAt > bestHeard)
            {
                best = r;
                bestHeard = peer.routes[r].heardAt;
            }
        return best;
    }

    void Node::Heard(Peer& peer, int route, int64_t now)
    {
        if (route < 0 || route > MaxNodes)
            return;
        peer.routes[route].heardAt = now;
        if (peer.route < 0 && RouteExists(peer, route))
        {
            peer.route = route;
            peer.routeSince = now;
            peer.direct = route == 0;
        }
    }

    void Node::Probe(Peer& peer, int route, int64_t now)
    {
        if (!peer.link || !peer.link->Up())
            return;
        RouteStats& rs = peer.routes[route];
        rs.probeSentAt = now;
        uint32_t id = ++peer.probeId;
        rs.outstanding.emplace_back(id, now);
        Writer m;
        m.U16(MsgPing).U32(id).I64(NetNow(now));
        Transmit(peer, peer.link->Seal(Unreliable, m.data), Unreliable, route);
    }

    void Node::SwitchRoute(Peer& peer, int to, std::string const& why, int64_t now)
    {
        int from = peer.route;
        auto uses = [&](int route) { return route == 0 ? std::string("a direct link") : RouteLabel(route); };
        std::string text;
        if (from == 0 || from < 0)
            text = "The connection to {player} now uses " + uses(to) + " (" + why + ").";
        else
            text = "The connection to {player} moved from " + uses(from) + " to " + uses(to) + " (" + why + ").";
        peer.route = to;
        peer.routeSince = now;
        peer.lastSwitch = now;
        peer.direct = to == 0;
        for (RouteStats& rs : peer.routes)
            rs.better = 0;
        Probe(peer, to, now);
        NetEvent("route", &peer, text);
    }

    static bool ClearlyBetter(RouteStats const& c, bool candidateIsDirect, RouteStats const& cur)
    {
        double dc = c.Median(), dr = cur.Median() < 0 ? 100000 : cur.Median();
        double lc = c.Loss(), lr = cur.Median() < 0 ? 1.0 : cur.Loss();
        if (dr - dc >= std::max(30.0, dr / 4) && lc <= lr)
            return true;                                   // a quarter of the delay and 30 ms better, no more loss
        if (lr - lc >= 0.05 && dc <= dr * 1.25)
            return true;                                   // 5 points less loss at a delay no more than a quarter worse
        if (candidateIsDirect && dc <= dr + 10 && lc <= lr)
            return true;                                   // a direct route is preferred at equal quality: it costs the node nothing
        return false;
    }

    void Node::EvaluateRoutes(Peer& peer, int64_t now)
    {
        if (peer.route < 0 || !RouteExists(peer, peer.route))
        {
            int to = PickRoute(peer);
            if (to >= 0 && to != peer.route)
                SwitchRoute(peer, to, "the route in use is gone", now);
            return;
        }
        RouteStats& cur = peer.routes[peer.route];
        int64_t last = std::max({cur.heardAt, cur.answeredAt, peer.routeSince});
        if (now - last > _config.routeDeadAfterMs)
        {
            // the route in use answered nothing: leave it at once for the best one that exists
            int best = -1;
            int32_t bestMedian = 0;
            int64_t bestHeard = 0;
            for (int r = 0; r <= MaxNodes; ++r)
            {
                if (r == peer.route || !RouteExists(peer, r))
                    continue;
                int32_t med = peer.routes[r].Median();
                bool better = best < 0 || (med >= 0 && (bestMedian < 0 || med < bestMedian)) || (med < 0 && bestMedian < 0 && peer.routes[r].heardAt > bestHeard);
                if (better)
                {
                    best = r;
                    bestMedian = med;
                    bestHeard = peer.routes[r].heardAt;
                }
            }
            if (best >= 0)
            {
                SwitchRoute(peer, best, "no answer for " + std::to_string((now - last) / 1000) + " s", now);
                return;
            }
        }
        // a route that has been clearly better at several evaluations in a row
        int candidate = -1;
        int32_t candidateMedian = 0;
        for (int r = 0; r <= MaxNodes; ++r)
        {
            if (r == peer.route)
                continue;
            RouteStats& c = peer.routes[r];
            if (!RouteExists(peer, r) || c.Finished() < 3 || c.Median() < 0 || cur.Finished() < 3)
            {
                c.better = 0;
                continue;
            }
            if (ClearlyBetter(c, r == 0, cur))
            {
                ++c.better;
                if (c.better >= _config.evalsNeeded && (candidate < 0 || c.Median() < candidateMedian))
                {
                    candidate = r;
                    candidateMedian = c.Median();
                }
            }
            else
                c.better = 0;
        }
        if (candidate >= 0 && now - peer.lastSwitch >= _config.routePauseMs)
        {
            RouteStats& c = peer.routes[candidate];
            std::string why = "delay " + std::to_string(c.Median()) + " ms against " + std::to_string(cur.Median()) + " ms";
            if (std::fabs(c.Loss() - cur.Loss()) >= 0.05)
                why += ", loss " + std::to_string(static_cast<int>(c.Loss() * 100)) + "% against " + std::to_string(static_cast<int>(cur.Loss() * 100)) + "%";
            SwitchRoute(peer, candidate, why, now);
        }
    }

    void Node::MaybeOffer(Peer& peer, int64_t now)
    {
        // the side with the lower identifier offers a direct link; again after 1, 2 and 5 minutes, then every 10, while it is relayed
        if (!(_me.id < peer.cert.id) || peer.routes[0].open || now < peer.nextOffer)
            return;
        static const int64_t delays[] = {60000, 120000, 300000, 600000};
        peer.nextOffer = now + delays[std::min(peer.offers, 3)];
        ++peer.offers;
        HostCommand d;
        d.kind = HostCommand::Connect;
        d.name = peer.inbox;
        _host.Command(d);
    }

    // ---------------------------------------------------------------- links

    void Node::Reevaluate(int64_t now)
    {
        std::vector<Peer*> all;
        for (auto& [id, p] : _peers)
            all.push_back(p.get());
        for (Peer* p : all)
            Consider(*p, now);
    }

    void Node::Consider(Peer& peer, int64_t now)
    {
        bool wanted = !_listener.Blocked(peer.cert) && _listener.WantLink(peer);
        if (wanted)
        {
            peer.notWantedSince = 0;
            if (!peer.wanted)
                peer.wantedSince = now;
            peer.wanted = true;
            if (!peer.linkUp && !peer.helloSent)
                OpenLinkTo(peer, now);
        }
        else
        {
            peer.wanted = false;
            if (!peer.notWantedSince)
                peer.notWantedSince = now;
        }
    }

    void Node::SendHello(Peer& peer, int64_t now)
    {
        if (!peer.link)
            peer.link = std::make_unique<Link>(_account, _me, _inboxHex, _scope.shard ? &_scope.proofKey : nullptr);
        Bytes hello = peer.link->Hello(peer.inbox, NetNow(now));
        // the hello goes by the route in use at first, and by each existing route in turn when it is repeated: one dead route cannot hold the link back
        std::vector<int> routes;
        for (int r = 0; r <= MaxNodes; ++r)
            if (RouteExists(peer, r))
                routes.push_back(r);
        int route = routes.empty() ? -1 : (peer.helloRound == 0 && peer.route >= 0 ? peer.route : routes[peer.helloRound % routes.size()]);
        ++peer.helloRound;
        peer.lastHello = now;
        HostCommand c;
        c.kind = HostCommand::Send;
        c.name = peer.inbox;
        c.data = hello;
        c.route = route < 0 ? RouteAny : static_cast<uint8_t>(route);
        _host.Command(c);
    }

    void Node::BringUp(Peer& peer, int64_t now)
    {
        peer.linkUp = true;
        peer.linkUpAt = now;
        peer.lastHeard = now;
        peer.heardFrame = false;
        peer.helloSent = true;
        peer.reliable.Reset();
        peer.keyed.Reset();
        if (peer.route < 0 || !RouteExists(peer, peer.route) || peer.routes[0].open)
            peer.route = PickRoute(peer);   // (a link that comes up starts on the direct route when there is one: nothing is measured yet)
        peer.routeSince = now;
        peer.direct = peer.route == 0;
        peer.sent = peer.received = 0;
        Probe(peer, std::max(peer.route, 0), now);
        Writer p;
        p.Blob(_presence);
        SendKeyedMessage(peer, 0xFFFF000000000001ull, MsgPresence, p.data);
        _listener.OnLinkUp(peer);
        UpdateTouch(peer);
        if (peer.lostLink)
        {
            peer.lostLink = false;
            NetEvent("link-restored", &peer, "The connection to {player} was restored.");
        }
    }

    void Node::OpenLinkTo(Peer& peer, int64_t now)
    {
        SendHello(peer, now);
        peer.helloSent = true;
        MaybeOffer(peer, now);
        if (peer.link->Up() && !peer.linkUp)
            BringUp(peer, now);
    }

    void Node::RestartLink(Peer& peer, std::string const& why, int64_t now)
    {
        bool wasUp = peer.linkUp;
        peer.linkUp = false;
        peer.helloSent = false;
        peer.heardFrame = false;
        peer.reliable.Reset();
        peer.keyed.Reset();
        if (!peer.link)
            peer.link = std::make_unique<Link>(_account, _me, _inboxHex, _scope.shard ? &_scope.proofKey : nullptr);
        peer.link->Restart();
        if (wasUp)
        {
            peer.lostLink = true;
            _listener.OnLinkDown(peer, why);
            NetEvent("link-lost", &peer, "The connection to {player} was lost (" + why + ").");
        }
        UpdateTouch(peer);
        OpenLinkTo(peer, now);
    }

    void Node::CloseLink(Peer& peer, std::string const& why, bool tell, int64_t now)
    {
        if (tell && peer.linkUp && peer.link && peer.link->Up())
        {
            // (said once on the route in use, outside the reliable channel: the link is closing)
            Writer m;
            m.U16(MsgBye);
            Transmit(peer, peer.link->Seal(Reliable, m.data), Reliable, peer.route);
        }
        bool wasUp = peer.linkUp;
        peer.linkUp = false;
        peer.helloSent = false;
        peer.heardFrame = false;
        peer.offers = 0;
        peer.nextOffer = 0;
        peer.link.reset();
        peer.samples.clear();
        peer.rtt = -1;
        peer.reliable.Reset();
        peer.keyed.Reset();
        for (RouteStats& rs : peer.routes)
        {
            rs.results.clear();
            rs.outstanding.clear();
            rs.better = 0;
        }
        peer.routes[0].open = false;
        peer.route = -1;
        HostCommand c;
        c.kind = HostCommand::Disconnect;
        c.name = peer.inbox;
        _host.Command(c);
        if (wasUp)
        {
            bool unplanned = why == "timeout" || why == "restarted";
            if (unplanned)
            {
                peer.lostLink = true;
                NetEvent("link-lost", &peer, "The connection to {player} was lost (" + why + ").");
            }
            _listener.OnLinkDown(peer, why);
        }
        UpdateTouch(peer);
    }

    void Node::SendReset(std::string const& toInbox, int route, int64_t now)
    {
        auto it = _resetSent.find(toInbox);
        if (it != _resetSent.end() && now - it->second < 3000)
            return;
        _resetSent[toInbox] = now;
        if (_resetSent.size() > 256)
            for (auto i = _resetSent.begin(); i != _resetSent.end();)
                i = now - i->second > 60000 ? _resetSent.erase(i) : std::next(i);
        HostCommand c;
        c.kind = HostCommand::Send;
        c.name = toInbox;
        c.data = Link::MakeReset(_account, _me, _inboxHex, toInbox, NetNow(now));
        c.route = route < 0 ? RouteAny : static_cast<uint8_t>(route);
        _host.Command(c);
    }

    void Node::OnInboxMessage(std::string const& fromInbox, Bytes const& data, int route, int64_t now)
    {
        if (data.empty())
            return;
        if (data[0] == 'E')
        {
            Envelope env;
            Bytes body(data.begin() + 1, data.end());
            if (!Envelope::Decode(body, env) || env.room != InboxTopic(_scope, _inboxHex))
                return;
            if (!CertOk(env.sender) || !env.Verify(nullptr, true) || env.sender.id == _me.id || env.sender.accountKey == _me.accountKey)
                return;
            bool duplicate = false;
            if (!Fresh(env, now, duplicate) || duplicate)
                return;
            OnEnvelope(env, env.room, true, route, now);
            return;
        }
        if (data[0] != 'L')
            return;
        Peer* peer = PeerByInbox(fromInbox);
        if (Link::IsReset(data))
        {
            Certificate cert;
            std::string from, to;
            int64_t time = 0;
            if (!Link::ParseReset(data, cert, from, to, time) || to != _inboxHex || from != fromInbox || !peer || peer->cert.id != cert.id)
                return;
            int64_t net = NetNow(now);
            if (time < net - _config.maxSkewMs || time > net + _config.maxSkewMs || now - peer->lastReset < 3000)
                return;
            peer->lastReset = now;
            Heard(*peer, route, now);
            if (peer->link && (peer->linkUp || peer->helloSent))
                RestartLink(*peer, "reset by the peer", now);
            return;
        }
        if (Link::IsHello(data))
        {
            if (!peer)
            {
                // a hello from someone we have not heard announce: check it on a probe link, then keep that link
                auto probe = std::make_unique<Link>(_account, _me, _inboxHex, _scope.shard ? &_scope.proofKey : nullptr);
                if (!probe->OnHello(data, NetNow(now), _config.maxSkewMs))
                    return;
                Certificate cert = probe->Peer();
                if (_listener.Blocked(cert) || cert.accountKey == _me.accountKey)
                    return;
                Peer& created = Touch(cert, fromInbox, now);
                created.link = std::move(probe);
                peer = &created;
                CertOk(cert);
            }
            else
            {
                if (!peer->link)
                    peer->link = std::make_unique<Link>(_account, _me, _inboxHex, _scope.shard ? &_scope.proofKey : nullptr);
                bool wasUp = peer->link->Up();
                if (!peer->link->OnHello(data, NetNow(now), _config.maxSkewMs))
                    return;
                if (wasUp && !peer->link->Up())
                {
                    // the peer started over: the link is not up any more until the hello is answered
                    peer->linkUp = false;
                    peer->helloSent = false;
                    peer->heardFrame = false;
                    peer->reliable.Reset();
                    peer->keyed.Reset();
                    peer->lostLink = true;
                    _listener.OnLinkDown(*peer, "restarted");
                    NetEvent("link-lost", peer, "The connection to {player} was lost (restarted).");
                    UpdateTouch(*peer);
                }
            }
            peer->lastHeard = now;
            Heard(*peer, route, now);
            bool wanted = _listener.WantLink(*peer) && !_listener.Blocked(peer->cert);
            if (wanted || peer->helloSent)
            {
                peer->wanted = wanted ? true : peer->wanted;
                if (!peer->link->HelloMade() || !peer->helloSent || !peer->linkUp)
                    OpenLinkTo(*peer, now);
            }
            return;
        }
        if (!Link::IsData(data))
            return;
        OpenResult result = peer && peer->link ? OnLinkFrame(*peer, data, route, now) : OpenResult::NotUp;
        // A frame that cannot be opened while nothing of this peer has opened for a while: tell it, so that it starts the handshake over
        if (result == OpenResult::Bad || result == OpenResult::NotUp)
            if (!peer || now - peer->lastHeard >= 3000)
                SendReset(fromInbox, route, now);
    }

    OpenResult Node::OnLinkFrame(Peer& peer, Bytes const& frame, int route, int64_t now)
    {
        Channel channel;
        Bytes plain;
        OpenResult result = peer.link->Try(frame, channel, plain);
        if (result == OpenResult::Replay)
            Heard(peer, route, now);
        if (result != OpenResult::Ok)
            return result;
        peer.lastHeard = now;
        peer.heardFrame = true;
        ++peer.received;
        Heard(peer, route, now);
        if (!peer.linkUp)
        {
            // the first frame that opens proves the other side has the keys
            BringUp(peer, now);
        }
        Reader r(plain);
        uint16_t type = r.U16();
        if (!r.ok())
            return result;
        Id128 id = peer.cert.id;
        if (type == MsgReliable)
        {
            uint64_t seq = r.U64();
            if (!r.ok())
                return result;
            std::vector<Bytes> ready;
            peer.reliable.OnData(seq, Bytes(r.here(), r.here() + r.remaining()), ready);
            for (Bytes const& message : ready)
            {
                Peer* p = Find(id);
                if (!p || !p->linkUp)
                    break;
                Deliver(*p, message, Reliable, route, now);
            }
            if (Peer* p = Find(id))
                if (p->linkUp)
                    PumpReliable(*p, now);
        }
        else if (type == MsgAck)
        {
            uint64_t cumulative = r.U64(), early = r.U64();
            if (r.ok())
            {
                peer.reliable.OnAck(cumulative, early);
                PumpReliable(peer, now);
            }
        }
        else if (type == MsgKeyed)
        {
            uint64_t key = r.U64(), version = r.U64();
            if (!r.ok())
                return result;
            if (peer.keyed.OnData(key, version))
                Deliver(peer, Bytes(r.here(), r.here() + r.remaining()), Unreliable, route, now);
            if (Peer* p = Find(id))
                if (p->linkUp)
                    PumpReliable(*p, now);
        }
        else if (type == MsgKeyedAck)
        {
            unsigned count = r.U16();
            for (unsigned i = 0; i < count && r.ok(); ++i)
            {
                uint64_t key = r.U64(), version = r.U64();
                if (r.ok())
                    peer.keyed.OnAck(key, version);
            }
            PumpReliable(peer, now);
        }
        else if (type < MsgFirstApplication)
            HandleInternal(peer, type, r, route, now);
        else
            _listener.OnMessage(peer, type, r, channel);
        return result;
    }

    void Node::Deliver(Peer& peer, Bytes const& message, Channel channel, int route, int64_t now)
    {
        Reader r(message);
        uint16_t type = r.U16();
        if (!r.ok())
            return;
        if (type < MsgFirstApplication)
            HandleInternal(peer, type, r, route, now);
        else
            _listener.OnMessage(peer, type, r, channel);
    }

    void Node::Transmit(Peer& peer, Bytes const& frame, Channel channel, int route)
    {
        if (frame.empty())
            return;
        HostCommand c;
        c.kind = HostCommand::Send;
        c.name = peer.inbox;
        c.data = frame;
        c.channel = channel;
        c.route = route < 0 ? RouteAny : static_cast<uint8_t>(route);
        _host.Command(c);
        ++peer.sent;
    }

    void Node::PumpReliable(Peer& peer, int64_t now)
    {
        if (!peer.link || !peer.link->Up())
            return;
        int64_t rtt = peer.rtt;
        if (peer.route >= 0 && peer.routes[peer.route].Median() >= 0)
            rtt = peer.routes[peer.route].Median();
        peer.reliable.Due(now, rtt, [&](ReliableStream::Frame f)
        {
            Writer w;
            w.U16(MsgReliable).U64(f.seq).Raw(*f.message);
            Transmit(peer, peer.link->Seal(Reliable, w.data), Reliable, peer.route);
        });
        peer.keyed.Due(now, rtt, [&](uint64_t key, uint64_t version, Bytes const& message)
        {
            Writer w;
            w.U16(MsgKeyed).U64(key).U64(version).Raw(message);
            Transmit(peer, peer.link->Seal(Unreliable, w.data), Unreliable, peer.route);
        });
        while (peer.keyed.AckDue())
        {
            auto acks = peer.keyed.TakeAcks();
            Writer w;
            w.U16(MsgKeyedAck).U16(static_cast<uint16_t>(acks.size()));
            for (auto const& [key, version] : acks)
                w.U64(key).U64(version);
            Transmit(peer, peer.link->Seal(Unreliable, w.data), Unreliable, peer.route);
        }
        if (peer.reliable.AckDue())
        {
            uint64_t cumulative, early;
            peer.reliable.MakeAck(cumulative, early);
            Writer w;
            w.U16(MsgAck).U64(cumulative).U64(early);
            Transmit(peer, peer.link->Seal(Unreliable, w.data), Unreliable, peer.route);
        }
    }

    void Node::SendMessage(Peer& peer, uint16_t type, Bytes const& body, Channel channel)
    {
        if (!peer.link || !peer.link->Up())
            return;
        Writer w;
        w.U16(type).Raw(body);
        if (channel == Unreliable)
        {
            Transmit(peer, peer.link->Seal(Unreliable, w.data), Unreliable, peer.route);
            return;
        }
        if (!peer.reliable.Queue(w.data))
        {
            _listener.OnLog("reliable queue to " + peer.name + " is full: a message of type " + std::to_string(type) + " was dropped");
            return;
        }
        PumpReliable(peer, _now);
    }

    bool Node::Send(Id128 const& id, uint16_t type, Bytes const& body, Channel channel)
    {
        Peer* p = Find(id);
        if (!p || !p->linkUp)
            return false;
        SendMessage(*p, type, body, channel);
        return true;
    }

    bool Node::SendKeyedMessage(Peer& peer, uint64_t key, uint16_t type, Bytes const& body)
    {
        if (!peer.link || !peer.link->Up())
            return false;
        Writer w;
        w.U16(type).Raw(body);
        if (!peer.keyed.Set(key, w.data))
        {
            _listener.OnLog("keyed queue to " + peer.name + " is full: a message of type " + std::to_string(type) + " was dropped");
            return false;
        }
        PumpReliable(peer, _now);
        return true;
    }

    bool Node::SendKeyed(Id128 const& id, uint64_t key, uint16_t type, Bytes const& body)
    {
        Peer* p = Find(id);
        return p && p->linkUp && SendKeyedMessage(*p, key, type, body);
    }

    size_t Node::BroadcastKeyed(std::function<bool(Peer const&)> const& filter, uint64_t key, uint16_t type, Bytes const& body)
    {
        size_t n = 0;
        for (auto& [id, p] : _peers)
            if (p->linkUp && (!filter || filter(*p)))
                n += SendKeyed(id, key, type, body) ? 1 : 0;
        return n;
    }

    size_t Node::Broadcast(std::function<bool(Peer const&)> const& filter, uint16_t type, Bytes const& body, Channel channel)
    {
        size_t n = 0;
        for (auto& [id, p] : _peers)
            if (p->linkUp && (!filter || filter(*p)))
            {
                SendMessage(*p, type, body, channel);
                ++n;
            }
        return n;
    }

    void Node::HandleInternal(Peer& peer, uint16_t type, Reader& body, int route, int64_t now)
    {
        switch (type)
        {
            case MsgPing:
            {
                uint32_t id = body.U32();
                int64_t t1 = body.I64();
                if (!body.ok())
                    return;
                Writer w;
                int64_t t2 = NetNow(now);
                w.U16(MsgPong).U32(id).I64(t1).I64(t2).I64(t2);
                // answered on the route it came by: that is what is measured
                Transmit(peer, peer.link->Seal(Unreliable, w.data), Unreliable, route);
                break;
            }
            case MsgPong:
            {
                uint32_t id = body.U32();
                int64_t t1 = body.I64(), t2 = body.I64(), t3 = body.I64(), t4 = NetNow(now);
                if (!body.ok())
                    return;
                // the quality of the route that carried the probe
                for (int r = 0; r <= MaxNodes; ++r)
                {
                    auto& out = peer.routes[r].outstanding;
                    for (auto it = out.begin(); it != out.end(); ++it)
                        if (it->first == id)
                        {
                            RouteStats& rs = peer.routes[r];
                            rs.results.push_back(static_cast<int32_t>(std::max<int64_t>(0, now - it->second)));
                            if (rs.results.size() > 20)
                                rs.results.pop_front();
                            rs.answeredAt = now;
                            out.erase(it);
                            r = MaxNodes + 1;
                            break;
                        }
                }
                // the network clock
                int64_t rtt = (t4 - t1) - (t3 - t2);
                if (rtt < 0)
                    return;
                int64_t theta = ((t2 - t1) + (t3 - t4)) / 2;
                peer.samples.emplace_back(rtt, theta);
                if (peer.samples.size() > 8)
                    peer.samples.pop_front();
                auto best = std::min_element(peer.samples.begin(), peer.samples.end());
                peer.rtt = best->first;
                peer.theta = best->second;
                break;
            }
            case MsgBye:
                CloseLink(peer, "peer closed", false, now);
                break;
            case MsgPresence:
            {
                Bytes app = body.Blob(1 << 14);
                if (!body.ok())
                    return;
                bool changed = peer.appData != app;
                peer.appData = app;
                if (changed)
                {
                    _listener.OnPeerAnnounced(peer, false);
                    Consider(peer, now);
                }
                break;
            }
            default:
                break;
        }
    }

    // ---------------------------------------------------------------- time and upkeep

    void Node::UpdateClock(int64_t now)
    {
        std::vector<int64_t> values{0};
        for (auto& [id, p] : _peers)
            if (p->linkUp && p->rtt >= 0 && !p->samples.empty())
                values.push_back(p->theta);
        if (values.size() < 2)
            return;
        std::sort(values.begin(), values.end());
        int64_t median = values.size() % 2 ? values[values.size() / 2] : (values[values.size() / 2 - 1] + values[values.size() / 2]) / 2;
        int64_t step = std::max(-_config.maxSlewMs, std::min(_config.maxSlewMs, median));
        _offset += step;
        for (auto& [id, p] : _peers)
        {
            p->samples.clear();   // (they were measured against the old offset)
            p->rtt = -1;
        }
    }

    void Node::Upkeep(Peer& peer, int64_t now)
    {
        // probes that were not answered in time are lost
        for (RouteStats& rs : peer.routes)
            while (!rs.outstanding.empty() && now - rs.outstanding.front().second > _config.probeLostAfterMs)
            {
                rs.outstanding.pop_front();
                rs.results.push_back(-1);
                if (rs.results.size() > 20)
                    rs.results.pop_front();
            }
        // probes: often on the route in use (every second while the clock converges), rarely on the others
        for (int r = 0; r <= MaxNodes; ++r)
        {
            if (!RouteExists(peer, r))
                continue;
            int64_t every = r == peer.route ? (now - peer.linkUpAt < 30000 ? 1000 : _config.probeEveryMs) : _config.probeOtherEveryMs;
            if (now - peer.routes[r].probeSentAt >= every)
                Probe(peer, r, now);
        }
        if (now - peer.lastEval >= _config.probeEveryMs)
        {
            peer.lastEval = now;
            EvaluateRoutes(peer, now);
        }
        MaybeOffer(peer, now);
        // our hello again, until a frame of this link has opened: the peer may not have it
        if (!peer.heardFrame && now - peer.lastHello >= 3000)
            SendHello(peer, now);
    }

    void Node::Maintain(int64_t now)
    {
        // forget old envelope hashes
        while (!_seenOrder.empty() && _seenOrder.front().first < now - 2 * _config.maxSkewMs - 1000)
        {
            _seen.erase(_seenOrder.front().second);
            _seenOrder.pop_front();
        }
        for (size_t k = 0; k < _nodes.size(); ++k)
            if (_nodes[k].up && _nodes[k].lostTold && now - _nodes[k].upSince >= _config.nodeStableMs)
            {
                _nodes[k].lostTold = false;
                NetEvent("node-back", nullptr, "The connection to relay #" + std::to_string(k + 1) + " works again.");
            }
        if (_nodeUp && now - _lastAnnounce >= _config.announceEveryMs)
        {
            _lastAnnounce = now;
            for (auto const& [name, room] : _rooms)
                AnnounceIn(room, now);
        }
        if (now - _lastClock >= _config.clockEveryMs)
        {
            _lastClock = now;
            UpdateClock(now);
        }
        std::vector<Peer*> peers;
        for (auto& [id, p] : _peers)
            peers.push_back(p.get());
        for (Peer* p : peers)
        {
            Consider(*p, now);
            UpdateTouch(*p);
            if (p->linkUp)
            {
                if (now - p->lastHeard > _config.peerTimeoutMs)
                {
                    CloseLink(*p, "timeout", false, now);
                    continue;
                }
                if (!p->wanted && p->notWantedSince && now - p->notWantedSince > _config.linkWantedGraceMs)
                {
                    CloseLink(*p, "nothing in common", true, now);
                    continue;
                }
                Upkeep(*p, now);
            }
            else if (p->helloSent && p->wanted && now - p->lastHello > 3000)
            {
                SendHello(*p, now);   // the hello again (it was lost, or the peer's host was not ready)
                p->helloSent = true;
            }
            if (!p->linkUp && now - p->lastSeen > 3 * _config.announceEveryMs)
            {
                bool carried = p->routes[0].open;
                for (int r = 1; r <= MaxNodes && !carried; ++r)
                    carried = RouteExists(*p, r);
                if (p->rooms.empty() || !carried)
                    DropPeer(*p, true);
            }
        }
    }
}
