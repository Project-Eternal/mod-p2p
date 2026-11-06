#include "P2PService.h"
#include "Characters.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GitRevision.h"
#include "GridDefines.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "Mirrors.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include "World.h"
#include "WoWProcess.h"
#include <chrono>

extern "C" void mysql_sqlite_save_now(void);   // (the SQLite shim: save every working copy now)
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include <sstream>

namespace p2p
{
    namespace
    {
        bool Has(std::string const& room, char const* part) { return room.find(part) != std::string::npos; }
    }

    Service& Service::Get()
    {
        static Service service;
        return service;
    }

    int64_t Service::MachineNow()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    int64_t Service::NetNow() const { return MachineNow() + _offset.load(std::memory_order_relaxed); }

    // ---------------------------------------------------------------- start and stop

    void Service::Startup()
    {
        if (!sConfigMgr->GetOption<bool>("P2P.Enable", true))
            return;
        _realm = sConfigMgr->GetOption<uint32>("RealmID", 1);   // (the realm of the server's own number: players meet only players of the same realm)
        if (!_host.Attach())
        {
            LOG_INFO("module", "[P2P] No peer host (the page has no relay node set): the world stays private");
            return;
        }
        _chatEvents = _host.ChatEvents();
        Store::EnsureTables();
        _account = Store::LoadOrCreateAccount();
        Characters::Get().Load(_account);
        _enabled = true;
        _running = true;
        _thread = std::thread([this] { Run(); });
        LOG_INFO("module", "[P2P] Ready, realm {} account {}", _realm, Hex(_account.signing.pub).substr(0, 16));
    }

    void Service::OnRealmUnbind(uint32_t realm)
    {
        if (!_running)
            return;
        LOG_INFO("module", "[P2P] Leaving realm {}", realm);
        for (Feature* f : _features)
            f->OnRealmUnbind();
        Characters::Get().Clear();
        _account = Account{};
        _me = Certificate{};
    }

    void Service::OnRealmBind(uint32_t realm)
    {
        if (!_running)
            return;   // (the first realm is bound before Startup, which does the same)
        _realm = realm;
        Store::EnsureTables();
        _account = Store::LoadOrCreateAccount();
        Characters::Get().Load(_account);
        LOG_INFO("module", "[P2P] Realm {}, account {}", _realm, Hex(_account.signing.pub).substr(0, 16));
    }

    void Service::Shutdown()
    {
        if (!_running)
        {
            _stopped = true;
            return;
        }
        End();
        _stop = true;
        _host.Kick();
        if (_thread.joinable())
            _thread.join();
        _running = false;
        _stopped = true;
    }

    // The server stops in order, through the core's own shutdown, which logs the player out and saves it: first what belongs to the shared world is handed
    // over (the creatures this server decides, a trade window that is not yet accepted), the others are told, and the host publishes its wills and closes.
    void Service::StopInOrder()
    {
        Player* local = Local();
        if (_enabled && local)
        {
            for (Feature* f : _features)
                f->OnStopping(local);
            _orderlyDone = true;
            End();
        }
        World::StopNow(SHUTDOWN_EXIT_CODE);
    }

    void Service::KeepNow(Player* player, CharacterDatabaseTransaction trans)
    {
        if (!trans)
            trans = CharacterDatabase.BeginTransaction();
        if (player && !player->IsMirror() && player->IsInWorld())
            player->SaveToDB(trans, false, false);
        // (the player's save uses statements that are prepared on the asynchronous connections only: DirectCommitTransaction would run it on a synchronous one and abort the server)
        CharacterDatabase.AsyncCommitTransaction(trans).m_future.wait();
        mysql_sqlite_save_now();
        if (WoWProcess* page = GetWoWProcess())
            std::thread([page] { page->KeepNow(); }).detach();   // (the page may take a moment to answer; the world thread does not wait)
        LOG_INFO("module", "[P2P] {} kept at once", player ? player->GetName() : std::string("the database"));
    }

