/*
 * The shared world: the creatures (and objects) of the world database exist on every server, and each one is decided by one server at a time
 * (core/Authority.h). The deciding server runs the creature as usual and tells the others what it does; on the others it is a puppet
 * (Creature::SetPuppet): no AI, no movement of its own, no regeneration, no respawn. Outcomes (damage, healing, death, respawn) travel as events.
 *
 * Everything here runs on the world thread, where the maps are idle; the unit hooks, which run on map update threads, only queue work for it.
 */
#include "Mirrors.h"
#include "core/Authority.h"
#include "core/Clock.h"
#include "CreatureAI.h"
#include "CreatureGroups.h"
#include "WaypointMgr.h"
#include "CellImpl.h"
#include "Creature.h"
#include "GameObject.h"
#include "GameTime.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Map.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ThreatManager.h"
#include "Transport.h"
#include "TemporarySummon.h"
#include <atomic>
#include <cmath>
#include <mutex>
#include <set>

AC_GAME_API extern bool (*ClockDrivenMovement)(Creature const*);
AC_GAME_API extern bool (*PuppetEngage)(Creature*, Unit*);
AC_GAME_API extern bool (*SummonAsPuppet)();

namespace p2p
{
    Id128 PartyLeader(Player* local);   // Party.cpp
    Id128 InstanceDecider(Player* local);   // Instances.cpp
    Id128 CurrentPartyId(Player* local);    // Party.cpp

    namespace
    {
        constexpr float ScanRange = 160.0f;      // creatures this near the local player are looked after
        constexpr float PeerRange = 260.0f;      // and told to peers this near them
        constexpr float TakeoverMargin = 30.0f;  // an untouched creature passes to a closer player only when that one is nearer by this much
        constexpr int64_t FightHandMs = 4000;    // a creature in a fight passes to a closer player when it has been closer by the margin this long
        constexpr int64_t AskWaitMs = 1000;      // a server that has not heard who decides a creature asks, and waits this long for the answer

        enum StateFlags : uint8_t { Dead = 1, Moving = 2, Walking = 4, Takeover = 8, Silent = 16, Release = 32, OnTransport = 64, OnClock = 128 };   // OnClock: the creature is untouched and moves by the clock
        enum EventKind : uint8_t { EvDeath = 1, EvRespawn = 2, EvDamage = 3, EvHeal = 4, EvEngage = 5, EvDisengage = 6, EvPlayerHit = 7, EvObjectDown = 8, EvObjectUp = 9, EvAsk = 10, EvFace = 11, EvEngageRequest = 12, EvYield = 13, EvAura = 14, EvSummonGone = 15 };

        // every creature within a range (the core's own searches take one entry)
        struct AnyCreatureInRange
        {
            WorldObject const* center;
            float range;
            bool operator()(Unit* unit) const { return unit->IsCreature() && center->IsWithinDist(unit, range, false); }
        };

        float Dist(float ax, float ay, float bx, float by) { return std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by)); }
    }

    void LootKilled(EntityKey const& key, ObjectGuid guid, bool native, bool tapperHere);   // Loot.cpp
    void LootKilledLate(EntityKey const& key, ObjectGuid guid, Id128 const& tapper);          // Loot.cpp
    Id128 PartyLeader(Player* local);   // Party.cpp

    class World : public Feature
    {
    public:
        static World& Get()
        {
            static World world;
            return world;
        }

        void OnLogin(Player* player) override
        {
            _table = std::make_unique<AuthorityTable>(Service::Get().Me().id);
            _tapper.clear();
            _timer = _stateTimer = _listTimer = _clockTimer = 0;
            _driven.clear();
            _placed.clear();
            _offClock.clear();
            _offSent.clear();
            _lookSent.clear();
            _lastThreat.clear();
            _asked.clear();
            {
                std::lock_guard<std::mutex> lock(_sumMutex);
                _sumByGuid.clear();
                _sumByKey.clear();
            }
            _sumSig.clear();
            _sumHeard.clear();
            ::SummonAsPuppet = &World::MakingPuppet;
            {
                std::lock_guard<std::mutex> lock(_engageMutex);
                _lastEngageMs.clear();
                _recentHit.clear();
            }
            ::ClockDrivenMovement = &World::ClockHook;
            ::PuppetEngage = &World::EngageHook;
            _clockOn = true;
        }

        // The server stops in order: every untouched creature this server decides is handed over (released) to the nearest player who responds
        void OnStopping(Player* local) override
        {
            HandOverAll(local);
            ForgetMySummons(true);
        }

        void HandOverAll(Player* local)
        {
            if (!_table || !local->IsInWorld())
                return;
            int64_t now = Service::Get().NetNow();
            for (auto const& [key, guid] : _near)
            {
                Creature* c = local->GetMap()->GetCreature(guid);
                AuthorityEntry* e = c ? _table->Find(key) : nullptr;
                if (!c || !e || !_table->Mine(*e) || c->isDead())
                    continue;   // (a creature in a fight is handed over too, with its threat list)
                Id128 best{};
                float bestDistance = 1e9f;
                for (auto const& [id, p] : Service::Get().Peers())
                {
                    if (!p.linked || p.presence.map != c->GetMapId() || p.presence.instanceKey != Service::Get().Presence().instanceKey ||
                        (p.presence.phaseMask & c->GetPhaseMask()) == 0 || !Service::Get().Responsive(id))
                        continue;
                    float d = PeerDistance(p, *c);
                    if (d < bestDistance)
                    {
                        best = id;
                        bestDistance = d;
                    }
                }
                if (!(best == Id128{}))
                    SendState(*c, *e, Release, Id128{}, best);
            }
            (void)now;
        }

        void OnLogout(Player* player) override
        {
            ClockStop(player);
            ReleaseAll();
            _table.reset();
            _heard.clear();
            std::lock_guard<std::mutex> lock(_deferMutex);
            _deferred.clear();
        }

        // from the unit hooks (map threads): work for the world thread
        void Defer(std::function<void()> f)
        {
            std::lock_guard<std::mutex> lock(_deferMutex);
            if (_deferred.size() < 5000)
                _deferred.push_back(std::move(f));
        }

        bool Active() const { return _table != nullptr; }

        // test aids (.p2p test): the creature nearest to the player, and a hit on it as the player's own
        uint32 _testSpawn = 0;   // the test commands look at the creature of this spawn number (0: by entry or the nearest)
        uint32 _testEntry = 0;   // the test commands look at the creatures of this entry only (0: any)

        Creature* Nearest(Player* local)
        {
            if (_testSpawn)
                for (auto const& [key, guid] : _near)
                    if (key.spawn == _testSpawn)
                        if (Creature* c = local->GetMap()->GetCreature(guid))
                            return c;   // (pinned by spawn number: the same creature on every server)
            if (_testEntry)
            {
                // (a summon of the entry, this server's or a puppet, comes before the creatures of the world: the aids of the summon scenarios look at it)
                std::lock_guard<std::mutex> lock(_sumMutex);
                for (auto const& [guid, key] : _sumByGuid)
                    if (Creature* c = local->GetMap()->GetCreature(guid); c && c->GetEntry() == _testEntry)
                        return c;
            }
            Creature* best = nullptr;
            for (auto const& [key, guid] : _near)
                if (Creature* c = local->GetMap()->GetCreature(guid))
                {
                    if (_testEntry && c->GetEntry() != _testEntry)
                        continue;
                    // (with an entry set, the creature is the one of the lowest spawn number near the player, the same on every server whatever the wanderers do; else the nearest)
                    bool better = !best;
                    if (best)
                    {
                        if (_testEntry)
                            better = c->GetSpawnId() < best->GetSpawnId();
                        else
                            better = local->GetExactDist2d(c) < local->GetExactDist2d(best);
                    }
                    if (better)
                        best = c;
                }
            return best;
        }

        std::string TestNear(Player* local)
        {
            Creature* c = _table ? Nearest(local) : nullptr;
            if (!c)
                return "no shared creature near";
            AuthorityEntry* e = _table->Find(KeyOf(c));
            return Acore::StringFormat("creature {} spawn {} {:.0f} yards: health {}/{}{}{}, claim {}, generation {:x}, guid {:x}{}", c->GetEntry(), c->GetSpawnId(), local->GetExactDist2d(c),
                c->GetHealth(), c->GetMaxHealth(), c->isDead() ? ", dead" : "", c->IsPuppet() ? ", puppet" : ", decided here", e && !e->claim.Empty() ? Hex(e->claim.server).substr(0, 8) : "none",
                e ? e->generation : 0, c->GetGUID().GetRawValue(), c->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE) ? ", lootable" : "") + Acore::StringFormat(", npcflag {:x} unitflag {:x} display {}", c->GetNpcFlags(), c->GetUnitFlags(), c->GetDisplayId()) + Acore::StringFormat(", at {:.1f} {:.1f} {:.1f}, loot gold {} items {} quest {}", c->GetPositionX(), c->GetPositionY(), c->GetPositionZ(), c->loot.gold, c->loot.items.size(), c->loot.quest_items.size()) + [&]
            {
                std::string out;
                for (LootItem const& i : c->loot.items)
                    out += Acore::StringFormat(" [item {} allowed {} quest {} ffa {} blocked {} underthreshold {}]", i.itemid, i.allowedGUIDs.size(), i.needs_quest, i.freeforall, i.is_blocked, i.is_underthreshold);
                for (LootItem const& i : c->loot.quest_items)
                    out += Acore::StringFormat(" [questitem {} allowed {}]", i.itemid, i.allowedGUIDs.size());
                std::string auras;
                for (auto const& pair : c->GetAppliedAuras())
                    auras += Acore::StringFormat(" {}", pair.second->GetBase()->GetId());
                return out + (auras.empty() ? "" : " auras" + auras);
            }();
        }

        // a test aid: the nearest creature is claimed here although another server decides it (a double claim): 'earlier' forges a claim older than the other's, which wins
        std::string TestClaim(Player* local, bool earlier)
        {
            Creature* c = _table ? Nearest(local) : nullptr;
            if (!c || !c->IsAlive())
                return "no living shared creature near";
            EntityKey key = KeyOf(c);
            AuthorityEntry& e = _table->At(key);
            int64_t now = Service::Get().NetNow();
            int64_t theirs = e.claim.Empty() ? now : e.claim.time;
            e.claim = Claim{Service::Get().Me().id, earlier ? theirs - 1000 : now};
            e.situation = Situation::Negotiated;
            c->SetPuppet(false);
            _table->Engage(e, Service::Get().Me().id, now);
            SeedThreat(*c, e, local);
            SendState(*c, e, 0);
            return Acore::StringFormat("claimed creature {} here with a claim {}", key.spawn, earlier ? "older than the other's" : "of now");
        }

        // a test aid: the nearest mirror is hit as the local player's own hit (its owner's server decides whether the game allows it)
        std::string TestHitPlayer(Player* local, uint32 damage)
        {
            Player* best = nullptr;
            for (auto const& ref : local->GetMap()->GetPlayers())
                if (Player* p = ref.GetSource(); p && p->IsMirror() && (!best || local->GetExactDist2d(p) < local->GetExactDist2d(best)))
                    best = p;
            if (!best)
                return "no other player's character near";
            Unit::DealDamage(local, best, damage, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
            return Acore::StringFormat("hit {} for {}", best->GetName(), damage);
        }

        // a test aid: the nearest shared creature, if this server decides it, casts a spell at the nearest other player's character (a creature's spell on a mirror)
        // a test aid: the shared creature that this server decides plays a sound to the players within 100 yards (the radius sounds of scripts)
        std::string TestRadiusSound(Player* local, uint32 sound)
        {
            Creature* c = _table ? Nearest(local) : nullptr;
            if (!c || !c->IsAlive())
                return "no living shared creature near";
            if (c->IsPuppet())
                return Acore::StringFormat("creature {} is a puppet here", c->GetEntry());
            c->PlayRadiusSound(sound, 100.0f);
            return Acore::StringFormat("creature {} played sound {} within 100 yards", c->GetEntry(), sound);
        }

        // a test aid: the local player casts one spell twice at the nearest shared creature in the same moment, so that two casts of it are in flight together
        std::string TestDoubleCast(Player* local, uint32 spell)
        {
            Creature* c = _table ? Nearest(local) : nullptr;
            if (!c || !c->IsAlive())
                return "no living shared creature near";
            local->CastSpell(c, spell, true);
            local->CastSpell(c, spell, true);
            return Acore::StringFormat("cast spell {} twice at creature {}", spell, c->GetEntry());
        }

        std::string TestCreatureCast(Player* local, uint32 spell)
        {
            Creature* c = _table ? Nearest(local) : nullptr;
            if (!c || !c->IsAlive())
                return "no living shared creature near";
            if (c->IsPuppet())
                return Acore::StringFormat("creature {} is a puppet here", c->GetEntry());
            Player* best = nullptr;
            for (auto const& ref : local->GetMap()->GetPlayers())
                if (Player* p = ref.GetSource(); p && p->IsMirror() && (!best || c->GetExactDist2d(p) < c->GetExactDist2d(best)))
                    best = p;
            if (!best)
                return "no other player's character near";
            c->CastSpell(best, spell, true);
            return Acore::StringFormat("creature {} cast spell {} at {}", c->GetEntry(), spell, best->GetName());
        }

        // test aids for summons: a creature of an entry is summoned beside the player (it stays until it is dismissed), and the nearest summon, this server's or a puppet, is hit as the player's own hit
        std::string TestSummon(Player* local, uint32 entry)
        {
            TempSummon* s = local->SummonCreature(entry, local->GetPositionX() + 2.0f, local->GetPositionY() + 2.0f, local->GetPositionZ(), local->GetOrientation(), TEMPSUMMON_MANUAL_DESPAWN, 0);
            if (s)
                s->SetFaction(local->GetFaction());   // (as a pet or a guardian has its owner's faction: the creatures of the world are hostile to it)
            return s ? Acore::StringFormat("summoned creature {} (guid {:x})", entry, s->GetGUID().GetRawValue()) : std::string("summon failed");
        }

        std::string TestHitSummon(Player* local, uint32 damage)
        {
            Creature* best = nullptr;
            {
                std::lock_guard<std::mutex> lock(_sumMutex);
                for (auto const& [guid, key] : _sumByGuid)
                    if (Creature* c = local->GetMap()->GetCreature(guid); c && c->IsAlive() && (!best || local->GetExactDist2d(c) < local->GetExactDist2d(best)))
                        best = c;
            }
            if (!best)
                return "no shared summon near";
            Unit::DealDamage(local, best, damage, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
            return Acore::StringFormat("hit summon {} ({}) for {}: health now {}/{}", best->GetEntry(), best->IsPuppet() ? "puppet" : "decided here", damage, best->GetHealth(), best->GetMaxHealth());
        }

        // a test aid: the creature the aids look at (pinned by spawn), if this server decides it, hits the nearest puppet of another server's summon (the owner applies the hit)
        std::string TestCreatureHitSummon(Player* local, uint32 damage)
        {
            Creature* c = _table ? Nearest(local) : nullptr;
            if (!c || !c->IsAlive() || c->IsPuppet())
                return "no living shared creature decided here";
            Creature* best = nullptr;
            {
                std::lock_guard<std::mutex> lock(_sumMutex);
                for (auto const& [guid, key] : _sumByGuid)
                    if (Creature* s = local->GetMap()->GetCreature(guid); s && s->IsPuppet() && s->IsAlive() && (!best || local->GetExactDist2d(s) < local->GetExactDist2d(best)))
                        best = s;
            }
            if (!best)
                return "no puppet of a summon near";
            Unit::DealDamage(c, best, damage, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
            return Acore::StringFormat("creature {} hit the puppet of summon {} for {}", c->GetEntry(), best->GetEntry(), damage);
        }

        std::string TestHit(Player* local, uint32 damage)
        {
            Creature* c = _table ? Nearest(local) : nullptr;
            if (!c || !c->IsAlive())
                return "no living shared creature near";
            Unit::DealDamage(local, c, damage, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
            if (c->IsAlive())
            {
                c->GetThreatMgr().AddThreat(local, float(damage) + 1.0f);   // (a real blow adds threat; the aid's damage is dealt directly)
                c->SetInCombatWith(local);
            }
            return Acore::StringFormat("hit creature {} (spawn {}) for {}", c->GetEntry(), c->GetSpawnId(), damage);
        }

        // for .p2p status: how many creatures near the local player this server decides, and how many it only shows
        std::string Describe()
        {
            Player* local = Service::Get().Local();
            size_t mine = 0, puppets = 0, dead = 0;
            if (_table && local && local->IsInWorld())
                for (auto const& [key, guid] : _near)
                    if (Creature* c = local->GetMap()->GetCreature(guid))
                    {
                        (c->IsPuppet() ? puppets : mine)++;
                        dead += c->isDead() ? 1 : 0;
                    }
            return Acore::StringFormat("creatures near: {} decided here, {} puppets, {} dead ({} listed); {} entries in the authority table", mine, puppets, dead, _near.size(), _table ? _table->Size() : 0);
        }

        void LocalTalk(Player* local, ObjectGuid guid);

        std::string DescribeOne(Creature* c)
        {
            if (!_table)
                return "the shared world is not running";
            if (!Shareable(c))
                return "this creature is not shared with other players (a summon or a pet)";
            AuthorityEntry* e = _table->Find(KeyOf(c));
            std::string decider = !e || e->claim.Empty() ? "this server (nobody else has claimed it)" :
                _table->Mine(*e) ? "this server" : Service::Get().NameOf(e->claim.server) + "'s server";
            char const* situation = !e ? "untouched" : e->situation == Situation::Untouched ? "untouched" :
                e->situation == Situation::Negotiated ? "in a fight shared by several players" : "owned by one player";
            return Acore::StringFormat("creature {} (spawn {}): decided by {}; {}{}", c->GetEntry(), c->GetSpawnId(), decider, situation, c->isDead() ? "; dead" : "");
        }

        void SetTestEntry(uint32 entry) { _testEntry = entry; _testSpawn = 0; }
        void SetTestSpawn(uint32 spawn) { _testSpawn = spawn; }
        Creature* LootLookup(EntityKey const& key, Player* local) { return Lookup(key, local); }
        Id128 TapperOf(EntityKey const& key) const
        {
            auto it = _tapper.find(key);
            return it == _tapper.end() ? Id128{} : it->second;
        }
        EntityKey KeyOf(Creature const* c) const
        {
            if (c->IsSummon())
            {
                std::lock_guard<std::mutex> lock(_sumMutex);
                auto it = _sumByGuid.find(c->GetGUID());
                if (it != _sumByGuid.end())
                    return it->second;
            }
            EntityKey k;
            k.kind = 1;
            k.map = c->GetMapId();
            k.instance = Service::Get().Presence().instanceKey;
            k.spawn = static_cast<uint32_t>(c->GetSpawnId());
            return k;
        }

        bool DeciderOf(Creature const* c, Id128& server)
        {
            if (!_table || !Tracked(c))
                return false;
            AuthorityEntry* e = _table->Find(KeyOf(c));
            if (!e || e->claim.Empty())
                return false;
            server = e->claim.server;
            return true;
        }

        bool DecidesHere(Creature const* c)
        {
            if (!_table || !Tracked(c))
                return false;
            AuthorityEntry* e = _table->Find(KeyOf(c));
            return e && !e->claim.Empty() && _table->Mine(*e);
        }

        static bool Shareable(Creature const* c)
        {
            return c && c->GetSpawnId() && !c->IsSummon() && !c->IsPet() && !c->IsTotem() && !c->IsTrigger() && !c->IsVehicle() &&
                !c->GetCharmerOrOwnerGUID() && !c->IsHunterPet();
        }

        // a creature that other servers know under a key: one of the world database, or a summon this server or another one shares
        static bool Tracked(Creature const* c)
        {
            return Shareable(c) || (c && c->IsSummon() && Get().SummonKnown(c));
        }

        bool SummonKnown(Creature const* c) const
        {
            std::lock_guard<std::mutex> lock(_sumMutex);
            return _sumByGuid.count(c->GetGUID()) != 0;
        }

        static std::vector<std::pair<Id128, float>> ThreatOf(Creature& c)
        {
            std::vector<std::pair<Id128, float>> out;
            for (auto const* ref : c.GetThreatMgr().GetSortedThreatList())
            {
                if (out.size() >= 8)
                    break;
                Id128 id = NetIdOf(ref->GetVictim());
                if (!(id == Id128{}))
                    out.emplace_back(id, ref->GetThreat());
            }
            return out;
        }

        // a unit as the other servers know it: a player by its character, anything else is nobody here
        static Id128 NetIdOf(Unit const* unit)
        {
            if (!unit || !unit->IsPlayer())
                return Id128{};
            Service& s = Service::Get();
            if (Player* l = s.Local(); l && l == unit)
                return s.Me().id;
            return Mirrors::Get().PeerOf(unit->GetGUID());
        }

        static Unit* UnitOfNet(Id128 const& id)
        {
            if (id == Id128{})
                return nullptr;
            Service& s = Service::Get();
            if (id == s.Me().id)
                return s.Local();
            return Mirrors::Get().MirrorOf(id);
        }

        // ------------------------------------------------------------ movement by the clock
        // The data a creature on the clock moves by, from the core's own waypoint manager and the creature's template; never from a message. Nothing is "on the clock" for a
        // creature whose AI or script moves it, that is in a formation, flies, rides a transport or follows a path with events, smooth curves or spline points.
        struct ClockData
        {
            uint8_t kind = 0;                 // 0 not on the clock, 1 a waypoint path, 2 wandering
            std::vector<clk::Node> nodes;
            int64_t cycle = 0;
            clk::Wander wander;
            int64_t offset = 0;
        };

        static ClockData ComputeClock(Creature const* c)
        {
            ClockData d;
            if (!Shareable(c) || c->GetFormation() || c->GetTransport() || c->CanFly())
                return d;
            if (c->GetAIName() == "SmartAI" || !c->GetScriptName().empty())
                return d;   // (a script may move the creature at its own times)
            uint64_t identity = clk::Mix((uint64_t(c->GetMapId()) << 32) ^ c->GetSpawnId());
            if (c->GetDefaultMovementType() == WAYPOINT_MOTION_TYPE)
            {
                WaypointPath const* path = sWaypointMgr->GetPath(c->GetWaypointPath());
                if (!path || path->Nodes.size() < 2)
                    return d;
                for (WaypointNode const& n : path->Nodes)
                {
                    if (n.EventId || n.SmoothTransition || !n.SplinePoints.empty() || n.MoveType == WAYPOINT_MOVE_TYPE_LAND || n.MoveType == WAYPOINT_MOVE_TYPE_TAKEOFF)
                        return d;
                    clk::Node node;
                    node.p = {n.X, n.Y, n.Z};
                    node.delayMs = n.Delay;
                    node.speed = n.Velocity > 0 ? n.Velocity : c->GetSpeed(n.MoveType == WAYPOINT_MOVE_TYPE_WALK ? MOVE_WALK : MOVE_RUN);
                    d.nodes.push_back(node);
                }
                d.cycle = clk::PathCycleMs(d.nodes);
                if (d.cycle <= 0)
                    return ClockData();
                d.kind = 1;
                d.offset = clk::OffsetOf(identity, d.cycle);
            }
            else if (c->GetDefaultMovementType() == RANDOM_MOTION_TYPE && c->GetWanderDistance() >= 1.0f)
            {
                float x, y, z;
                c->GetRespawnPosition(x, y, z);
                d.wander = {{x, y, z}, c->GetWanderDistance(), c->GetSpeed(MOVE_WALK), identity};
                int64_t slot = clk::WanderSlotMs(d.wander);
                if (slot <= 0)
                    return d;
                d.kind = 2;
                d.offset = clk::OffsetOf(identity, slot * 64);
            }
            return d;
        }

        ClockData const* ClockOf(Creature const* c)
        {
            if (!c->GetSpawnId())
                return nullptr;
            std::lock_guard<std::mutex> lock(_clockMutex);
            auto it = _clock.find(c->GetSpawnId());
            if (it == _clock.end())
                it = _clock.emplace(c->GetSpawnId(), ComputeClock(c)).first;
            return it->second.kind ? &it->second : nullptr;
        }

        // the hook of the core, on a map thread: a creature on the clock gets an idle movement generator that does nothing, while the shared world runs
        static bool ClockHook(Creature const* c)
        {
            World& w = Get();
            return w._clockOn.load() && w.ClockOf(c) != nullptr;
        }

        // The core engages creatures by itself in several places (aggro range, a call for help, a formation): for a puppet, whose decider is another server, that
        // would be a second fight. The core asks this hook before a puppet attacks; for a player of this server the decider is asked to engage it, and the puppet stays out
        // of the fight. A puppet that the local player has just hit is let be: the hit claims the creature for this server.
        static bool EngageHook(Creature* c, Unit* victim)
        {
            World& w = Get();
            if (!w._clockOn.load() || !w._table || !Shareable(c) || !c->IsPuppet())
                return false;
            Player* player = victim->GetCharmerOrOwnerPlayerOrPlayerItself();
            Player* local = Service::Get().Local();
            if (!player || !local)
                return false;
            int64_t now = GameTime::GetGameTimeMS().count();
            {
                std::lock_guard<std::mutex> lock(w._engageMutex);
                auto hit = w._recentHit.find(c->GetGUID());
                if (hit != w._recentHit.end() && now - hit->second < 5000)
                    return false;
            }
            EntityKey key = w.KeyOf(c);
            AuthorityEntry* e = w._table->Find(key);
            if (!e || e->claim.Empty() || w._table->Mine(*e))
                return false;
            if (player == local)
            {
                bool ask = false;
                {
                    std::lock_guard<std::mutex> lock(w._engageMutex);
                    int64_t& last = w._lastEngageMs[key];
                    if (now - last >= 3000)
                    {
                        last = now;
                        ask = true;
                    }
                }
                if (ask)
                {
                    Id128 decider = e->claim.server;
                    Service::Get().Defer([key, decider]
                    {
                        Writer w2;
                        w2.U8(EvEngageRequest);
                        key.Write(w2);
                        Service::Get().Say(decider, game::MsgEvent, w2.data);
                    });
                }
            }
            return true;
        }

        clk::Where ClockWhere(ClockData const& d, int64_t time)
        {
            return d.kind == 1 ? clk::PathAt(d.nodes, d.cycle, time, d.offset) : clk::WanderAt(d.wander, time, d.offset);
        }

        // Is the creature on the clock at this moment: untouched, alive, not evading, its decider (if another server) has said so
        bool OnClockNow(Creature const* c)
        {
            if (!ClockOf(c) || !c->IsAlive() || c->IsInCombat() || c->IsInEvadeMode() || c->GetVictim())
                return false;
            EntityKey key = KeyOf(c);
            if (_offClock.count(key))
                return false;
            AuthorityEntry* e = _table ? _table->Find(key) : nullptr;
            return !e || e->situation == Situation::Untouched;
        }

        void ClockStart(Creature* c)
        {
            if (!_driven.insert(c->GetGUID()).second)
                return;
            c->GetMotionMaster()->InitDefault();   // (the hook makes it the idle one)
        }

        // every second: each creature on the clock is moved toward where the clock will have it a second from now (speed = distance, so that it walks at the path's
        // pace when on schedule and catches up when behind); one far from its place is put there
        void DriveClock(Player* local)
        {
            int64_t now = Service::Get().NetNow();
            for (auto it = _driven.begin(); it != _driven.end();)
            {
                Creature* c = local->GetMap()->GetCreature(*it);
                if (!c || !c->IsInWorld())
                {
                    it = _driven.erase(it);
                    continue;
                }
                ++it;
                ClockData const* d = ClockOf(c);
                if (!d)
                    continue;
                bool on = OnClockNow(c);
                EntityKey key = KeyOf(c);
                if (!on)
                {
                    // (it left the clock: whoever decides it moves it; when it is untouched again it rejoins, where the clock says)
                    continue;
                }
                clk::Where w0 = ClockWhere(*d, now), w1 = ClockWhere(*d, now + 1000);
                if (!w0.valid || !w1.valid)
                    continue;
                float gz = w1.at.z;
                if (d->kind == 2)
                {
                    float h = c->GetMap()->GetHeight(c->GetPhaseMask(), w1.at.x, w1.at.y, w1.at.z + 2.0f, true);
                    if (h > INVALID_HEIGHT)
                        gz = h;
                }
                float dist0 = Dist(c->GetPositionX(), c->GetPositionY(), w0.at.x, w0.at.y);
                if (dist0 > 25.0f || (!_placed.count(c->GetGUID()) && dist0 > 2.0f))
                {
                    float z0 = d->kind == 2 ? c->GetMap()->GetHeight(c->GetPhaseMask(), w0.at.x, w0.at.y, w0.at.z + 2.0f, true) : w0.at.z;
                    c->GetMap()->CreatureRelocation(c, w0.at.x, w0.at.y, z0 > INVALID_HEIGHT ? z0 : w0.at.z, c->GetOrientation());
                    c->StopMoving();
                }
                _placed.insert(c->GetGUID());
                float dx = w1.at.x - c->GetPositionX(), dy = w1.at.y - c->GetPositionY();
                float dist = std::sqrt(dx * dx + dy * dy);
                bool moving = c->isMoving() && !c->movespline->Finalized();
                if (dist < 0.4f)
                {
                    if (moving && !w1.moving)
                        c->StopMoving();
                    continue;
                }
                if (moving && (c->movespline->FinalDestination() - G3D::Vector3(w1.at.x, w1.at.y, gz)).length() < 0.6f)
                    continue;
                float speed = std::min(dist / 1.0f, c->GetSpeed(MOVE_RUN) * 1.6f);
                bool walking = std::abs(speed - c->GetSpeed(MOVE_WALK)) < std::abs(speed - c->GetSpeed(MOVE_RUN));
                if (walking != c->HasUnitMovementFlag(MOVEMENTFLAG_WALKING))
                    c->SetWalk(walking);
                Movement::MoveSplineInit init(c);
                init.MoveTo(w1.at.x, w1.at.y, gz, false);
                init.SetVelocity(speed);
                init.Launch();
                (void)key;
            }
        }

        // the creatures given back to the core's own movement when the shared world stops
        void ClockStop(Player* local)
        {
            _clockOn = false;
            if (local && local->IsInWorld())
                for (ObjectGuid const& g : _driven)
                    if (Creature* c = local->GetMap()->GetCreature(g))
                        c->GetMotionMaster()->InitDefault();
            _driven.clear();
            _placed.clear();
            _offClock.clear();
        }


        // ------------------------------------------------------------ summoned creatures
        // A creature that a server makes during play (a script's summon, a pet, a guardian, a totem) belongs for its whole life to that server, which tells the others its
        // state under a key made of its own identity and a counter. The others make a puppet of it as a summon of the summoner they know, and remove it when told, when the
        // server is lost or when it falls silent. Its fight goes through the paths of the creatures of the world database (events to its decider, hits to the owners of
        // mirrors); it is never claimed, taken over or handed over.
        static constexpr size_t MaxSummons = 64;
        static constexpr int64_t SummonSilenceMs = 10000;
        static inline thread_local bool _makingPuppet = false;
        static bool MakingPuppet() { return _makingPuppet; }

        struct SummonSig { bool moving = false, dead = false; int dx = 0, dy = 0; };

        static uint64_t HashOf(EntityKey const& key)
        {
            uint64_t h = (uint64_t(key.map) << 32) ^ key.spawn ^ key.instance;
            for (uint8_t b : key.owner)
                h = (h ^ b) * 0x100000001b3ull;
            return h;
        }

        // world thread, twice a second: the summons of this server are found, the ones that are gone are told, the puppets of silent servers are removed
        void ScanSummons(Player* local)
        {
            Service& svc = Service::Get();
            Id128 me = svc.Me().id;
            int64_t now = svc.NetNow();
            // 1. the summons this server shares: still here?
            std::vector<std::pair<EntityKey, ObjectGuid>> mine;
            {
                std::lock_guard<std::mutex> lock(_sumMutex);
                for (auto const& [key, guid] : _sumByKey)
                    if (key.owner == me)
                        mine.push_back({key, guid});
            }
            for (auto const& [key, guid] : mine)
            {
                Creature* c = key.map == local->GetMapId() && key.instance == svc.Presence().instanceKey ? local->GetMap()->GetCreature(guid) : nullptr;
                if (!c || !c->IsInWorld() || c->IsPuppet() || c->GetVehicleKit() || c->IsCharmed())
                {
                    ForgetSummon(key, true);
                    continue;
                }
            }
            // 2. new ones
            size_t count = 0;
            {
                std::lock_guard<std::mutex> lock(_sumMutex);
                for (auto const& [key, guid] : _sumByKey)
                    count += key.owner == me ? 1 : 0;
            }
            if (count < MaxSummons)
            {
                std::list<Creature*> list;
                AnyCreatureInRange check{local, ScanRange};
                Acore::CreatureListSearcher<AnyCreatureInRange> searcher(local, list, check);
                Cell::VisitObjects(local, searcher, ScanRange);
                for (Creature* c : list)
                {
                    if (count >= MaxSummons)
                        break;
                    if (!c->IsSummon() || c->IsPuppet() || !c->IsInWorld() || c->isDead() || c->GetVehicleKit() || c->IsVehicle() || c->IsCharmed() || SummonKnown(c))
                        continue;
                    // (what a mirror's player summoned does not exist here: mirrors cast nothing)
                    if (TempSummon* ts = c->ToTempSummon())
                        if (Unit* summoner = ts->GetSummonerUnit(); summoner && summoner->IsPlayer() && summoner != local)
                            continue;
                    EntityKey key;
                    key.kind = 3;
                    key.map = c->GetMapId();
                    key.instance = svc.Presence().instanceKey;
                    key.spawn = ((static_cast<uint32_t>(now / 1000) & 0xFFF) << 20) | (++_sumCounter & 0xFFFFF);
                    key.owner = me;
                    {
                        std::lock_guard<std::mutex> lock(_sumMutex);
                        _sumByGuid[c->GetGUID()] = key;
                        _sumByKey[key] = c->GetGUID();
                    }
                    AuthorityEntry& e = _table->At(key);
                    e.claim = Claim{me, now};
                    e.situation = Situation::Untouched;
                    ++count;
                    SendSummonState(*c, key);
                    LOG_INFO("module", "[P2P] Summon {} (entry {}) is shared as {}:{}", c->GetGUID().ToString(), c->GetEntry(), Hex(me).substr(0, 8), key.spawn);
                }
            }
            // 3. puppets of servers that fell silent
            std::vector<EntityKey> silent;
            for (auto const& [key, heard] : _sumHeard)
                if (!(key.owner == me) && now - heard > SummonSilenceMs)
                    silent.push_back(key);
            for (EntityKey const& key : silent)
                RemoveSummonPuppet(key, local);
        }

        // world thread, every update: a summon that began a new movement is told at once, the others once a second
        void SummonStates(Player* local, bool all)
        {
            Id128 me = Service::Get().Me().id;
            std::vector<std::pair<EntityKey, ObjectGuid>> mine;
            {
                std::lock_guard<std::mutex> lock(_sumMutex);
                for (auto const& [key, guid] : _sumByKey)
                    if (key.owner == me)
                        mine.push_back({key, guid});
            }
            for (auto const& [key, guid] : mine)
            {
                Creature* c = local->GetMap()->GetCreature(guid);
                if (!c || !c->IsInWorld())
                    continue;
                SummonSig sig;
                sig.dead = c->isDead();
                sig.moving = c->isMoving() && !c->movespline->Finalized();
                if (sig.moving)
                {
                    G3D::Vector3 d = c->movespline->FinalDestination();
                    sig.dx = static_cast<int>(d.x * 2.0f);
                    sig.dy = static_cast<int>(d.y * 2.0f);
                }
                SummonSig& last = _sumSig[key];
                bool changed = last.moving != sig.moving || last.dead != sig.dead || last.dx != sig.dx || last.dy != sig.dy;
                last = sig;
                if (all || changed)
                    SendSummonState(*c, key);
            }
        }

        void SendSummonState(Creature& c, EntityKey const& key)
        {
            Writer w;
            key.Write(w);
            bool moving = c.isMoving() && !c.movespline->Finalized();
            float splineSpeed = moving ? c.movespline->Velocity() : 0.0f;
            bool walking = moving && std::abs(splineSpeed - c.GetSpeed(MOVE_WALK)) < std::abs(splineSpeed - c.GetSpeed(MOVE_RUN));
            uint8_t flags = (c.isDead() ? Dead : 0) | (moving ? Moving : 0) | (walking ? Walking : 0);
            w.U8(flags).U32(c.GetEntry()).U8(static_cast<uint8_t>(c.GetLevel())).Str(c.GetName());
            // the summoner as the others know it: a player, a creature they know, or nobody
            ObjectGuid sg;
            if (TempSummon* ts = c.ToTempSummon())
                sg = ts->GetSummonerGUID();
            if (sg.IsEmpty())
                sg = c.GetOwnerGUID();
            Unit* summoner = sg.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(c, sg);
            Id128 summonerId = summoner ? NetIdOf(summoner) : Id128{};
            if (!(summonerId == Id128{}))
                w.U8(1).Id(summonerId);
            else if (Creature* sc = summoner ? summoner->ToCreature() : nullptr; sc && Tracked(sc))
            {
                w.U8(2);
                KeyOf(sc).Write(w);
            }
            else
                w.U8(0);
            w.U32(c.GetHealth()).U32(c.GetMaxHealth()).F32(c.GetPositionX()).F32(c.GetPositionY()).F32(c.GetPositionZ()).F32(c.GetOrientation());
            if (moving)
            {
                G3D::Vector3 d = c.movespline->FinalDestination();
                w.F32(d.x).F32(d.y).F32(d.z).F32(splineSpeed);
            }
            uint32_t phase = c.GetPhaseMask();
            w.U32(phase).U32(c.GetFaction());
            Unit* victim = c.GetVictim();
            w.Id(victim ? NetIdOf(victim) : Id128{});
            WriteLook(w, c);
            float x = c.GetPositionX(), y = c.GetPositionY();
            uint32_t map = key.map;
            uint64_t instance = key.instance;
            Service::Get().SayAllKeyed([=](PeerInfo const& p)
            {
                return p.presence.map == map && p.presence.instanceKey == instance && (p.presence.phaseMask & phase) != 0 && Dist(p.presence.x, p.presence.y, x, y) < PeerRange;
            }, game::KeyEntityBase | (clk::Mix(HashOf(key)) >> 2), game::MsgSummon, w.data);
        }

        // a summon of this server left the world (or is no longer shared): the others remove their puppets
        void ForgetSummon(EntityKey const& key, bool tell)
        {
            {
                std::lock_guard<std::mutex> lock(_sumMutex);
                auto it = _sumByKey.find(key);
                if (it != _sumByKey.end())
                {
                    _sumByGuid.erase(it->second);
                    _sumByKey.erase(it);
                }
            }
            _sumSig.erase(key);
            _sumHeard.erase(key);
            _deadSeen.erase(key);
            // (a summon's entries live and die with it: a shaman's totems or a resummoned pet would fill the tables for the whole session)
            if (_table)
                _table->Forget(key);
            _tapper.erase(key);
            _rewardedLate.erase(key);
            if (tell)
            {
                Writer w;
                w.U8(EvSummonGone);
                key.Write(w);
                uint32_t map = key.map;
                uint64_t instance = key.instance;
                Service::Get().SayAll([=](PeerInfo const& p) { return p.presence.map == map && p.presence.instanceKey == instance; }, game::MsgEvent, w.data);
            }
        }

        void ForgetMySummons(bool tell)
        {
            Id128 me = Service::Get().Me().id;
            std::vector<EntityKey> keys;
            {
                std::lock_guard<std::mutex> lock(_sumMutex);
                for (auto const& [key, guid] : _sumByKey)
                    if (key.owner == me)
                        keys.push_back(key);
            }
            for (EntityKey const& key : keys)
                ForgetSummon(key, tell);
        }

        // a puppet of another server's summon: gone from the world and from the tables
        void RemoveSummonPuppet(EntityKey const& key, Player* local)
        {
            ObjectGuid guid;
            {
                std::lock_guard<std::mutex> lock(_sumMutex);
                auto it = _sumByKey.find(key);
                if (it == _sumByKey.end())
                    return;
                guid = it->second;
            }
            if (Creature* c = local->GetMap()->GetCreature(guid))
            {
                LOG_INFO("module", "[P2P] The puppet of summon {}:{} (entry {}) is removed", Hex(key.owner).substr(0, 8), key.spawn, c->GetEntry());
                ApplyingRemote(true);
                if (TempSummon* ts = c->ToTempSummon())
                    ts->UnSummon();
                else
                    c->DespawnOrUnsummon();
                ApplyingRemote(false);
            }
            ForgetSummon(key, false);
        }

        void RemoveSummonsOf(Id128 const& owner, Player* local)
        {
            std::vector<EntityKey> keys;
            {
                std::lock_guard<std::mutex> lock(_sumMutex);
                for (auto const& [key, guid] : _sumByKey)
                    if (key.owner == owner)
                        keys.push_back(key);
            }
            for (EntityKey const& key : keys)
                RemoveSummonPuppet(key, local);
        }

        void OnSummonState(InEvent const& ev, Player* local)
        {
            Reader r(ev.data);
            EntityKey key = EntityKey::Read(r);
            uint8_t flags = r.U8();
            uint32_t entryId = r.U32();
            uint8_t level = r.U8();
            std::string name = r.Str();
            uint8_t summonerKind = r.U8();
            Id128 summonerId{};
            EntityKey summonerKey;
            if (summonerKind == 1)
                summonerId = r.Id();
            else if (summonerKind == 2)
                summonerKey = EntityKey::Read(r);
            uint32_t health = r.U32(), maxHealth = r.U32();
            float x = r.F32(), y = r.F32(), z = r.F32(), o = r.F32();
            float dx = 0, dy = 0, dz = 0, speed = 0;
            if (flags & Moving)
            {
                dx = r.F32(); dy = r.F32(); dz = r.F32(); speed = r.F32();
            }
            uint32_t phase = r.U32(), faction = r.U32();
            Id128 targetId = r.Id();
            Look look = ReadLook(r);
            if (!r.ok() || key.kind != 3 || !(key.owner == ev.peer) || key.map != local->GetMapId() || key.instance != Service::Get().Presence().instanceKey)
                return;
            PeerInfo* peer = Service::Get().FindPeer(ev.peer);
            if (!peer || Dist(peer->presence.x, peer->presence.y, x, y) > 600.0f)
                return;
            int64_t now = Service::Get().NetNow();
            Creature* c = Lookup(key, local);
            if (!c)
            {
                if (flags & Dead)
                    return;
                if (!sObjectMgr->GetCreatureTemplate(entryId))
                    return;
                size_t theirs = 0;   // (counted per owner: one server cannot fill the room of the others)
                {
                    std::lock_guard<std::mutex> lock(_sumMutex);
                    for (auto const& [k, g] : _sumByKey)
                        theirs += k.owner == key.owner ? 1 : 0;
                }
                if (theirs >= MaxSummons)
                    return;
                WorldObject* summoner = nullptr;
                if (summonerKind == 1)
                    summoner = UnitOfNet(summonerId);
                else if (summonerKind == 2)
                    summoner = Lookup(summonerKey, local);
                Position pos(x, y, z, o);
                ApplyingRemote(true);
                _makingPuppet = true;
                TempSummon* s = local->GetMap()->SummonCreature(entryId, pos, nullptr, 0, summoner);
                _makingPuppet = false;
                ApplyingRemote(false);
                if (!s)
                    return;
                // (not removed at its death: the corpse stays as long as the owner's, which tells when it is gone; without a duration the core makes a summon despawn at death)
                s->SetTempSummonType(TEMPSUMMON_MANUAL_DESPAWN);
                c = s;
                {
                    std::lock_guard<std::mutex> lock(_sumMutex);
                    _sumByGuid[c->GetGUID()] = key;
                    _sumByKey[key] = c->GetGUID();
                }
                AuthorityEntry& e = _table->At(key);
                e.claim = Claim{key.owner, now};
                e.situation = Situation::Untouched;
                if (level)
                    c->SetLevel(level);
                LOG_INFO("module", "[P2P] Summon of {}'s server (entry {}) is shown here as a puppet", Service::Get().NameOf(ev.peer), entryId);
            }
            _sumHeard[key] = now;
            if (!c->IsInWorld())
                return;
            ApplyingRemote(true);
            c->SetPuppet(true);
            if (faction && c->GetFaction() != faction)
                c->SetFaction(faction);
            if (!(flags & Dead))
                c->SetTarget(UnitOfNet(targetId) ? UnitOfNet(targetId)->GetGUID() : ObjectGuid::Empty);
            ApplyLook(c, look);
            if (c->GetMaxHealth() != maxHealth && maxHealth)
                c->SetMaxHealth(maxHealth);
            if ((flags & Dead) && !c->isDead())
            {
                int64_t seen = _deadSeen.emplace(key, now).first->second;
                if (now - seen >= 2500)
                    Kill(*c, nullptr);
            }
            else if (!(flags & Dead) && c->IsAlive())
            {
                _deadSeen.erase(key);
                if (health && c->GetHealth() != health)
                    c->SetHealth(health);
                bool alreadyThere = (flags & Moving) && c->isMoving() && !c->movespline->Finalized() &&
                    (c->movespline->FinalDestination() - G3D::Vector3(dx, dy, dz)).length() < 0.6f;
                if ((flags & Moving) && !alreadyThere)
                {
                    if (bool(flags & Walking) != c->HasUnitMovementFlag(MOVEMENTFLAG_WALKING))
                        c->SetWalk(flags & Walking);
                    Movement::MoveSplineInit init(c);
                    init.MoveTo(dx, dy, dz, false);
                    init.SetVelocity(speed > 0 ? speed : c->GetSpeed(MOVE_WALK));
                    init.Launch();
                }
                else if (!(flags & Moving) && !c->isMoving() && c->GetExactDist2d(x, y) > 1.0f)
                    c->GetMap()->CreatureRelocation(c, x, y, z, o);
                if (c->GetExactDist2d(x, y) > 12.0f)
                    c->GetMap()->CreatureRelocation(c, x, y, z, o);
            }
            if (phase && c->GetPhaseMask() != phase)
                c->SetPhaseMask(phase, true);
            ApplyingRemote(false);
            // (a pet has a name of its player's choosing; the game asks for it with its own pet name query, which reads the creature's name)
            if (!name.empty() && c->GetName() != name)
                c->SetName(name);
        }

        // ------------------------------------------------------------ the world thread
        void Update(uint32_t diff, Player* local) override
        {
            if (!_table || !local->IsInWorld())
                return;
            std::vector<std::function<void()>> deferred;
            {
                std::lock_guard<std::mutex> lock(_deferMutex);
                deferred.swap(_deferred);
            }
            for (auto& f : deferred)
                f();
            _timer += diff;
            _stateTimer += diff;
            _listTimer += diff;
            _clockTimer += diff;
            _summonTimer += diff;
            if (_timer >= 500)
            {
                _timer = 0;
                Scan(local);
                HeardAgain();
                ScanSummons(local);
            }
            if (_summonTimer >= 100)
            {
                _summonTimer = 0;
                _summonSecond += 100;
                bool all = _summonSecond >= 1000;
                if (all)
                    _summonSecond = 0;
                SummonStates(local, all);
            }
            if (_clockTimer >= 1000)
            {
                _clockTimer = 0;
                DriveClock(local);
            }
            if (_listTimer >= 10000)
            {
                _listTimer = 0;
                SendLists(local);
            }
            if (_stateTimer >= 1000)
            {
                _stateTimer = 0;
                Broadcast(local);
            }
            RespawnDue(local);
        }

        // The states of the creatures this server decides near a peer, to that peer: when its link comes up and when it is heard again after a silence, so that the two
        // servers find out at once whether they hold two states of one creature.
        void SendMineTo(PeerInfo const& p, Id128 const& id)
        {
            Player* local = Service::Get().Local();
            Id128 me = Service::Get().Me().id;
            for (auto const& [key, guid] : _near)
            {
                AuthorityEntry* e = _table->Find(key);
                Creature* c = e && !e->claim.Empty() && e->claim.server == me ? local->GetMap()->GetCreature(guid) : nullptr;
                if (c && PeerDistance(p, *c) < PeerRange && p.presence.map == key.map && p.presence.instanceKey == key.instance)
                    SendState(*c, *e, 0, Id128{}, id);
            }
        }

        void HeardAgain()
        {
            for (auto const& [id, p] : Service::Get().Peers())
            {
                bool up = p.linked && Service::Get().Responsive(id);
                bool& was = _heard[id];
                if (up && !was)
                    SendMineTo(p, id);
                was = up;
            }
        }

        // the creatures near the local player: who decides each?
        void Scan(Player* local)
        {
            int64_t now = Service::Get().NetNow();
            std::list<Creature*> list;
            AnyCreatureInRange check{local, ScanRange};
            Acore::CreatureListSearcher<AnyCreatureInRange> searcher(local, list, check);
            Cell::VisitObjects(local, searcher, ScanRange);
            _near.clear();
            if (++_scans % 40 == 1)
                LOG_INFO("module", "[P2P] World scan: {} creatures within {} yards of {}", list.size(), ScanRange, local->GetName());
            for (Creature* c : list)
            {
                if (!Shareable(c))
                    continue;
                EntityKey key = KeyOf(c);
                _near[key] = c->GetGUID();
                if (ClockOf(c))
                    ClockStart(c);
                AuthorityEntry& e = _table->At(key);
                bool peersAround = PeersNear(*c);
                if (!peersAround)
                {
                    // alone with it: the creature is ours, as before
                    if (c->IsPuppet() && (e.claim.Empty() || _table->Mine(e) || !PeerLinked(e.claim.server)))
                    {
                        e.claim = Claim{};
                        c->SetPuppet(false);
                    }
                    _asked.erase(key);
                    continue;
                }
                if (e.claim.Empty())
                {
                    // Ownership is asked, not guessed: a server that has heard no claim for a creature asks those around first, and takes it only if
                    // nobody answers within a second and its player is the closest. Until then the creature stands as it is.
                    int64_t& asked = _asked[key];
                    c->SetPuppet(true);
                    if (!asked)
                    {
                        asked = now;
                        Event(EvAsk, key, {}, false);
                        continue;
                    }
                    if (now - asked < AskWaitMs)
                        continue;
                    if (IAmClosest(*c, local))
                    {
                        e.claim = Claim{Service::Get().Me().id, now};
                        e.situation = Situation::Untouched;
                        e.generation = e.generation ? e.generation : 0;
                        c->SetPuppet(false);
                        _asked.erase(key);
                    }
                }
                else if (!_table->Mine(e))
                {
                    _asked.erase(key);
                    if (!PeerLinked(e.claim.server))
                    {
                        // its decider is gone
                        _table->Leave(e, e.claim.server, now);
                        e.claim = Claim{};
                        continue;
                    }
                    if (!Service::Get().Responsive(e.claim.server) && now - e.lastHeard > 5000 && !c->isDead() && IAmClosest(*c, local))   // (a state heard from the decider lately: it is back, the takeover is not made again)
                    {
                        // its decider has stopped responding: it is treated as if it had disconnected. The closest of the others takes the creature, citing the
                        // silence, and goes on from the last state it received; a creature in a fight keeps fighting those who are known to attack it
                        Id128 previous = e.claim.server;
                        bool fight = e.situation != Situation::Untouched;
                        _table->Leave(e, previous, now);
                        e.claim = Claim{Service::Get().Me().id, now};
                        c->SetPuppet(false);
                        if (fight)
                            SeedThreat(*c, e, local);
                        // (once: the one line of the takeover)
                        LOG_INFO("module", "[P2P] Took over creature {} from a silent server{}{}", key.spawn, fight ? ", in a fight" : "", fight && c->IsInCombat() ? " (still fighting)" : "");
                        SendState(*c, e, Takeover | Silent, previous);
                        continue;
                    }
                    c->SetPuppet(true);
                }
                else
                {
                    c->SetPuppet(false);
                    // an untouched creature passes to a player who is much closer
                    if (e.situation == Situation::Untouched && !c->IsInCombat() && !c->isDead())
                        MaybeHandOver(*c, e, local, now, false);
                    else if (e.situation != Situation::Untouched && c->IsInCombat() && !c->isDead())
                        MaybeHandOver(*c, e, local, now, true);   // (a fight passes too, with its threat list)
                }
            }
        }

        // The new decider of a creature in a fight builds its threat list again from the attackers it knows: its own player and the mirrors of the
        // players seen to hit it (the participants of the fight). With none the creature is untouched and returns home.
        void SeedThreat(Creature& c, AuthorityEntry const& e, Player* local)
        {
            ApplyingRemote(true);
            std::set<Id128> done;
            // the threat list its last decider told, then the participants of the fight that are not on it
            auto last = _lastThreat.find(KeyOf(&c));
            if (last != _lastThreat.end())
                for (auto const& [id, amount] : last->second)
                {
                    Unit* u = UnitOfNet(id);
                    Player* p = u ? u->ToPlayer() : nullptr;
                    if (!p || !p->IsAlive() || p->GetMapId() != c.GetMapId())
                        continue;   // (an attacker with no counterpart here is left out, as an attacker out of reach is on a normal server)
                    c.GetThreatMgr().AddThreat(p, std::max(amount, 1.0f));
                    c.SetInCombatWith(p);
                    done.insert(id);
                }
            for (Id128 const& id : e.participants)
            {
                if (done.count(id))
                    continue;
                Player* p = id == Service::Get().Me().id ? local : Mirrors::Get().MirrorOf(id);
                if (!p || !p->IsAlive() || p->GetMapId() != c.GetMapId())
                    continue;
                c.GetThreatMgr().AddThreat(p, 1.0f);
                c.SetInCombatWith(p);
            }
            ApplyingRemote(false);
        }

        bool PeerLinked(Id128 const& id) const
        {
            if (id == Service::Get().Me().id)
                return true;
            PeerInfo* p = Service::Get().FindPeer(id);
            return p && p->linked;
        }

        bool PeersNear(Creature const& c) const
        {
            Player* local = Service::Get().Local();
            for (auto const& [id, p] : Service::Get().Peers())
                if (p.linked && p.presence.map == c.GetMapId() && p.presence.instanceKey == Service::Get().Presence().instanceKey &&
                    (p.presence.phaseMask & c.GetPhaseMask()) != 0 && Dist(p.presence.x, p.presence.y, c.GetPositionX(), c.GetPositionY()) < PeerRange)
                    return true;
            return false;
        }

        // the creatures that the core links into one group (a formation) have one decider: the one of the group's leader, so the distances that decide are the leader's
        // (their pull, assistance and evading are then the core's own, on one server)
        static Creature const& Anchor(Creature const& c)
        {
            if (CreatureGroup const* formation = c.GetFormation())
                if (Creature const* leader = formation->GetLeader(); leader && leader->IsInWorld() && leader->GetMap() == c.GetMap())
                    return *leader;
            return c;
        }

        float PeerDistance(PeerInfo const& p, Creature const& creature) const
        {
            Creature const& c = Anchor(creature);
            float x = p.presence.x, y = p.presence.y;
            if (Player* m = Mirrors::Get().MirrorOf(p.cert.id))
            {
                x = m->GetPositionX();
                y = m->GetPositionY();
            }
            return Dist(x, y, c.GetPositionX(), c.GetPositionY());
        }

        bool IAmClosest(Creature const& c, Player* local) const
        {
            if (c.GetMap()->Instanceable())
            {
                // the member whose server runs the instance (always one whose player is inside it) decides all of its creatures
                Id128 decider = InstanceDecider(local);
                return decider == Id128{} || decider == Service::Get().Me().id;
            }
            float mine = local->GetExactDist2d(&Anchor(c));
            Id128 me = Service::Get().Me().id;
            for (auto const& [id, p] : Service::Get().Peers())
            {
                if (!p.linked || p.presence.map != c.GetMapId() || p.presence.instanceKey != Service::Get().Presence().instanceKey)
                    continue;
                if ((p.presence.phaseMask & c.GetPhaseMask()) == 0 || !Service::Get().Responsive(id))
                    continue;   // (a player who has stopped responding decides nothing)
                float theirs = PeerDistance(p, c);
                if (theirs < mine || (theirs == mine && id < me))
                    return false;
            }
            return true;
        }

        // The decider of an untouched creature sees that a peer is closer by the margin: it releases the creature to it. The release holds the creature's
        // full state; the peer claims the creature citing the release, and the other servers accept the claim because the release came from the decider they knew.
        // For a creature in a fight the closer player must have been closer by the margin for FightHandMs, so that a moment's movement does not move a fight.
        void MaybeHandOver(Creature& c, AuthorityEntry& e, Player* local, int64_t now, bool fight)
        {
            if (local->GetMap()->Instanceable() || now - _releasedAt[KeyOf(&c)] < 5000)
                return;
            float mine = local->GetExactDist2d(&Anchor(c));
            Id128 best{};
            float bestDistance = mine;
            for (auto const& [id, p] : Service::Get().Peers())
            {
                if (!p.linked || p.presence.map != c.GetMapId() || p.presence.instanceKey != Service::Get().Presence().instanceKey || (p.presence.phaseMask & c.GetPhaseMask()) == 0 ||
                    !Service::Get().Responsive(id))
                    continue;
                float theirs = PeerDistance(p, c);
                if (AuthorityTable::GovernorShouldChange(mine, theirs, TakeoverMargin) && theirs < bestDistance)
                {
                    best = id;
                    bestDistance = theirs;
                }
            }
            EntityKey ckey = KeyOf(&c);
            if (best == Id128{})
            {
                _closerSince.erase(ckey);
                return;
            }
            if (fight)
            {
                auto it = _closerSince.find(ckey);
                if (it == _closerSince.end() || it->second.first != best)
                {
                    _closerSince[ckey] = {best, now};
                    return;
                }
                if (now - it->second.second < FightHandMs)
                    return;
                LOG_INFO("module", "[P2P] The fight of creature {} passes to {}'s server (its player has been closer for {} s)", ckey.spawn, Service::Get().NameOf(best), (now - it->second.second) / 1000);
                _closerSince.erase(ckey);
            }
            _releasedAt[ckey] = now;
            SendState(c, e, Release, Id128{}, best);
        }

        // ------------------------------------------------------------ the creatures each server decides
        // Every 10 seconds each server tells the servers whose players are near what it decides near them: the spawn, the life and a short state. A creature that a server has
        // not heard of is then not taken for one that nobody decides, and a server that lost track of one (dead against alive, another life) asks for its state.
        void SendLists(Player* local)
        {
            if (!_table)
                return;
            Id128 me = Service::Get().Me().id;
            for (auto const& [id, p] : Service::Get().Peers())
            {
                if (!p.linked || !Service::Get().Responsive(id) || p.presence.map != local->GetMapId() || p.presence.instanceKey != Service::Get().Presence().instanceKey)
                    continue;
                Writer entries;
                uint16_t count = 0;
                for (auto const& [key, guid] : _near)
                {
                    AuthorityEntry* e = _table->Find(key);
                    if (!e || e->claim.server != me)
                        continue;
                    Creature* c = local->GetMap()->GetCreature(guid);
                    if (!c || PeerDistance(p, *c) > PeerRange)
                        continue;
                    if (count >= 200)
                        break;
                    entries.U32(key.spawn).U64(e->generation).U8((c->isDead() ? 1 : 0) | (c->IsInCombat() ? 2 : 0));
                    ++count;
                }
                if (!count)
                    continue;
                Writer w;
                w.U16(count).Raw(entries.data);
                Service::Get().SayKeyed(id, game::KeyCreatureList, game::MsgCreatures, w.data);
            }
        }

        void OnCreatures(InEvent const& ev, Player* local)
        {
            PeerInfo* peer = Service::Get().FindPeer(ev.peer);
            if (!peer || peer->presence.map != local->GetMapId() || peer->presence.instanceKey != Service::Get().Presence().instanceKey)
                return;
            Reader r(ev.data);
            uint16_t count = r.U16();
            int64_t now = Service::Get().NetNow();
            for (uint16_t i = 0; i < count && i < 200 && r.ok(); ++i)
            {
                uint32_t spawn = r.U32();
                uint64_t generation = r.U64();
                uint8_t flags = r.U8();
                EntityKey key;
                key.kind = 1;
                key.map = peer->presence.map;
                key.instance = peer->presence.instanceKey;
                key.spawn = spawn;
                Creature* c = FindBySpawn(local->GetMap(), spawn);
                if (!c || !Shareable(c) || !c->IsInWorld())
                    continue;
                AuthorityEntry& e = _table->At(key);
                if (e.claim.Empty())
                {
                    // decided by that server: not taken for a creature nobody decides
                    e.claim = Claim{ev.peer, now};
                    e.situation = Situation::Untouched;
                    c->SetPuppet(true);
                    _asked.erase(key);
                }
                else if (e.claim.server != ev.peer)
                {
                    if (_table->Mine(e))
                        SendState(*c, e, 0, Id128{}, ev.peer);   // (two servers list one creature: the claim rule settles it from the states)
                    continue;
                }
                if ((generation != e.generation || bool(flags & 1) != c->isDead()) && now - _listAsked[key] > 10000)
                {
                    _listAsked[key] = now;
                    Writer w;
                    w.U8(EvAsk);
                    key.Write(w);
                    Service::Get().Say(ev.peer, game::MsgEvent, w.data);
                }
            }
        }

        // ------------------------------------------------------------ what goes out
        void Broadcast(Player* local)
        {
            Id128 me = Service::Get().Me().id;
            std::vector<std::pair<EntityKey, ObjectGuid>> mine;
            for (auto const& [key, guid] : _near)
            {
                AuthorityEntry* e = _table->Find(key);
                if (e && !e->claim.Empty() && e->claim.server == me)
                    mine.push_back({key, guid});
            }
            for (auto const& [key, guid] : mine)
            {
                Creature* c = local->GetMap()->GetCreature(guid);
                if (!c)
                    c = FindBySpawn(local->GetMap(), key.spawn);
                if (c)
                {
                    // the creature we decide came back to life by itself: that is a respawn the others must hear of
                    bool& wasDead = _wasDead[key];
                    if (wasDead && !c->isDead())
                    {
                        AuthorityEntry& e = _table->At(key);
                        e.generation = AuthorityTable::MakeGeneration(me, Service::Get().NetNow());
                        e.dead = false;
                        _tapper.erase(key);
                        Writer w;
                        w.U64(e.generation);
                        Event(EvRespawn, key, w.data);
                    }
                    wasDead = c->isDead();
                    // a creature on the clock moves the same everywhere and nobody tells where it is: only what differs from the clock is told.
                    // Its leaving the clock is told by the states that follow; its rejoining once, with the flag.
                    if (OnClockNow(c))
                    {
                        bool& offSent = _offSent[key];
                        // what it shows (auras, model, size, flags) is the decider's too: a change is told like a loss of health. The first sight only notes it
                        Writer look;
                        WriteLook(look, *c);
                        uint64_t digest = clk::Mix(std::hash<std::string>()(std::string(look.data.begin(), look.data.end())));
                        auto [seen, first] = _lookSent.emplace(key, digest);
                        bool lookChanged = !first && seen->second != digest;
                        seen->second = digest;
                        if (c->GetHealth() < c->GetMaxHealth())
                            offSent = true;   // (hurt: its health is told while it regenerates, and once more when it is whole)
                        else if (lookChanged)
                            offSent = false;
                        else if (!offSent)
                            continue;
                        else
                            offSent = false;
                        SendState(*c, _table->At(key), 0);
                        continue;
                    }
                    if (ClockOf(c))
                        _offSent[key] = true;
                    SendState(*c, _table->At(key), 0);
                }
            }
        }

        static Creature* FindBySpawn(Map* map, uint32_t spawn)
        {
            auto range = map->GetCreatureBySpawnIdStore().equal_range(spawn);
            Creature* best = nullptr;
            for (auto it = range.first; it != range.second; ++it)
                if (!best || it->second->IsAlive())
                    best = it->second;
            return best;
        }

        // The state of a creature this server decides, to the peers around it (or to one: 'only'). 'releasedBy' is the previous decider when this is the claim
        // of a server that took the creature over; a Release is the previous decider handing it to 'only'.
        // ------------------------------------------------------------ what a creature shows: the state carries it, a puppet is set to it
        struct Look
        {
            uint32_t display = 0, native = 0, mount = 0, unitFlags = 0, npcFlags = 0, dynFlags = 0, bytes1 = 0, emoteState = 0;
            uint8_t sheath = 0;
            float scale = 1.0f;
            std::vector<game::AuraRecord> auras;
        };

        // what is the viewer's or this server's own, not the decider's: whether a corpse can be looted or skinned and by whom, and whether the creature is in combat here
        static constexpr uint32_t LocalUnitFlags = UNIT_FLAG_IN_COMBAT | UNIT_FLAG_SKINNABLE;
        static constexpr uint32_t LocalDynFlags = UNIT_DYNFLAG_LOOTABLE | UNIT_DYNFLAG_TAPPED | UNIT_DYNFLAG_TAPPED_BY_PLAYER | UNIT_DYNFLAG_TAPPED_BY_ALL_THREAT_LIST;

        static void WriteLook(Writer& w, Creature& c)
        {
            w.U32(c.GetDisplayId()).U32(c.GetNativeDisplayId()).U32(c.GetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID));
            w.U32(c.GetUInt32Value(UNIT_FIELD_FLAGS) & ~LocalUnitFlags).U32(c.GetUInt32Value(UNIT_NPC_FLAGS)).U32(c.GetUInt32Value(UNIT_DYNAMIC_FLAGS) & ~LocalDynFlags);
            w.U32(c.GetUInt32Value(UNIT_FIELD_BYTES_1)).U32(c.GetUInt32Value(UNIT_NPC_EMOTESTATE)).U8(c.GetByteValue(UNIT_FIELD_BYTES_2, 0)).F32(c.GetObjectScale());
            std::vector<game::AuraRecord> auras;
            Mirrors::CollectAuras(&c, auras);
            w.U8(static_cast<uint8_t>(auras.size()));
            for (game::AuraRecord const& a : auras)
                w.U32(a.spell).I64(a.expiry).U32(static_cast<uint32_t>(a.maxDuration)).U8(a.stacks).U8(a.flags).Id(a.caster);
        }

        static Look ReadLook(Reader& r)
        {
            Look l;
            l.display = r.U32(); l.native = r.U32(); l.mount = r.U32();
            l.unitFlags = r.U32(); l.npcFlags = r.U32(); l.dynFlags = r.U32();
            l.bytes1 = r.U32(); l.emoteState = r.U32(); l.sheath = r.U8(); l.scale = r.F32();
            for (uint8_t n = r.U8(), i = 0; i < n && i < 64 && r.ok(); ++i)
            {
                game::AuraRecord a;
                a.spell = r.U32(); a.expiry = r.I64(); a.maxDuration = static_cast<int32_t>(r.U32()); a.stacks = r.U8(); a.flags = r.U8(); a.caster = r.Id();
                l.auras.push_back(a);
            }
            return l;
        }

        // a puppet is set to what its decider shows, changing only what differs
        void ApplyLook(Creature* c, Look const& l)
        {
            if (!c->IsAlive())
                return;
            if (l.display && c->GetDisplayId() != l.display)
                c->SetDisplayId(l.display);
            if (l.native && c->GetNativeDisplayId() != l.native)
                c->SetNativeDisplayId(l.native);
            if (c->GetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID) != l.mount)
                c->SetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID, l.mount);
            if (l.scale > 0.0f && std::abs(c->GetObjectScale() - l.scale) > 0.001f)
                c->SetObjectScale(l.scale);
            uint32_t flags = (l.unitFlags & ~LocalUnitFlags) | (c->GetUInt32Value(UNIT_FIELD_FLAGS) & LocalUnitFlags);
            if (c->GetUInt32Value(UNIT_FIELD_FLAGS) != flags)
                c->SetUInt32Value(UNIT_FIELD_FLAGS, flags);
            if (c->GetUInt32Value(UNIT_NPC_FLAGS) != l.npcFlags)
                c->SetUInt32Value(UNIT_NPC_FLAGS, l.npcFlags);
            uint32_t dyn = (l.dynFlags & ~LocalDynFlags) | (c->GetUInt32Value(UNIT_DYNAMIC_FLAGS) & LocalDynFlags);
            if (c->GetUInt32Value(UNIT_DYNAMIC_FLAGS) != dyn)
                c->SetUInt32Value(UNIT_DYNAMIC_FLAGS, dyn);
            if (c->GetUInt32Value(UNIT_FIELD_BYTES_1) != l.bytes1)
                c->SetUInt32Value(UNIT_FIELD_BYTES_1, l.bytes1);
            if (c->GetUInt32Value(UNIT_NPC_EMOTESTATE) != l.emoteState)
                c->SetUInt32Value(UNIT_NPC_EMOTESTATE, l.emoteState);
            if (c->GetByteValue(UNIT_FIELD_BYTES_2, 0) != l.sheath)
                c->SetByteValue(UNIT_FIELD_BYTES_2, 0, l.sheath);
            Mirrors::Get().SyncAuras(c, l.auras);
        }

        void SendState(Creature& c, AuthorityEntry& e, uint8_t extraFlags, Id128 const& releasedBy = Id128{}, Id128 const& only = Id128{})
        {
            Writer w;
            EntityKey key = KeyOf(&c);
            key.Write(w);
            uint8_t flags = extraFlags | (c.isDead() ? Dead : 0) | (OnClockNow(&c) ? OnClock : 0);
            // a creature on a transport (a boat, a zeppelin, an elevator) is described by its place on the transport, which is the same for every server; it does
            // not walk anywhere in the state (its path, if any, is the transport's)
            Transport* transport = c.GetTransport();
            if (transport)
                flags |= OnTransport;
            bool moving = !transport && c.isMoving() && !c.movespline->Finalized();
            // (walking or running is read from the speed of the spline: a waypoint path asks for the walk speed without setting the unit's walk flag)
            float splineSpeed = moving ? c.movespline->Velocity() : 0.0f;
            bool walking = moving && std::abs(splineSpeed - c.GetSpeed(MOVE_WALK)) < std::abs(splineSpeed - c.GetSpeed(MOVE_RUN));
            if (moving)
                flags |= Moving | (walking ? Walking : 0);
            w.Id(e.claim.server).I64(e.claim.time).U8(static_cast<uint8_t>(e.situation)).U8(flags).U64(e.generation);
            if (transport)
                w.U32(c.GetHealth()).U32(c.GetMaxHealth()).F32(c.GetTransOffsetX()).F32(c.GetTransOffsetY()).F32(c.GetTransOffsetZ()).F32(c.GetTransOffsetO());
            else
                w.U32(c.GetHealth()).U32(c.GetMaxHealth()).F32(c.GetPositionX()).F32(c.GetPositionY()).F32(c.GetPositionZ()).F32(c.GetOrientation());
            if (moving)
            {
                G3D::Vector3 d = c.movespline->FinalDestination();
                w.F32(d.x).F32(d.y).F32(d.z).F32(splineSpeed);
            }
            w.U32(c.GetPhaseMask());
            w.Id(releasedBy).U32(c.GetEntry());
            if (transport)
                w.U32(transport->GetEntry());
            // its faction (a script may turn it hostile), who it attacks, and who has hurt it most: a server that takes it over goes on from there
            w.U32(c.GetFaction());
            {
                Unit* victim = c.GetVictim();
                w.Id(victim ? NetIdOf(victim) : Id128{}).U8(victim ? 1 : 0);
                std::vector<std::pair<Id128, float>> threats;
                for (auto const* ref : c.GetThreatMgr().GetSortedThreatList())
                {
                    if (threats.size() >= 8)
                        break;
                    Id128 id = NetIdOf(ref->GetVictim());
                    if (!(id == Id128{}))
                        threats.emplace_back(id, ref->GetThreat());
                }
                w.U8(static_cast<uint8_t>(threats.size()));
                for (auto const& [id, amount] : threats)
                    w.Id(id).F32(amount);
                // the spell in progress (a release carries it: the new decider casts it again)
                Spell* casting = c.GetCurrentSpell(CURRENT_GENERIC_SPELL);
                w.U32(casting ? casting->GetSpellInfo()->Id : 0);
            }
            WriteLook(w, c);
            uint32_t phase = c.GetPhaseMask();
            float x = c.GetPositionX(), y = c.GetPositionY();
            // (the keyed channel: the newest state of each creature goes first and a lost one is never waited for)
            uint64_t channel = game::KeyEntityBase | (clk::Mix((uint64_t(key.map) << 32) ^ key.spawn ^ key.instance) >> 2);
            if (!(only == Id128{}))
            {
                Service::Get().SayKeyed(only, channel, game::MsgEntity, w.data);
                return;
            }
            Service::Get().SayAllKeyed([=](PeerInfo const& p)
            {
                return p.presence.map == key.map && p.presence.instanceKey == key.instance && (p.presence.phaseMask & phase) != 0 &&
                    Dist(p.presence.x, p.presence.y, x, y) < PeerRange;
            }, channel, game::MsgEntity, w.data);
        }

        void EventTo(EventKind kind, EntityKey const& key, Bytes const& extra, Id128 const& to)
        {
            Writer w;
            w.U8(kind);
            key.Write(w);
            w.Raw(extra);
            Service::Get().Say(to, game::MsgEvent, w.data);
        }

        void Event(EventKind kind, EntityKey const& key, Bytes const& extra, bool toAll = true)
        {
            Writer w;
            w.U8(kind);
            key.Write(w);
            w.Raw(extra);
            uint32_t map = key.map;
            uint64_t instance = key.instance;
            Service::Get().SayAll([=](PeerInfo const& p) { return p.presence.map == map && p.presence.instanceKey == instance; }, game::MsgEvent, w.data);
        }

        // ------------------------------------------------------------ what comes in
        void OnEvent(InEvent const& e, Player* local) override
        {
            if (!_table)
                return;
            if (e.kind == InEvent::LinkDown || e.kind == InEvent::Gone)
            {
                int64_t now = Service::Get().NetNow();
                if (local)
                    RemoveSummonsOf(e.peer, local);
                for (EntityKey const& key : _table->ServerGone(e.peer, now))
                    if (AuthorityEntry* entry = _table->Find(key))
                        if (entry->claim.Empty() && local)
                            if (Creature* c = Lookup(key, local))
                                c->SetPuppet(false);   // (nobody decides it any more: it is ours again, from where it stands)
                return;
            }
            if (e.kind != InEvent::Message || !local || !local->IsInWorld())
                return;
            if (e.type == game::MsgSummon)
                OnSummonState(e, local);
            else if (e.type == game::MsgEntity)
                OnState(e, local);
            else if (e.type == game::MsgEvent)
                OnWorldEvent(e, local);
            else if (e.type == game::MsgCreatures)
                OnCreatures(e, local);
        }

        Creature* Lookup(EntityKey const& key, Player* local)
        {
            if (key.map != local->GetMapId() || key.instance != Service::Get().Presence().instanceKey)
                return nullptr;
            if (key.kind == 3)
            {
                ObjectGuid guid;
                {
                    std::lock_guard<std::mutex> lock(_sumMutex);
                    auto it = _sumByKey.find(key);
                    if (it == _sumByKey.end())
                        return nullptr;
                    guid = it->second;
                }
                return local->GetMap()->GetCreature(guid);
            }
            return FindBySpawn(local->GetMap(), key.spawn);
        }

        void OnState(InEvent const& ev, Player* local)
        {
            Reader r(ev.data);
            EntityKey key = EntityKey::Read(r);
            Claim claim;
            claim.server = r.Id();
            claim.time = r.I64();
            Situation situation = static_cast<Situation>(r.U8());
            uint8_t flags = r.U8();
            uint64_t generation = r.U64();
            uint32_t health = r.U32(), maxHealth = r.U32();
            float x = r.F32(), y = r.F32(), z = r.F32(), o = r.F32();
            float dx = 0, dy = 0, dz = 0, speed = 0;
            if (flags & Moving)
            {
                dx = r.F32(); dy = r.F32(); dz = r.F32(); speed = r.F32();
            }
            r.U32();
            Id128 releasedBy = r.Id();
            uint32_t entryId = r.U32();
            uint32_t transportEntry = (flags & OnTransport) ? r.U32() : 0;
            uint32_t faction = r.U32();
            Id128 targetId = r.Id();
            r.U8();   // (attacking: the target is what it fights)
            std::vector<std::pair<Id128, float>> threats;
            for (uint8_t n = r.U8(), i = 0; i < n && i < 8 && r.ok(); ++i)
            {
                Id128 who = r.Id();
                float amount = r.F32();
                threats.emplace_back(who, amount);
            }
            uint32_t castSpell = r.U32();
            Look look = ReadLook(r);
            if (!r.ok() || claim.server != ev.peer)   // (a server speaks for the entities it claims, no others)
                return;
            Creature* c = Lookup(key, local);
            if (!c || !Shareable(c) || !c->IsInWorld())
                return;
            // a creature on a transport: its place is the offset on that transport, which this server has its own copy of (else the place is not applied);
            // 'x, y, z, o' become the world place the offset gives here, so that everything below works on the world place
            bool onBoat = false;
            float offX = x, offY = y, offZ = z, offO = o;
            if (flags & OnTransport)
            {
                Transport* transport = c->GetTransport();
                if (!transport || transport->GetEntry() != transportEntry)
                    return;   // (the creature is not on that transport here)
                onBoat = true;
                transport->CalculatePassengerPosition(x, y, z, &o);
            }
            PeerInfo* peer = Service::Get().FindPeer(ev.peer);
            if (!peer || (!onBoat && Dist(peer->presence.x, peer->presence.y, x, y) > 600.0f))
                return;   // not plausible: a server claims what is not near its player
            // the creature is put at that place: on a boat, by its offset (the transport moves it with itself)
            auto relocate = [&]
            {
                if (onBoat)
                    c->m_movementInfo.transport.pos.Relocate(offX, offY, offZ, offO);
                else
                    c->GetMap()->CreatureRelocation(c, x, y, z, o);
            };
            if (entryId && c->GetEntry() != entryId)
                LOG_ERROR("module", "[P2P] Creature spawn {} is entry {} here and {} on the server that decides it", key.spawn, c->GetEntry(), entryId);
            AuthorityEntry& entry = _table->At(key);
            entry.lastHeard = Service::Get().NetNow();   // (the speaker is heard: a silent-takeover waits five seconds more)
            if (flags & Release)
            {
                // the decider hands the creature to this server (only this one was told): it claims it, citing the release, and goes on from this state
                if (entry.claim.server != ev.peer)
                    return;
                entry.claim = Claim{Service::Get().Me().id, Service::Get().NetNow()};
                entry.generation = generation;
                entry.situation = situation;
                c->SetPuppet(false);
                if (situation != Situation::Untouched)
                {
                    // a creature in a fight: it goes on fighting those it was fighting, with the threat it had
                    _lastThreat[key] = threats;
                    SeedThreat(*c, entry, local);
                    LOG_INFO("module", "[P2P] Took over the fight of creature {} from {}'s release ({} attackers on its threat list)", key.spawn, Service::Get().NameOf(ev.peer), threats.size());
                    if (Unit* victim = UnitOfNet(targetId))
                        if (c->IsAlive() && c->AI())
                        {
                            c->AI()->AttackStart(victim);
                            if (castSpell && sSpellMgr->GetSpellInfo(castSpell))
                                c->CastSpell(victim, castSpell, false);   // (the cast the hand-over interrupted)
                        }
                }
                if (c->GetExactDist2d(x, y) > 1.0f)
                    relocate();
                if (c->GetHealth() != health && health)
                    c->SetHealth(health);
                if (flags & Moving)
                {
                    Movement::MoveSplineInit init(c);
                    init.MoveTo(dx, dy, dz, true);   // (the way on the ground from where it stands, as the game would walk it)
                    init.SetVelocity(speed > 0 ? speed : c->GetSpeed(MOVE_WALK));
                    init.Launch();
                }
                SendState(*c, entry, Takeover, ev.peer);
                return;
            }
            bool lost = false;
            bool mineBefore = _table->Mine(entry);
            // Two servers that hear each other again hold two states of one creature: a newer life wins; within one
            // life death wins; of two living states the claim rule picks the decider and the lower health is kept. A server that holds a conflicting state answers with its own.
            bool newerLife = false;
            if (!(flags & Takeover))
            {
                if (generation < entry.generation)
                {
                    if (mineBefore)
                        SendState(*c, entry, 0, Id128{}, ev.peer);
                    return;
                }
                newerLife = generation > entry.generation;
                if (!newerLife && (flags & Dead) && c->IsAlive())
                {
                    // killed on the other side: dead here too. The kill, its rewards and its loot stay with the side that killed it.
                    ApplyingRemote(true);
                    Unit::Kill(nullptr, c, false);
                    ApplyingRemote(false);
                    c->loot.clear();
                    c->RemoveDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
                    entry.dead = true;
                    return;
                }
                if (!newerLife && !(flags & Dead) && c->isDead())
                {
                    if (mineBefore)
                        SendState(*c, entry, 0, Id128{}, ev.peer);   // (dead here: that wins, the other side lets it die from this state)
                    else if (entry.claim.server != ev.peer)
                        return;
                }
            }
            if (flags & Takeover)
            {
                // a server took the creature over: accepted only if it cites the decider this server knew (a release from it, or its silence, which this
                // server sees too), and only for an untouched creature
                bool citesKnown = releasedBy == entry.claim.server && !(releasedBy == Id128{});
                bool silent = (flags & Silent) && !Service::Get().Responsive(releasedBy);
                // (a silent takeover is accepted for a creature in a fight too: the silent decider is as good as disconnected)
                if (!citesKnown || ((flags & Silent) && !silent) || (flags & Dead))
                {
                    if (mineBefore)
                        SendState(*c, entry, 0, Id128{}, ev.peer);   // (a takeover of what this server decides and answers: the other side yields to this state)
                    return;
                }
                lost = _table->Mine(entry);
                if (flags & Silent)
                    entry.participants.erase(std::remove(entry.participants.begin(), entry.participants.end(), releasedBy), entry.participants.end());
                entry.situation = situation;
                entry.claim = claim;
            }
            else if (newerLife)
            {
                lost = mineBefore;
                entry.claim = claim;
                entry.situation = situation;
            }
            else if (!_table->OnClaim(entry, claim, situation, lost))
            {
                if (mineBefore && claim.server != Service::Get().Me().id)
                {
                    // two living states, and this server keeps the creature: the lower health of the two is kept, and the answer is this server's state
                    if (c->IsAlive() && health && health < c->GetHealth())
                        c->SetHealth(health);
                    SendState(*c, entry, 0, Id128{}, ev.peer);
                }
                return;
            }
            if (lost)
            {
                LOG_DEBUG("module", "[P2P] Lost the creature {} to another server", key.spawn);
                if (c->IsAlive() && c->GetHealth() < health && situation != Situation::Untouched)
                    health = c->GetHealth();   // (the damage done on either side in a fight is not undone; the health of an untouched creature is its decider's: it regenerates)
                // Two simultaneous claims: the loser sends its last threat list to the winner, which merges it with its own and goes on from its own place
                if (claim.server != Service::Get().Me().id)
                {
                    std::vector<std::pair<Id128, float>> mine = ThreatOf(*c);
                    {
                        Writer yw;
                        yw.U8(EvYield);
                        key.Write(yw);
                        yw.U8(static_cast<uint8_t>(mine.size()));
                        for (auto const& [id, amount] : mine)
                            yw.Id(id).F32(amount);
                        Service::Get().Say(claim.server, game::MsgEvent, yw.data);
                        LOG_INFO("module", "[P2P] Lost the claim of creature {} to {}'s server: its {} attackers are told to the winner", key.spawn, Service::Get().NameOf(claim.server), mine.size());
                    }
                }
            }
            if (claim.server != Service::Get().Me().id)
            {
                c->SetPuppet(true);
                if (generation > entry.generation && !(flags & Dead) && c->isDead())
                    c->Respawn(true);
                entry.generation = generation;
                // what it follows and attacks, its faction, and the threat list a server that takes it over goes on from
                _lastThreat[key] = threats;
                if (faction && c->GetFaction() != faction)
                    c->SetFaction(faction);
                if (!(flags & Dead))
                    c->SetTarget(UnitOfNet(targetId) ? UnitOfNet(targetId)->GetGUID() : ObjectGuid::Empty);
                ApplyLook(c, look);
                // a creature that is on the clock for its decider is moved by the clock here too; one that is not is moved as its decider moves it
                bool clockCreature = ClockOf(c) != nullptr;
                if (clockCreature)
                {
                    if (flags & OnClock)
                        _offClock.erase(key);
                    else
                        _offClock[key] = Service::Get().NetNow();
                }
                bool onClock = clockCreature && (flags & OnClock) && !(flags & Dead);
                if (c->GetMaxHealth() != maxHealth && maxHealth)
                    c->SetMaxHealth(maxHealth);
                if ((flags & Dead) && !c->isDead())
                {
                    // the state says dead: the death event (with the tapper and the party, which the rewards need) is waited for a moment, so that the kill runs here as it did
                    // on the server that decides it; if it does not come, the state kills it (the event's late arrival gives the rewards, EvDeath)
                    int64_t seen = _deadSeen.emplace(key, Service::Get().NetNow()).first->second;
                    if (Service::Get().NetNow() - seen >= 2500)
                        Kill(*c, nullptr);
                }
                else if (!(flags & Dead) && c->IsAlive())
                {
                    _deadSeen.erase(key);
                    if (situation != Situation::Negotiated || std::abs(int(c->GetHealth()) - int(health)) > int(maxHealth / 10))
                        c->SetHealth(health);
                    if (onClock)
                        return;   // (its place and walk are the clock's)
                    bool alreadyThere = (flags & Moving) && c->isMoving() && !c->movespline->Finalized() &&
                        (c->movespline->FinalDestination() - G3D::Vector3(dx, dy, dz)).length() < 0.6f;
                    if ((flags & Moving) && !alreadyThere)
                    {
                        // (the walk flag must be on the unit itself: a spline's SetWalk only picks the speed, and the client chooses the walk or run animation from the unit's flag)
                        if (bool(flags & Walking) != c->HasUnitMovementFlag(MOVEMENTFLAG_WALKING))
                            c->SetWalk(flags & Walking);
                        Movement::MoveSplineInit init(c);
                        init.MoveTo(dx, dy, dz, false);
                        init.SetVelocity(speed > 0 ? speed : c->GetSpeed(MOVE_WALK));
                        init.Launch();
                    }
                    else if (!(flags & Moving) && !c->isMoving())
                    {
                        // standing: put it where its decider has it (and facing as it faces), changing nothing that already is right
                        if (c->GetExactDist2d(x, y) > 1.0f)
                            relocate();
                        else if (std::abs(Position::NormalizeOrientation(c->GetOrientation() - o)) > 0.2f && std::abs(Position::NormalizeOrientation(o - c->GetOrientation())) > 0.2f)
                            c->SetFacingTo(o);
                    }
                    if (c->GetExactDist2d(x, y) > 12.0f)
                        relocate();
                }
            }
        }

        void Kill(Creature& c, Unit* killer)
        {
            // a death applied from another server is not told again (only the deciding server announces the deaths of its creatures)
            bool announce = _table && _table->Mine(_table->At(KeyOf(&c)));
            if (!announce)
                ApplyingRemote(true);
            Unit::Kill(killer, &c, false);
            if (!announce)
                ApplyingRemote(false);
        }

        void OnWorldEvent(InEvent const& ev, Player* local)
        {
            Reader r(ev.data);
            EventKind kind = static_cast<EventKind>(r.U8());
            EntityKey key = EntityKey::Read(r);
            int64_t now = Service::Get().NetNow();
            if (kind == EvPlayerHit)
            {
                OnPlayerHit(ev, key, r, local);
                return;
            }
            if (kind == EvObjectDown || kind == EvObjectUp)
            {
                OnObjectEvent(kind, key, r, local);
                return;
            }
            if (kind == EvSummonGone)
            {
                if (key.kind == 3 && key.owner == ev.peer)
                    RemoveSummonPuppet(key, local);
                return;
            }
            if (kind == EvAsk || kind == EvFace)
            {
                // someone asks who decides a creature: the decider answers with its state; or asks it to face a player who talks to it
                Creature* creature = Lookup(key, local);
                AuthorityEntry* known = creature ? _table->Find(key) : nullptr;
                if (!creature || !known || !_table->Mine(*known))
                    return;
                if (kind == EvAsk)
                    SendState(*creature, *known, 0, Id128{}, ev.peer);
                else if (Player* talker = Mirrors::Get().MirrorOf(ev.peer); talker && !creature->IsInCombat() && creature->IsAlive())
                {
                    creature->StopMoving();
                    creature->SetFacingToObject(talker);
                }
                return;
            }
            Creature* c = Lookup(key, local);
            if (!c || !Tracked(c))
                return;
            AuthorityEntry& entry = _table->At(key);
            switch (kind)
            {
                case EvDeath:
                {
                    Id128 tapper = r.Id();
                    uint64_t generation = r.U64();
                    Id128 party = r.remaining() >= 16 ? r.Id() : Id128{};
                    if (!r.ok())
                        break;
                    if (key.kind == 3)
                        LOG_INFO("module", "[P2P] Summon {} (entry {}) died; the tapper is {}{}", key.spawn, c->GetEntry(), Service::Get().NameOf(tapper), c->IsAlive() ? "" : " (the puppet was already dead here)");
                    if (!c->IsAlive())
                    {
                        // a state showed the creature dead first: no kill runs here, but the party's rewards are given as the kill would have given them (credit, experience,
                        // reputation by the core's rules for this server's player), and the leader's server still makes the party's loot
                        bool lateMember = tapper != Service::Get().Me().id && !(party == Id128{}) && CurrentPartyId(local) == party;
                        if (lateMember && local->IsAlive() && local->IsAtGroupRewardDistance(c) && !_rewardedLate.count(key))
                        {
                            _rewardedLate.insert(key);
                            Player* mirror = Mirrors::Get().MirrorOf(tapper);
                            Player* source = mirror && mirror->GetGroup() && mirror->GetGroup() == local->GetGroup() ? mirror : local;
                            LOG_INFO("module", "[P2P] The death of creature {} was seen as a state first: the party's rewards are given now", key.spawn);
                            ApplyingRemote(true);
                            source->RewardPlayerAndGroupAtKill(c, false);
                            ApplyingRemote(false);
                        }
                        if (lateMember && PartyLeader(local) == Service::Get().Me().id)
                            LootKilledLate(key, c->GetGUID(), tapper);
                        break;
                    }
                    _rewardedLate.erase(key);
                    _deadSeen.erase(key);
                    _tapper[key] = tapper;
                    entry.dead = true;
                    entry.generation = generation;
                    bool member = !(party == Id128{}) && CurrentPartyId(local) == party;
                    if (tapper == Service::Get().Me().id)
                    {
                        c->LowerPlayerDamageReq(c->GetMaxHealth(), true, local->GetLevel());   // (the damage was dealt on another server: the core wants some by a player here to reward the kill)
                        Unit::Kill(local, c, true);   // the killing blow was another server's; the corpse and its loot are ours until the leader's server sends the party's
                        LootKilled(key, c->GetGUID(), true, true);
                    }
                    else if (member && local->IsAlive() && local->IsAtGroupRewardDistance(c))
                    {
                        LOG_INFO("module", "[P2P] A member killed creature {}: the party's rewards are given to this server's player", key.spawn);
                        // a creature killed by a member of the party: this server rewards its own player by the core's rules (distance to the corpse, group,
                        // quest credit, experience, reputation), as if the party's kill were its own. The party's loot is made by the leader's server (Loot.cpp):
                        // if that is this one, the loot the core makes here is the party's; else this copy of the corpse has none until the record arrives.
                        bool leader = PartyLeader(local) == Service::Get().Me().id;
                        Player* mirror = Mirrors::Get().MirrorOf(tapper);
                        ApplyingRemote(true);
                        c->SetLootRecipient(leader || !mirror ? local : mirror);
                        c->LowerPlayerDamageReq(c->GetMaxHealth(), true, local->GetLevel());   // (the member's damage reached this server as health states, the core rewards a kill only after damage by a player here)
                        // the killer is the member's mirror when it is in the group here (each server processes the kill as that player's act, and the
                        // core's rules give this server's player its share); the mirror's own share is dropped by the hooks of Rules.cpp
                        Unit::Kill(mirror && mirror->GetGroup() && mirror->GetGroup() == local->GetGroup() ? static_cast<Unit*>(mirror) : static_cast<Unit*>(local), c, true);
                        ApplyingRemote(false);
                        if (!leader)
                        {
                            c->loot.clear();
                            c->RemoveDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
                        }
                        LootKilled(key, c->GetGUID(), leader, false);
                    }
                    else
                    {
                        Player* mirror = Mirrors::Get().MirrorOf(tapper);
                        c->SetLootRecipient(mirror);
                        Kill(*c, nullptr);
                        c->RemoveDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
                        c->loot.clear();
                        if (member)
                            LootKilled(key, c->GetGUID(), false, false);   // (a record of the party's loot may come)
                    }
                    break;
                }
                case EvRespawn:
                {
                    uint64_t generation = r.U64();
                    if (!r.ok())
                        break;
                    if (entry.generation != generation || c->isDead())
                    {
                        entry.generation = generation;
                        entry.dead = false;
                        _tapper.erase(key);
                        if (c->isDead())
                            c->Respawn(true);
                    }
                    break;
                }
                case EvAura:
                {
                    // an aura that a player of another server put on a creature this server decides: checked by the game's rules at this moment
                    Id128 who = r.Id();
                    uint32_t spell = r.U32();
                    int32 left = static_cast<int32>(r.U32()), max = static_cast<int32>(r.U32());
                    uint8_t stacks = r.U8(), beneficial = r.U8();
                    SpellInfo const* info = r.ok() ? sSpellMgr->GetSpellInfo(spell) : nullptr;
                    Player* caster = Mirrors::Get().MirrorOf(who);
                    if (!info || !caster || !_table->Mine(entry) || !c->IsAlive() || info->IsPassive() || left > 3600 * 1000 || max > 3600 * 1000 || caster->GetExactDist(c) > 120.0f)
                        break;
                    if (beneficial ? !caster->IsValidAssistTarget(c) : !caster->IsValidAttackTarget(c, info))
                    {
                        LOG_INFO("module", "[P2P] Refused aura {} of {} on creature {}: not allowed now", spell, caster->GetName(), key.spawn);
                        break;
                    }
                    ApplyingRemote(true);
                    if (Aura* aura = caster->AddAura(info, MAX_EFFECT_MASK, c))
                    {
                        if (left > 0)
                        {
                            aura->SetMaxDuration(max > 0 ? max : left);
                            aura->SetDuration(left);
                        }
                        if (stacks > 1)
                            aura->SetStackAmount(stacks);
                        if (!c->IsInCombat() || !c->GetVictim())
                            c->GetThreatMgr().AddThreat(caster, 0.0f);   // (a harmful aura puts its caster on the threat list, as a hit does)
                    }
                    ApplyingRemote(false);
                    break;
                }
                case EvDamage:
                case EvHeal:
                {
                    Id128 who = r.Id();
                    uint32_t amount = r.U32();
                    if (!r.ok() || !c->IsAlive() || amount > c->GetMaxHealth() * 2u)
                        break;
                    Player* source = who == Service::Get().Me().id ? nullptr : Mirrors::Get().MirrorOf(who);
                    ApplyingRemote(true);
                    if (kind == EvDamage)
                    {
                        if (!_tapper.count(key))
                            _tapper[key] = who;
                        if (_table->Mine(entry) && source)
                        {
                            // the decider: the attacker is on its threat list and the creature fights back
                            c->GetThreatMgr().AddThreat(source, float(amount));
                        }
                        if (c->GetHealth() > amount)
                            c->SetHealth(c->GetHealth() - amount);
                        else if (!c->isDead())
                        {
                            // (the death of a summon of this server is told by this server only, with the tapper recorded above; the hook is not muted for it)
                            bool ownSummon = key.kind == 3 && key.owner == Service::Get().Me().id;
                            if (ownSummon)
                                ApplyingRemote(false);
                            Kill(*c, source);
                            if (ownSummon)
                                ApplyingRemote(true);
                        }
                    }
                    else
                        c->SetHealth(std::min<uint32>(c->GetMaxHealth(), c->GetHealth() + amount));
                    ApplyingRemote(false);
                    break;
                }
                case EvEngage:
                    _table->Engage(entry, ev.peer, now);
                    break;
                case EvYield:
                {
                    // the loser of a double claim sends its threat list: the higher threat of each attacker is kept
                    uint8_t n = r.U8();
                    if (!_table->Mine(entry) || !c->IsAlive())
                        break;
                    size_t merged = 0;
                    ApplyingRemote(true);
                    for (uint8_t i = 0; i < n && r.ok(); ++i)
                    {
                        Id128 who = r.Id();
                        float theirs = r.F32();
                        Unit* u = UnitOfNet(who);
                        Player* p = u ? u->ToPlayer() : nullptr;
                        if (!p || !p->IsAlive())
                            continue;
                        float ours = c->GetThreatMgr().GetThreat(p);
                        if (theirs > ours)
                            c->GetThreatMgr().AddThreat(p, theirs - ours, nullptr, true, true);
                        c->SetInCombatWith(p);
                        ++merged;
                    }
                    ApplyingRemote(false);
                    LOG_INFO("module", "[P2P] Merged the threat of {} attackers of creature {} from the yielding server {}", merged, key.spawn, Service::Get().NameOf(ev.peer));
                    break;
                }
                case EvEngageRequest:
                {
                    // a server whose player a puppet of ours wants to attack asks us to engage it, as the core would on a normal server
                    Player* requester = Mirrors::Get().MirrorOf(ev.peer);
                    if (_table->Mine(entry) && requester && requester->IsAlive() && c->IsAlive() && !c->IsPuppet() && !c->IsInCombat() && c->AI())
                    {
                        _table->Engage(entry, ev.peer, now);
                        ApplyingRemote(true);
                        c->AI()->AttackStart(requester);
                        ApplyingRemote(false);
                    }
                    break;
                }
                case EvDisengage:
                    if (entry.claim.server == ev.peer)
                        entry.situation = Situation::Untouched;
                    entry.participants.erase(std::remove(entry.participants.begin(), entry.participants.end(), ev.peer), entry.participants.end());
                    break;
                default:
                    break;
            }
        }

        // a creature or a player of another server hurt one of ours: the victim's server checks that it is plausible and applies it
        void OnPlayerHit(InEvent const& ev, EntityKey const& attackerKey, Reader& r, Player* local)
        {
            Id128 victim = r.Id();
            uint32_t amount = r.U32();
            uint8_t fromPlayer = r.U8();
            EntityKey summonKey;
            if (fromPlayer == 2)
                summonKey = EntityKey::Read(r);   // the victim is a summon of this server
            if (!r.ok() || victim != Service::Get().Me().id || (fromPlayer != 2 && !local->IsAlive()))
                return;   // (a dead player's summons can still be hurt)
            if (fromPlayer == 2)
            {
                Creature* target = summonKey.kind == 3 && summonKey.owner == victim ? Lookup(summonKey, local) : nullptr;
                Creature* from = Lookup(attackerKey, local);
                if (!target || !from || target->IsPuppet() || !target->IsAlive() || target->GetExactDist2d(from) > 80.0f ||
                    !from->IsHostileTo(target) || target->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE))   // (the copy of the creature is a puppet here, whose own flags would refuse any attack: the factions decide)
                {
                    LOG_INFO("module", "[P2P] Refused a hit of {} on a summon of this server: {}", amount, !target ? "no such summon" : !from ? "the creature is not known here" : !target->IsAlive() ? "it is dead" :
                        target->GetExactDist2d(from) > 80.0f ? "too far" : Acore::StringFormat("not a valid target (factions {} and {}, hostile {}, unit flags {:x})", from->GetFaction(), target->GetFaction(), from->IsHostileTo(target), target->GetUnitFlags()).c_str());
                    return;
                }
                if (amount > std::max(1500.0f, from->GetMaxHealth() * 0.6f))
                    return;
                // (a lethal hit is not muted, so that the death of the summon is told by this server as any death of its summons)
                bool lethal = target->GetHealth() <= amount;
                if (!lethal)
                    ApplyingRemote(true);
                Unit::DealDamage(from, target, amount, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
                if (!lethal)
                    ApplyingRemote(false);
                LOG_INFO("module", "[P2P] Creature {} of another server hit this server's summon {} for {}: health now {}/{}", from->GetEntry(), target->GetEntry(), amount, target->GetHealth(), target->GetMaxHealth());
                return;
            }
            Unit* attacker = nullptr;
            if (fromPlayer)
                attacker = Mirrors::Get().MirrorOf(ev.peer);
            else if (Creature* c = Lookup(attackerKey, local))
                attacker = c;
            if (!attacker || attacker->GetExactDist2d(local) > 80.0f)
                return;
            // only what the game allows at this moment: damage that arrives after a duel ended, from a player that may not attack this one, is refused however
            // late the message comes
            if (!attacker->IsValidAttackTarget(local))
            {
                LOG_INFO("module", "[P2P] Refused a hit of {} from {}: it is not a valid target now", amount, attacker->GetName());
                return;
            }
            float bound = attacker->IsCreature() ? std::max(1500.0f, attacker->GetMaxHealth() * 0.6f) : std::max(2500.0f, float(local->GetMaxHealth()));
            if (amount > bound)
                return;
            ApplyingRemote(true);
            Unit::DealDamage(attacker, local, amount, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
            ApplyingRemote(false);
        }

        void OnObjectEvent(EventKind kind, EntityKey const& key, Reader& r, Player* local)
        {
            uint32_t seconds = r.U32();
            if (!r.ok() || key.map != local->GetMapId())
                return;
            auto range = local->GetMap()->GetGameObjectBySpawnIdStore().equal_range(key.spawn);
            for (auto it = range.first; it != range.second; ++it)
            {
                GameObject* go = it->second;
                ApplyingRemote(true);
                if (kind == EvObjectDown && go->isSpawned())
                {
                    {
                        // (the core's next update of this object ends in "ready, despawned", which the hook would send as an "up": it would bring the
                        // looted object back on the server that opened it. That one transition is ours, not a respawn.)
                        std::lock_guard<std::mutex> lock(_echoMutex);
                        _echo.insert(key.spawn);
                    }
                    go->SetLootState(GO_JUST_DEACTIVATED);
                    go->SetRespawnTime(seconds);
                }
                else if (kind == EvObjectUp && !go->isSpawned())
                    go->Respawn();
                ApplyingRemote(false);
            }
        }

        // a dead creature whose time has come is brought back by the server that decides it (or by the lowest server of those that know it)
        void RespawnDue(Player* local)
        {
            if (_respawnTimer++ % 10)
                return;
            int64_t now = Service::Get().NetNow();
            time_t gameNow = GameTime::GetGameTime().count();
            for (auto const& [key, guid] : _near)
            {
                Creature* c = local->GetMap()->GetCreature(guid);
                if (!c || !c->IsPuppet() || !c->isDead() || c->GetRespawnTime() > gameNow)
                    continue;
                AuthorityEntry* e = _table->Find(key);
                if (!e)
                    continue;
                // every server that sees it due may respawn it; the generation tells the others which respawn it was
                e->generation = AuthorityTable::MakeGeneration(Service::Get().Me().id, now);
                e->dead = false;
                _tapper.erase(key);
                c->SetPuppet(false);
                c->Respawn(true);
                e->claim = Claim{Service::Get().Me().id, now};
                e->situation = Situation::Untouched;
                Writer w;
                w.U64(e->generation);
                Event(EvRespawn, key, w.data);
            }
        }

        void ReleaseAll()
        {
            Player* local = Service::Get().Local();
            if (!local || !local->IsInWorld())
                return;
            for (auto const& [key, guid] : _near)
                if (Creature* c = local->GetMap()->GetCreature(guid))
                    c->SetPuppet(false);
            _near.clear();
        }

        // ------------------------------------------------------------ hooks (map threads)
        static thread_local bool _applying;
        static bool Applying() { return _applying; }
        static void ApplyingRemote(bool on) { _applying = on; }

        // A local aura on a puppet is not kept by the game as it is: the caster's aura goes to the creature's decider, which checks it and puts it on
        // the real creature; it comes back in the state. Until then the puppet shows it (the state removes what its decider does not list). The hook runs on a map thread.
        void LocalAura(Unit* unit, Aura* aura)
        {
            Creature* c = unit->ToCreature();
            if (!c || !c->IsPuppet() || Mirrors::Syncing() || Applying() || !_table || !Tracked(c))
                return;
            Player* local = Service::Get().Local();
            Unit* caster = aura->GetCaster();
            SpellInfo const* info = aura->GetSpellInfo();
            if (!local || !caster || info->IsPassive() || info->HasAttribute(SPELL_ATTR0_DO_NOT_DISPLAY))
                return;
            if (caster != local && caster->GetCharmerOrOwnerGUID() != local->GetGUID())
                return;
            ObjectGuid guid = c->GetGUID();
            uint32 spell = info->Id;
            int32 left = aura->GetDuration(), max = aura->GetMaxDuration();
            uint8 stacks = aura->GetStackAmount(), beneficial = info->IsPositive() ? 1 : 0;
            Defer([this, guid, spell, left, max, stacks, beneficial]
            {
                Player* l = Service::Get().Local();
                Creature* cr = l ? l->GetMap()->GetCreature(guid) : nullptr;
                if (!cr)
                    return;
                EntityKey key = KeyOf(cr);
                AuthorityEntry* e = _table->Find(key);
                if (!e || e->claim.Empty() || _table->Mine(*e))
                    return;
                Writer w;
                w.U8(EvAura);
                key.Write(w);
                w.Id(Service::Get().Me().id).U32(spell).U32(static_cast<uint32_t>(left)).U32(static_cast<uint32_t>(max)).U8(stacks).U8(beneficial);
                Service::Get().Say(e->claim.server, game::MsgEvent, w.data);
            });
        }

        void LocalDamage(Unit* attacker, Unit* victim, uint32 damage)
        {
            if (Applying() || !_table)
                return;
            Player* local = Service::Get().Local();
            if (!local)
                return;
            Unit* owner = attacker ? (attacker->GetCharmerOrOwner() ? attacker->GetCharmerOrOwner() : attacker) : nullptr;
            bool mine = owner == local || (attacker && attacker->IsCreature() && !attacker->ToCreature()->IsPuppet() && attacker->ToCreature()->IsSummon() && SummonKnown(attacker->ToCreature()));
            if (victim->IsCreature() && mine && Tracked(victim->ToCreature()))
            {
                ObjectGuid vg = victim->GetGUID();
                {
                    std::lock_guard<std::mutex> lock(_engageMutex);
                    _recentHit[vg] = GameTime::GetGameTimeMS().count();
                    if (_recentHit.size() > 500)
                        _recentHit.erase(_recentHit.begin());
                }
                Defer([this, vg, damage]
                {
                    Player* l = Service::Get().Local();
                    Creature* c = l ? l->GetMap()->GetCreature(vg) : nullptr;
                    if (!c)
                        return;
                    EntityKey key = KeyOf(c);
                    if (key.kind == 3)
                    {
                        // a summon is decided by the server that made it for good: the hit goes to that server alone, no claim is made
                        if (!(key.owner == Service::Get().Me().id))
                        {
                            Writer w;
                            w.Id(Service::Get().Me().id).U32(damage);
                            EventTo(EvDamage, key, w.data, key.owner);
                        }
                        return;
                    }
                    AuthorityEntry& e = _table->At(key);
                    int64_t now = Service::Get().NetNow();
                    bool first = e.claim.Empty() || e.situation == Situation::Untouched;
                    if (!_tapper.count(key))
                        _tapper[key] = Service::Get().Me().id;
                    if (e.claim.Empty() || (e.situation == Situation::Untouched && _table->Mine(e)) || !PeerLinked(e.claim.server))
                    {
                        e.claim = Claim{};
                        _table->Engage(e, Service::Get().Me().id, now);
                    }
                    else if (std::find(e.participants.begin(), e.participants.end(), Service::Get().Me().id) == e.participants.end())
                    {
                        _table->Engage(e, Service::Get().Me().id, now);
                    }
                    if (first)
                        Event(EvEngage, key, {});
                    Writer w;
                    w.Id(Service::Get().Me().id).U32(damage);
                    Event(EvDamage, key, w.data);
                });
            }
            else if (victim->IsPlayer() && victim->ToPlayer()->IsMirror() && owner == local)
            {
                // a hit on another server's player: that server applies it; here the mirror must not die of it
                Id128 peer = Mirrors::Get().PeerOf(victim->GetGUID());
                if (!(peer == Id128{}))
                {
                    Defer([this, peer, damage]
                    {
                        Writer w;
                        w.Id(peer).U32(damage).U8(1);
                        EntityKey none;
                        Writer all;
                        all.U8(EvPlayerHit);
                        none.Write(all);
                        all.Raw(w.data);
                        Service::Get().Say(peer, game::MsgEvent, all.data);
                    });
                }
            }
            else if (victim->IsCreature() && victim->ToCreature()->IsPuppet() && attacker && attacker->IsCreature() && !attacker->ToCreature()->IsPuppet() &&
                     Tracked(attacker->ToCreature()) && SummonKnown(victim->ToCreature()))
            {
                // a creature that this server decides hits another server's summon (pet, guardian, totem, escort): the summon's owner applies the hit
                ObjectGuid ag = attacker->GetGUID(), vg = victim->GetGUID();
                Defer([this, ag, vg, damage]
                {
                    Player* l = Service::Get().Local();
                    Creature* a = l ? l->GetMap()->GetCreature(ag) : nullptr;
                    Creature* v = l ? l->GetMap()->GetCreature(vg) : nullptr;
                    if (!a || !v)
                        return;
                    EntityKey vkey = KeyOf(v);
                    if (vkey.kind != 3 || vkey.owner == Service::Get().Me().id)
                        return;
                    Writer w;
                    w.U8(EvPlayerHit);
                    KeyOf(a).Write(w);
                    w.Id(vkey.owner).U32(damage).U8(2);
                    vkey.Write(w);
                    Service::Get().Say(vkey.owner, game::MsgEvent, w.data);
                });
            }
            else if (victim->IsPlayer() && victim->ToPlayer()->IsMirror() && attacker && attacker->IsCreature() && Tracked(attacker->ToCreature()))
            {
                // the decider of a creature fights another server's player: that server's player takes the hit
                Id128 peer = Mirrors::Get().PeerOf(victim->GetGUID());
                if (!(peer == Id128{}))
                {
                    ObjectGuid ag = attacker->GetGUID();
                    Defer([this, peer, damage, ag]
                    {
                        Player* l = Service::Get().Local();
                        Creature* c = l ? l->GetMap()->GetCreature(ag) : nullptr;
                        if (!c)
                            return;
                        Writer w;
                        w.U8(EvPlayerHit);
                        KeyOf(c).Write(w);
                        w.Id(peer).U32(damage).U8(0);
                        Service::Get().Say(peer, game::MsgEvent, w.data);
                    });
                }
            }
        }

        void LocalDeath(Unit* unit, Unit* killer)
        {
            if (Applying() || !_table || !unit->IsCreature() || !Tracked(unit->ToCreature()))
                return;
            ObjectGuid ug = unit->GetGUID();
            Defer([this, ug]
            {
                Player* l = Service::Get().Local();
                Creature* c = l ? l->GetMap()->GetCreature(ug) : nullptr;
                if (!c)
                    return;
                EntityKey key = KeyOf(c);
                if (key.kind == 3 && !(key.owner == Service::Get().Me().id))
                    return;   // (the death of a summon is told by the server that made it)
                AuthorityEntry& e = _table->At(key);
                int64_t now = Service::Get().NetNow();
                Id128 tapper = _tapper.count(key) ? _tapper[key] : Service::Get().Me().id;
                e.dead = true;
                e.generation = e.generation;
                // the death carries the tapper and the tapper's party: each member's server rewards its own player by the core's rules
                Id128 party{};
                if (tapper == Service::Get().Me().id)
                    party = CurrentPartyId(l);
                else if (PeerInfo* tp = Service::Get().FindPeer(tapper))
                    party = tp->presence.party;
                // a member of my party tapped it and I am the leader: the loot the core made here is the party's (Loot.cpp sends it), whoever landed the blow
                bool leaderKeeps = tapper != Service::Get().Me().id && !(party == Id128{}) && CurrentPartyId(l) == party && PartyLeader(l) == Service::Get().Me().id &&
                    (!c->loot.empty() || !c->loot.quest_items.empty());
                if (tapper != Service::Get().Me().id && !leaderKeeps)
                {
                    // the corpse is another player's to loot
                    c->loot.clear();
                    c->RemoveDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
                }
                if (key.kind == 3)
                    LOG_INFO("module", "[P2P] Summon {} (entry {}) of this server died; the tapper is {}", key.spawn, c->GetEntry(), Service::Get().NameOf(tapper));
                Writer w;
                w.Id(tapper).U64(e.generation).Id(party);
                Event(EvDeath, key, w.data);
                LootKilled(key, ug, tapper == Service::Get().Me().id || leaderKeeps, tapper == Service::Get().Me().id);   // (its loot is shared with the party)
                (void)now;
            });
        }

        void LocalCombat(Unit* unit, bool enter)
        {
            if (Applying() || !_table || !unit->IsCreature() || !Shareable(unit->ToCreature()))
                return;
            if (enter)
                return;
            ObjectGuid ug = unit->GetGUID();
            Defer([this, ug]
            {
                Player* l = Service::Get().Local();
                Creature* c = l ? l->GetMap()->GetCreature(ug) : nullptr;
                if (!c)
                    return;
                EntityKey key = KeyOf(c);
                AuthorityEntry& e = _table->At(key);
                if (_table->Mine(e) && e.situation == Situation::Negotiated)
                {
                    e.situation = Situation::Untouched;
                    e.participants.clear();
                    Event(EvDisengage, key, {});
                }
            });
        }

        void ObjectDown(GameObject* go, bool down)
        {
            if (Applying() || !_table || !go->GetSpawnId())
                return;
            if (!down)
            {
                std::lock_guard<std::mutex> lock(_echoMutex);
                if (_echo.erase(static_cast<uint32_t>(go->GetSpawnId())))
                    return;
            }
            ObjectGuid g = go->GetGUID();
            // (the core fixes the respawn time of a looted object only in its next update: at this hook it has the delay, not the time. Servers that applied 0
            // seconds never despawned the object, which could then be looted again and again.)
            uint32_t seconds = down ? go->GetRespawnDelay() : std::max<int64>(0, go->GetRespawnTime() - GameTime::GetGameTime().count());
            uint32_t spawn = static_cast<uint32_t>(go->GetSpawnId());
            uint32_t map = go->GetMapId();
            Defer([this, down, seconds, spawn, map]
            {
                EntityKey key;
                key.kind = 2;
                key.map = map;
                key.instance = Service::Get().Presence().instanceKey;
                key.spawn = spawn;
                Writer w;
                w.U32(seconds);
                Event(down ? EvObjectDown : EvObjectUp, key, w.data);
            });
        }

    private:
        std::unique_ptr<AuthorityTable> _table;
        std::map<EntityKey, ObjectGuid> _near;
        std::map<Id128, bool> _heard;   // the peers that were linked and responsive at the last scan
        std::map<EntityKey, Id128> _tapper;
        std::map<EntityKey, bool> _wasDead;
        std::map<EntityKey, int64_t> _asked, _releasedAt;
        std::mutex _echoMutex;
        std::set<uint32_t> _echo;   // objects taken down by another server, whose own despawn transition is not to be sent back
        std::mutex _deferMutex;
        std::vector<std::function<void()>> _deferred;
        std::mutex _clockMutex;
        std::map<uint32_t, ClockData> _clock;      // by spawn number: what the creature moves by (kind 0: not on the clock)
        std::atomic<bool> _clockOn{false};         // the shared world runs: creatures on the clock are driven from here
        std::set<ObjectGuid> _driven, _placed;     // creatures switched to the idle generator, and those put where the clock says at least once
        std::map<EntityKey, uint64_t> _lookSent;   // digest of what an untouched creature on the clock shows, to tell a change of it
        std::map<EntityKey, int64_t> _offClock;    // creatures that left the clock (a state without the OnClock flag), since when
        mutable std::mutex _sumMutex;
        std::map<ObjectGuid, EntityKey> _sumByGuid;   // the summons shared with other servers, this server's and its puppets of the others' (kind 3 keys)
        std::map<EntityKey, ObjectGuid> _sumByKey;
        std::map<EntityKey, SummonSig> _sumSig;
        std::map<EntityKey, int64_t> _sumHeard;        // puppets: when their state was last heard
        uint32_t _sumCounter = 0, _summonTimer = 0, _summonSecond = 0;
        uint32_t _timer = 0, _stateTimer = 0, _respawnTimer = 0, _scans = 0, _listTimer = 0, _clockTimer = 0;
        std::map<EntityKey, bool> _offSent;               // creatures this server decides that were told as off the clock (the rejoin is told once)
        std::map<EntityKey, std::vector<std::pair<Id128, float>>> _lastThreat;   // the last threat list of a creature that another server decided
        std::map<EntityKey, std::pair<Id128, int64_t>> _closerSince;
        std::set<EntityKey> _rewardedLate;
        std::map<EntityKey, int64_t> _listAsked;
        std::map<EntityKey, int64_t> _deadSeen;
        std::mutex _engageMutex;
        std::map<EntityKey, int64_t> _lastEngageMs;
        std::map<ObjectGuid, int64_t> _recentHit;         // creatures the local player hit lately: they may engage it by themselves here (the hit claims them)
    };

    thread_local bool World::_applying = false;

    class WorldUnitHooks : public UnitScript
    {
    public:
        WorldUnitHooks() : UnitScript("P2PWorldUnits", true, { UNITHOOK_ON_DAMAGE, UNITHOOK_ON_UNIT_DEATH, UNITHOOK_ON_UNIT_EXIT_COMBAT, UNITHOOK_ON_AURA_APPLY }) { }
        void OnAuraApply(Unit* unit, Aura* aura) override { World::Get().LocalAura(unit, aura); }
        void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
        {
            if (!World::Get().Active())
                return;
            World::Get().LocalDamage(attacker, victim, damage);
            // a hit on a mirror does not kill it here: its owner decides
            if (victim->IsPlayer() && victim->ToPlayer()->IsMirror() && damage >= victim->GetHealth() && victim->GetHealth() > 1)
                damage = victim->GetHealth() - 1;
        }
        void OnUnitDeath(Unit* unit, Unit* killer) override { World::Get().LocalDeath(unit, killer); }
        void OnUnitExitCombat(Unit* unit) override { World::Get().LocalCombat(unit, false); }
    };

    class WorldObjectHooks : public AllGameObjectScript
    {
    public:
        WorldObjectHooks() : AllGameObjectScript("P2PWorldObjects") { }
        void OnGameObjectLootStateChanged(GameObject* go, uint32 state, Unit* unit) override
        {
            if (!World::Get().Active())
                return;
            if (state == GO_JUST_DEACTIVATED)
                World::Get().ObjectDown(go, true);
            else if (state == GO_READY && !go->isSpawned())
                World::Get().ObjectDown(go, false);
        }
    };

    // for Loot.cpp: the creature of a key here, the key of a creature, and the server whose tap a dead creature is
    Creature* WorldLookup(EntityKey const& key, Player* local) { return World::Get().LootLookup(key, local); }
    bool WorldKeyOf(Creature const* c, EntityKey& key)
    {
        if (!c || !World::Tracked(c))
            return false;
        key = World::Get().KeyOf(c);
        return true;
    }
    // for Spells.cpp (world thread): the server that decides the creature, as this server knows it
    bool WorldDeciderOf(Creature const* c, Id128& server) { return World::Get().DeciderOf(c, server); }
    // for Spells.cpp: whether other servers know the creature under a key (a creature of the world database, or a shared summon)
    bool WorldTracked(Creature const* c) { return c && World::Tracked(c); }
    Id128 WorldTapper(EntityKey const& key) { return World::Get().TapperOf(key); }
    // for Spells.cpp (world thread): whether this server decides the creature (it has been claimed and the claim is ours), and whether another server does
    bool WorldDecides(Creature const* c)
    {
        return World::Get().DecidesHere(c);
    }

    void TestEntry(uint32 entry) { World::Get().SetTestEntry(entry); }
    void TestSpawn(uint32 spawn) { World::Get().SetTestSpawn(spawn); }

    std::string TestSummon(Player* local, uint32 entry) { return World::Get().TestSummon(local, entry); }
    std::string TestHitSummon(Player* local, uint32 damage) { return World::Get().TestHitSummon(local, damage); }
    std::string TestCreatureHitSummon(Player* local, uint32 damage) { return World::Get().TestCreatureHitSummon(local, damage); }
    std::string TestCreatureCast(Player* local, uint32 spell) { return World::Get().TestCreatureCast(local, spell); }
    std::string TestDoubleCast(Player* local, uint32 spell) { return World::Get().TestDoubleCast(local, spell); }
    std::string TestRadiusSound(Player* local, uint32 sound) { return World::Get().TestRadiusSound(local, sound); }
    std::string TestHitPlayer(Player* local, uint32 damage) { return World::Get().TestHitPlayer(local, damage); }
    std::string TestClaim(Player* local, bool earlier) { return World::Get().TestClaim(local, earlier); }

    std::string TestWorld(Player* local, bool hit, uint32 damage)
    {
        return hit ? World::Get().TestHit(local, damage) : World::Get().TestNear(local);
    }

    std::string DescribeWorld()
    {
        return World::Get().Describe();
    }

    // for the status command with a creature targeted: which server decides it, and in which state
    std::string DescribeCreature(Creature* c)
    {
        return World::Get().DescribeOne(c);
    }

    // A player talks to a creature: the creature turns toward that player, for everyone. When the creature is decided elsewhere, its decider is asked to do it.
    void World::LocalTalk(Player* local, ObjectGuid guid)
    {
        if (!_table || !local->IsInWorld())
            return;
        Creature* c = local->GetMap()->GetCreature(guid);
        if (!c || !Tracked(c))
            return;
        AuthorityEntry* e = _table->Find(KeyOf(c));
        if (!e || e->claim.Empty() || _table->Mine(*e))
            return;
        Writer w;
        w.U8(EvFace);
        KeyOf(c).Write(w);
        Service::Get().Say(e->claim.server, game::MsgEvent, w.data);
    }

    class WorldPackets : public ServerScript
    {
    public:
        WorldPackets() : ServerScript("P2PWorldPackets", { SERVERHOOK_CAN_PACKET_RECEIVE }) { }
        bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
        {
            Service& service = Service::Get();
            if (packet.GetOpcode() == CMSG_GOSSIP_HELLO && service.Enabled() && service.Local() && session == service.Local()->GetSession() && World::Get().Active())
            {
                WorldPacket p(packet);
                p.rpos(0);
                ObjectGuid guid;
                p >> guid;
                World::Get().LocalTalk(service.Local(), guid);
            }
            return true;
        }
    };

    void AddWorld()
    {
        Service::Get().AddFeature(&World::Get());
        new WorldUnitHooks();
        new WorldObjectHooks();
        new WorldPackets();
    }
}