    void Service::Command(std::function<void()> f)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _commands.push_back(std::move(f));
    }

    void Service::Post(std::function<void(Node&)> f)
    {
        Command([this, f = std::move(f)] { if (_node) f(*_node); });
    }

    void Service::Defer(std::function<void()> f)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_deferred.size() < 5000)
            _deferred.push_back(std::move(f));
    }

    void Service::Run()
    {
        while (!_stop)
        {
            _host.Wait(20);
            std::deque<std::function<void()>> commands;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                commands.swap(_commands);
            }
            for (auto& f : commands)
                f();
            if (_node)
            {
                int64_t now = MachineNow();
                _node->Tick(now);
                _offset.store(_node->Offset(), std::memory_order_relaxed);
                if (now - _figuresAt >= 1000)
                {
                    _figuresAt = now;
                    RefreshFigures();
                }
            }
        }
        if (_node)
        {
            _node->Stop(MachineNow());
            _node.reset();
        }
        if (_qualityShown.exchange(false))
        {
            HostCommand c;
            c.kind = HostCommand::Quality;
            c.text = quality::Text(quality::Stopped());
            _host.Command(c);
        }
    }

    // p2p thread: a copy of what the node measures, for the world thread
    void Service::RefreshFigures()
    {
        int64_t now = MachineNow();
        std::map<Id128, LinkFigures> figures;
        _node->ForEachPeer([&](Peer& p)
        {
            LinkFigures f;
            f.linked = p.linkUp;
            f.direct = p.linkUp && p.route == 0;
            f.inTouch = p.inTouch;
            f.route = p.route >= 0 ? _node->RouteName(p.route) : "none";
            f.routeLabel = p.route < 0 ? "none" : p.route == 0 ? "direct" : "relay #" + std::to_string(p.route);
            if (p.route >= 0)
            {
                f.rtt = p.routes[p.route].Median();
                f.loss = p.routes[p.route].Loss();
            }
            f.upForMs = p.linkUp ? now - p.linkUpAt : 0;
            f.silentMs = p.linkUp ? now - p.lastHeard : 0;
            f.directReason = p.directReason;
            f.sent = p.sent;
            f.received = p.received;
            f.inbox = p.inbox;
            f.rooms.assign(p.rooms.begin(), p.rooms.end());
            for (int r = 0; r <= MaxNodes; ++r)
            {
                if (!_node->RouteExists(p, r) && p.routes[r].Finished() == 0)
                    continue;
                RouteFigures rf;
                rf.name = _node->RouteName(r);
                rf.exists = _node->RouteExists(p, r);
                rf.inUse = r == p.route;
                rf.rtt = p.routes[r].Median();
                rf.loss = p.routes[r].Loss();
                rf.probes = static_cast<uint32_t>(p.routes[r].Finished());
                f.routes.push_back(rf);
            }
            figures[p.cert.id] = std::move(f);
        });
        std::vector<NodeFigures> nodes;
        for (Node::NodeInfo const& n : _node->Nodes())
            nodes.push_back({n.address, n.up, n.everUp, n.removed, n.since, n.reason});
        std::lock_guard<std::mutex> lock(_figuresMutex);
        _figures.swap(figures);
        _nodeFigures.swap(nodes);
    }

    bool Service::Figures(Id128 const& peer, LinkFigures& out)
    {
        std::lock_guard<std::mutex> lock(_figuresMutex);
        auto it = _figures.find(peer);
        if (it == _figures.end())
            return false;
        out = it->second;
        return true;
    }

    std::vector<NodeFigures> Service::NodeList()
    {
        std::lock_guard<std::mutex> lock(_figuresMutex);
        return _nodeFigures;
    }

    void Service::RelayAdd(std::string const& address, bool tried)
    {
        Post([address, tried](Node& n) { n.AddRelay(address, tried); });
        _host.Kick();
    }

    void Service::RelayRemove(unsigned number)
    {
        Post([number](Node& n) { n.RemoveRelay(number); });
        _host.Kick();
    }

    // .p2p relays: every node of the list with its number, whether it is reached and since when, or why not
    std::vector<std::string> Service::DescribeRelays()
    {
        std::vector<std::string> lines;
        int64_t now = MachineNow();
        auto sec = [](int64_t ms) { return ms < 90000 ? std::to_string(ms / 1000) + " s" : std::to_string(ms / 60000) + " min"; };
        std::vector<NodeFigures> nodes = NodeList();
        for (size_t k = 0; k < nodes.size(); ++k)
        {
            NodeFigures const& n = nodes[k];
            if (n.removed)
                continue;
            std::string state = n.up ? "reached for " + sec(now - n.sinceMs) : n.everUp ? "lost " + sec(now - n.sinceMs) + " ago" :
                n.reason.empty() ? "not reached yet" : "not reached (" + n.reason + ")";
            lines.push_back(Acore::StringFormat("relay #{} {}: {}", k + 1, n.address, state));
        }
        if (lines.empty())
            lines.push_back("no relay nodes (the shared world needs one: .p2p relay add ADDRESS)");
        return lines;
    }

    bool Service::Responsive(Id128 const& peer)
    {
        std::lock_guard<std::mutex> lock(_figuresMutex);
        auto it = _figures.find(peer);
        return it == _figures.end() || !it->second.linked || it->second.silentMs < 5000;
    }

    // a character met: its pair room is kept until logout, so that it can be reached (whisper, mail) wherever it goes
    void Service::Meet(Id128 const& peer)
    {
        if (peer == Id128{} || peer == _me.id)
            return;
        _met[peer] = NetNow();
        if (_met.size() > 300)
        {
            auto oldest = _met.begin();
            for (auto it = _met.begin(); it != _met.end(); ++it)
                if (it->second < oldest->second)
                    oldest = it;
            _met.erase(oldest);
        }
        _refresh = true;
    }

    // The rooms' scope: the realm, or the private shard's prefix and key. The key comes from the identifier with a deliberately slow function (about a second:
    // it runs once per realm and session, at the login, where the game shows its loading screen anyway).
    void Service::PrepareScope()
    {
        std::string const& identifier = _host.Shard();
        if (identifier.empty())
        {
            _scope = Scope(_realm);
            return;
        }
        if (_scope.shard && _scope.realm == _realm)
            return;
        int64_t t0 = MachineNow();
        _scope = Scope::Shard(_realm, Scope::DeriveShardKey(identifier, _realm));
        LOG_INFO("module", "[P2P] Private shard {} on realm {} (derived in {} ms)", Hex(crypto::Sha256(ToBytes(_scope.prefix))).substr(0, 8), _realm, MachineNow() - t0);
    }

    // a character has entered the world: this server announces it
    void Service::Begin()
    {
        Certificate me = _me;
        Account account = _account;
        uint32_t realm = _realm;
        Scope scope = _scope;
        Command([this, me, account, realm, scope]
        {
            Node::Config config;
            config.realm = realm;
            config.shard = scope.shard;
            config.shardKey = scope.key;
            _node = std::make_unique<Node>(config, account, me, _host, *this);
            _node->Start(MachineNow());
        });
        _host.Kick();
    }

    void Service::End()
    {
        Command([this]
        {
            if (_node)
            {
                _node->Stop(MachineNow());
                _node.reset();
            }
            _nodeUp = false;
            if (_qualityShown.exchange(false))
            {
                HostCommand c;
                c.kind = HostCommand::Quality;
                c.text = quality::Text(quality::Stopped());   // the stop of the shared world: the page takes its bar away
                _host.Command(c);
            }
        });
        _host.Kick();
    }

    // ---------------------------------------------------------------- the player

    Player* Service::Local() const { return _local ? ObjectAccessor::FindPlayer(_local) : nullptr; }

    void Service::OnLogin(Player* player)
    {
        if (!_enabled || player->GetSession()->IsHeadless())
            return;   // (the mirrors of other players are headless: only a real player's login starts the node)
        if (_local && _local != player->GetGUID())
            return;
        _local = player->GetGUID();
        _me = Store::OwnCharacter(_account, player->GetGUID().GetCounter(), player->GetName());
        _peers.clear();
        _met.clear();
        _routeTold.clear();
        _lastChat.clear();
        _lastPresence.clear();
        _lastRoomNames.clear();
        _instanceKeys.clear();
        _refresh = true;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _interest = Interest();
            _interest.active = true;
        }
        PrepareScope();
        for (Feature* f : _features)
            f->OnLogin(player);
        Begin();
        LOG_INFO("module", "[P2P] {} is {}", player->GetName(), Hex(_me.id));
    }

    void Service::OnLogout(Player* player)
    {
        if (!_enabled || player->GetGUID() != _local)
            return;
        // a player whose connection was lost (the core logs it out at once: OfflineSessionTime = 0) leaves the shared world as in an orderly stop: the
        // creatures it decides are handed over, an unaccepted trade is cancelled, the others hear the goodbye
        if (!_orderlyDone && player->IsInWorld())
            for (Feature* f : _features)
                f->OnStopping(player);
        _orderlyDone = false;
        for (Feature* f : _features)
            f->OnLogout(player);
        End();
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _interest = Interest();
            _events.clear();
        }
        _local = ObjectGuid::Empty;
        _peers.clear();
    }

    PeerInfo* Service::FindPeer(Id128 const& id)
    {
        auto it = _peers.find(id);
        return it == _peers.end() ? nullptr : &it->second;
    }

    void Service::Tell(Player* player, std::string const& text)
    {
        if (player && player->GetSession())
            ChatHandler(player->GetSession()).SendSysMessage(std::string("[") + GitRevision::GetCompanyNameStr() + "] " + text);
    }

    // ---------------------------------------------------------------- the world thread

    void Service::Update(uint32_t diff)
    {
        _updating = true;
        if (_stopRequested.exchange(false))
            StopInOrder();
        if (!_enabled)
            return;
        std::deque<InEvent> events;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            events.swap(_events);
        }
        std::vector<std::function<void()>> deferred;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            deferred.swap(_deferred);
        }
        for (auto& f : deferred)
            f();
        Player* local = Local();
        for (InEvent const& e : events)
            Apply(e);
        UpdateQuality(diff, local);
        if (!local || !local->IsInWorld())
            return;
        _refreshTimer += diff;
        if (_refresh || _refreshTimer >= 500)
        {
            _refreshTimer = 0;
            _refresh = false;
            RefreshInterest(local);
        }
        for (Feature* f : _features)
            f->Update(diff, local);
    }

    // The connection quality indicator: for each peer in touch the effective latency of the route in use (figures copied by the p2p
    // thread once a second), weighted by the distance of its mirror when it is around the local player; computed here because the mirrors and the
    // player's position belong to the world thread. The page gets the result through the ring once a second; core/Quality.h has the formulas.
    void Service::UpdateQuality(uint32_t diff, Player* local)
    {
        if (!local || !local->IsInWorld() || _orderlyDone)   // (after the orderly stop the page has had its "none")
            return;
        _qualityTimer += diff;
        if (_qualityTimer < 1000)
            return;
        _qualityTimer = 0;
        std::vector<quality::Sample> samples;
        if (_nodeUp)
        {
            float sight = local->GetMap() ? local->GetMap()->GetVisibilityRange() : 100.0f;
            for (auto const& [id, info] : _peers)
            {
                if (!info.inTouch)
                    continue;
                LinkFigures f;
                quality::Sample s;
                // (a peer whose route in use has no finished probe yet is not measured: it is left out, not counted as the worst)
                if (!Figures(id, f) || std::none_of(f.routes.begin(), f.routes.end(), [](RouteFigures const& r) { return r.inUse && r.probes > 0; }))
                    continue;
                s.rttMs = f.rtt;
                s.loss = f.loss;
                Player* mirror = Mirrors::Get().MirrorOf(id);
                if (mirror && mirror->IsInWorld() && mirror->GetMap() == local->GetMap())
                {
                    float d = local->GetExactDist(mirror);
                    if (d <= sight)
                    {
                        s.around = true;
                        s.yards = d;
                    }
                }
                samples.push_back(s);
            }
        }
        SendQuality(quality::Combine(samples));
    }

    void Service::SendQuality(quality::Result const& r)
    {
        _qualityShown = true;
        std::string text = quality::Text(r);
        Command([this, text]
        {
            HostCommand c;
            c.kind = HostCommand::Quality;
            c.text = text;
            _host.Command(c);
        });
        _host.Kick();
    }

    void Service::Apply(InEvent const& e)
    {
        Player* local = Local();
        switch (e.kind)
        {
            case InEvent::NodeState:
                _nodeUp = e.flag;
                if (local)
                    Tell(local, e.flag ? "Connected to a relay node. Other players can find you." : "No relay node can be reached, so the shared world is private for now.");
                break;
            case InEvent::Announced:
            {
                if (e.flag)
                    LOG_INFO("module", "[P2P] Met {} ({})", Hex(e.peer).substr(0, 8), e.data.size());
                PeerInfo& info = _peers[e.peer];
                info.cert = e.cert;
                info.version = game::Presence::VersionOf(e.data);
                info.compatible = info.version == game::Presence::Version;
                if (info.compatible)
                    game::Presence::Decode(e.data, info.presence);
                break;
            }
            case InEvent::LinkUp:
                LOG_INFO("module", "[P2P] Link up with {} ({})", Hex(e.peer).substr(0, 8), e.direct ? "direct" : "relayed");
                if (PeerInfo* p = FindPeer(e.peer))
                {
                    p->linked = true;
                    p->direct = e.direct;
                }
                Meet(e.peer);
                break;
            case InEvent::InTouch:
                if (PeerInfo* p = FindPeer(e.peer))
                    p->inTouch = e.flag;
                LOG_INFO("module", "[P2P] {} {} touch", NameOf(e.peer), e.flag ? "in" : "out of");
                break;
            case InEvent::NetEvent:
            {
                // the text is a sentence for the chat and the log; {player} stands for the name of the other player
                std::string chat = e.text, line = chat;
                if (e.peer != Id128{})
                    for (size_t at; (at = line.find("{player}")) != std::string::npos;)
                        line.replace(at, 8, NameOf(e.peer));
                LOG_INFO("module", "[P2P] {}", line);
                if (!local || !_chatEvents || e.what == "node-quiet")
                    break;
                {
                    // a message that says the same as the last one for that player or node (the same reason again) is for the log only
                    std::string who = Hex(e.peer);
                    if (e.peer == Id128{})
                    {
                        size_t second = chat.find(' ', chat.find(' ') + 1);
                        who = "node " + chat.substr(0, second);
                    }
                    std::string& last = _lastChat[who];
                    if (last == chat)
                        break;
                    last = chat;
                }
                if (e.what == "route" && !(e.peer == Id128{}))
                {
                    // a struggling connection does not fill the chat: one report per peer per 30 seconds
                    int64_t now = NetNow();
                    int64_t& told = _routeTold[e.peer];
                    if (now - told < 30000)
                        break;
                    told = now;
                }
                Tell(local, line);
                break;
            }
            case InEvent::LinkDown:
                LOG_INFO("module", "[P2P] Link down with {}: {}", Hex(e.peer).substr(0, 8), e.text);
                if (PeerInfo* p = FindPeer(e.peer))
                    p->linked = false;
                break;
            case InEvent::Gone:
                LOG_INFO("module", "[P2P] {} left the shared world", Hex(e.peer).substr(0, 8));
                break;
            case InEvent::Message:
                break;
        }
        for (Feature* f : _features)
            f->OnEvent(e, local);
        if (e.kind == InEvent::Gone)
            _peers.erase(e.peer);
    }

    // the presence and the rooms from where the character is; handed to the node when they changed
    void Service::RefreshInterest(Player* player)
    {
        game::Presence p;
        Map* map = player->GetMap();
        p.map = player->GetMapId();
        p.phaseMask = player->GetPhaseMask();
        p.zone = player->GetZoneId();
        p.area = player->GetAreaId();
        p.x = player->GetPositionX();
        p.y = player->GetPositionY();
        p.z = player->GetPositionZ();
        p.level = player->GetLevel();
        for (Feature* f : _features)
            f->Presence(player, p);
        if (map->Instanceable())
        {
            // the instance is the party's (the leader's) while the character is in a party, its own alone; a character that joins a party while
            // inside an instance keeps that instance: it joins the leader's the next time it enters
            auto cached = _instanceKeys.find({p.map, player->GetInstanceId()});
            if (cached == _instanceKeys.end())
            {
                Writer w;
                if (!(p.party == Id128{}))
                    w.Id(p.party);
                else
                    w.Id(_me.id).U32(player->GetInstanceId());
                w.U32(p.map);
                Key32 h = crypto::Sha256(w.data);
                uint64_t key;
                memcpy(&key, h.data(), 8);
                cached = _instanceKeys.insert({{p.map, player->GetInstanceId()}, key | 1}).first;
            }
            p.instanceKey = cached->second;
        }
        _myPresence = p;

        std::vector<Room> rooms;
        if (p.instanceKey)
        {
            Id128 key{};
            memcpy(key.data(), &p.instanceKey, 8);
            Room r;
            r.name = InstanceRoom(_scope, p.map, key);
            rooms.push_back(r);
        }
        else
        {
            // every tile (533 yards) the character can be seen from or see into
            float radius = std::max(map->GetVisibilityRange(), 100.0f);
            std::set<std::pair<int, int>> tiles;
            for (int dx = -1; dx <= 1; dx += 2)
                for (int dy = -1; dy <= 1; dy += 2)
                {
                    GridCoord g = Acore::ComputeGridCoord(p.x + dx * radius, p.y + dy * radius);
                    tiles.insert({int(g.x_coord), int(g.y_coord)});
                }
            for (auto const& [tx, ty] : tiles)
            {
                Room r;
                r.name = TileRoom(_scope, p.map, tx, ty);
                rooms.push_back(r);
            }
        }
        for (Feature* f : _features)
            f->Rooms(player, rooms);
        {
            // characters met in this session stay reachable: their pair rooms are kept until logout (the ones ignored are not)
            std::set<Id128> blocked;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                blocked = _interest.blocked;
            }
            std::set<std::string> have;
            for (Room const& r : rooms)
                have.insert(r.name);
            for (auto const& [id, at] : _met)
            {
                KnownCharacter* k = blocked.count(id) ? nullptr : Characters::Get().Find(id);
                Bytes secret;
                if (k && PairSecret(_account, _me, k->cert, secret))
                {
                    Room r = PrivateRoom(_scope, "pair", secret);
                    if (have.insert(r.name).second)
                        rooms.push_back(r);
                }
            }
        }

        std::vector<std::string> names;
        for (Room const& r : rooms)
            names.push_back(r.name);
        std::sort(names.begin(), names.end());
        Bytes encoded = p.Encode();
        bool roomsChanged = names != _lastRoomNames, presenceChanged = encoded != _lastPresence;
        if (!roomsChanged && !presenceChanged)
            return;
        _lastRoomNames = names;
        _lastPresence = encoded;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _interest.mine = p;
        }
        if (roomsChanged)
            LOG_INFO("module", "[P2P] Rooms {}", names.empty() ? std::string("none") : names.front() + (names.size() > 1 ? " +" + std::to_string(names.size() - 1) : std::string()));
        if (roomsChanged)
            Post([rooms](Node& n) { n.SetRooms(rooms); });
        if (presenceChanged)
            Post([encoded](Node& n) { n.SetPresence(encoded); });
        _host.Kick();
    }

    void Service::WantRefresh() { _refresh = true; }

    void Service::SetBlocked(std::set<Id128> const& blocked)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _interest.blocked = blocked;
    }

    void Service::Need(Id128 const& peer, bool needed)
    {
        if (needed)
            Meet(peer);
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (needed)
                _interest.needed.insert(peer);
            else
                _interest.needed.erase(peer);
        }
        Post([](Node& n) { n.Reevaluate(MachineNow()); });
        _host.Kick();
    }

    void Service::Say(Id128 const& peer, uint16_t type, Bytes const& body, Channel channel)
    {
        Post([peer, type, body, channel](Node& n) { n.Send(peer, type, body, channel); });
        _host.Kick();
    }

    void Service::SayAll(std::function<bool(PeerInfo const&)> const& filter, uint16_t type, Bytes const& body, Channel channel)
    {
        // the filter is evaluated here, on the world thread, against its own view of the peers
        std::vector<Id128> to;
        for (auto const& [id, info] : _peers)
            if (info.linked && (!filter || filter(info)))
                to.push_back(id);
        if (to.empty())
            return;
        Post([to, type, body, channel](Node& n) { for (Id128 const& id : to) n.Send(id, type, body, channel); });
        _host.Kick();
    }

    void Service::SayKeyed(Id128 const& peer, uint64_t key, uint16_t type, Bytes const& body)
    {
        Post([peer, key, type, body](Node& n) { n.SendKeyed(peer, key, type, body); });
        _host.Kick();
    }

    void Service::SayAllKeyed(std::function<bool(PeerInfo const&)> const& filter, uint64_t key, uint16_t type, Bytes const& body)
    {
        std::vector<Id128> to;
        for (auto const& [id, info] : _peers)
            if (info.linked && (!filter || filter(info)))
                to.push_back(id);
        if (to.empty())
            return;
        Post([to, key, type, body](Node& n) { for (Id128 const& id : to) n.SendKeyed(id, key, type, body); });
        _host.Kick();
    }

    // ---------------------------------------------------------------- the p2p thread (Listener)

    void Service::Push(InEvent&& e)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_events.size() < 20000)
            _events.push_back(std::move(e));
    }

    bool Service::Blocked(Certificate const& cert)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _interest.blocked.count(cert.id) > 0;
    }

    bool Service::WantLink(Peer const& peer)
    {
        game::Presence theirs;
        if (!peer.appData.empty() && game::Presence::VersionOf(peer.appData) != game::Presence::Version)
            return false;   // a server of another protocol version: it cannot be talked to (the status command says so)
        bool known = game::Presence::Decode(peer.appData, theirs);
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_interest.active)
            return false;
        if (_interest.needed.count(peer.cert.id))
            return true;
        if (!known)
            return !peer.rooms.empty();   // (their announcement has not come yet: the rooms said enough to ask)
        for (std::string const& room : peer.rooms)
        {
            if (Has(room, "/party/") || Has(room, "/chan/") || Has(room, "/zone/"))
                return true;
            if (Has(room, "/world/") && (theirs.phaseMask & _interest.mine.phaseMask) != 0 && theirs.instanceKey == _interest.mine.instanceKey)
                return true;
        }
        return false;
    }

    void Service::OnPeerAnnounced(Peer& peer, bool isNew)
    {
        InEvent e;
        e.kind = InEvent::Announced;
        e.peer = peer.cert.id;
        e.cert = peer.cert;
        e.data = peer.appData;
        e.flag = isNew;
        Push(std::move(e));
    }

    void Service::OnLinkUp(Peer& peer)
    {
        InEvent e;
        e.kind = InEvent::LinkUp;
        e.peer = peer.cert.id;
        e.cert = peer.cert;
        e.direct = peer.direct;
        Push(std::move(e));
    }

    void Service::OnLinkDown(Peer& peer, std::string const& why)
    {
        InEvent e;
        e.kind = InEvent::LinkDown;
        e.peer = peer.cert.id;
        e.text = why;
        Push(std::move(e));
    }

    void Service::OnPeerGone(Peer& peer)
    {
        InEvent e;
        e.kind = InEvent::Gone;
        e.peer = peer.cert.id;
        e.cert = peer.cert;
        Push(std::move(e));
    }

    void Service::OnInTouch(Peer& peer, bool inTouch)
    {
        InEvent e;
        e.kind = InEvent::InTouch;
        e.peer = peer.cert.id;
        e.cert = peer.cert;
        e.flag = inTouch;
        Push(std::move(e));
    }

    void Service::OnNetwork(std::string const& kind, Peer* peer, std::string const& text)
    {
        InEvent e;
        e.kind = InEvent::NetEvent;
        e.what = kind;
        e.text = text;
        if (peer)
            e.peer = peer->cert.id;
        Push(std::move(e));
    }

    void Service::OnMessage(Peer& peer, uint16_t type, Reader& body, Channel channel)
    {
        InEvent e;
        e.kind = InEvent::Message;
        e.peer = peer.cert.id;
        e.type = type;
        e.channel = channel;
        e.data.assign(body.here(), body.here() + body.remaining());
        Push(std::move(e));
    }

    void Service::OnNodeState(bool up)
    {
        InEvent e;
        e.kind = InEvent::NodeState;
        e.flag = up;
        _nodeUp = up;
        Push(std::move(e));
    }

    void Service::OnLog(std::string const& line) { LOG_INFO("module", "[P2P] {}", line); }

    std::string Service::NameOf(Id128 const& peer)
    {
        // (for logs and command output: it never names; a character not met yet in this session shows its real name with the identifier)
        if (KnownCharacter* k = Characters::Get().Find(peer))
            return k->named ? k->localName : k->realName + " (" + Hex(peer).substr(0, 8) + ")";
        return "player " + Hex(peer).substr(0, 8);
    }

    namespace
    {
        std::string Fixed(double v, int decimals = 0) { return Acore::StringFormat("{:.{}f}", v, decimals); }
        std::string Seconds(int64_t ms) { return ms < 90000 ? std::to_string(ms / 1000) + " s" : std::to_string(ms / 60000) + " min"; }
    }

    // the summary: the nodes, how many players are in touch, how many of the connections are direct and how many go through a node
    std::string Service::Describe()
    {
        std::ostringstream out;
        std::vector<NodeFigures> nodes = NodeList();
        size_t up = 0;
        for (NodeFigures const& n : nodes)
            up += n.up;
        size_t touch = 0, linked = 0, direct = 0, other = 0;
        for (auto const& [id, p] : _peers)
        {
            touch += p.inTouch;
            linked += p.linked;
            direct += p.linked && p.direct;
            other += !p.compatible;
        }
        out << "Shared world: " << (_enabled ? "on" : "off") << ", realm " << _realm << (_scope.shard ? ", private shard" : "") << ". ";
        if (!_enabled)
            return out.str();
        out << "Relay nodes: " << up << " of " << nodes.size() << " reached. In touch with " << touch << " players; " << linked << " connections, "
            << direct << " direct and " << (linked - direct) << " through a node.";
        if (other)
            out << " " << other << " players have a different version.";
        return out.str();
    }

    // the players this one is in touch with, by the name this player knows them by, and how each is reached
    std::vector<std::string> Service::DescribePlayers()
    {
        std::vector<std::string> lines;
        for (auto const& [id, p] : _peers)
        {
            if (!p.compatible)
            {
                lines.push_back(Acore::StringFormat("{}: different version ({}), not met", NameOf(id), p.version));
                continue;
            }
            if (!p.inTouch)
                continue;
            std::string how = p.linked ? (p.direct ? "direct" : "through a node") : "known through a room";
            lines.push_back(Acore::StringFormat("{}: {}, map {} area {}", NameOf(id), how, p.presence.map, p.presence.area));
        }
        if (lines.empty())
            lines.push_back("nobody else is in touch");
        return lines;
    }

    // everything meant for a report: identifiers, rooms, the figures of every route, the reason a direct connection is not open
    std::vector<std::string> Service::DescribeDetails()
    {
        std::vector<std::string> lines;
        lines.push_back(Describe());
        lines.push_back(Acore::StringFormat("this server {} account {}, clock offset {} ms, {} characters met kept", Hex(_me.id), Hex(_account.signing.pub).substr(0, 16), _offset.load(), Characters::Get().Count()));
        for (NodeFigures const& n : NodeList())
            lines.push_back(Acore::StringFormat("node {}: {}", n.address, n.up ? "reached" : "not reached"));
        for (auto const& [id, p] : _peers)
        {
            LinkFigures f;
            bool have = Figures(id, f);
            lines.push_back(Acore::StringFormat("{} {} version {}{}{}", NameOf(id), Hex(id), p.version, p.compatible ? "" : " (different version)", p.inTouch ? ", in touch" : ""));
            if (!have)
                continue;
            lines.push_back(Acore::StringFormat("  inbox {}, route {}, up for {}, silent for {} ms, sent {} received {} frames", f.inbox, f.linked ? f.route : "-", Seconds(f.upForMs), f.silentMs, f.sent, f.received));
            if (!f.linked || !f.direct)
                lines.push_back(Acore::StringFormat("  direct connection: {}", DirectReasonName(f.directReason)));
            for (RouteFigures const& r : f.routes)
                lines.push_back(Acore::StringFormat("  route {}{}: {}, delay {} ms, loss {}% of {} probes", r.name, r.inUse ? " (in use)" : "", r.exists ? "exists" : "gone",
                    r.rtt, Fixed(r.loss * 100), r.probes));
            for (std::string const& room : f.rooms)
                lines.push_back("  room " + room);
        }
        return lines;
    }
}

// The page asks the server to stop in order when the game exits (web/acore.js, Acore_OnClientExit), and asks whether it has
extern "C"
{
#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_KEEPALIVE
#endif
    void acore_stop_server()
    {
        p2p::Service& service = p2p::Service::Get();
        service.RequestStop();
        // (the request is done by the world loop; while the server is still starting, for example waiting for the game's data files
        // (mod-wowcontroller, a patient read that ends on World::IsStopped), there is no loop yet and nothing to stop in order: stop now)
        if (!service.Updating())
            World::StopNow(SHUTDOWN_EXIT_CODE);
    }
#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_KEEPALIVE
#endif
    int acore_server_stopped() { return p2p::Service::Get().Stopped() ? 1 : 0; }
}
