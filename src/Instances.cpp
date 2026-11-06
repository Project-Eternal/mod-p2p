/*
 * Instances (dungeons and raids). The instance of a party is kept by every member's server, wherever the member is, so that leadership can pass to any
 * member and the party's progress survives the leader.
 *
 * The record of a party's instance holds its identity (the party's network key, the map and the difficulty), what the instance script has saved, the creatures
 * that died with the time they come back, and a version number. Every member's server stores it in the core's instance tables under a local instance number that
 * a table of the module maps to the network key, and binds its player to that instance.
 *
 * The creatures are decided by the member whose server runs the instance, always one whose player is inside. It stays the same while that player is inside and
 * responding; a new one is chosen when there is none: the leader's server if the leader is inside, else the first member inside in the order of joining. The
 * decider tells every change of the record to all members; when nobody is inside, the leader's server sends the stored record to the members who lack it.
 * The state of an encounter in progress cannot be handed over: a fight ends as a wipe when, and only when, the decider changes.
 */
#include "Mirrors.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Group.h"
#include "InstanceSaveMgr.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "QueryResult.h"
#include <set>

namespace p2p
{
    Id128 PartyLeader(Player* local);            // Party.cpp
    Id128 CurrentPartyId(Player* local);         // Party.cpp

    namespace
    {
        constexpr int64_t ResendMs = 30000;

        struct DeadSpawn { uint32_t spawn = 0; uint32_t respawnAt = 0; };

        struct InstanceRecord
        {
            uint64_t key = 0;
            Id128 party{};
            uint32_t map = 0, version = 0, mask = 0;
            uint8_t difficulty = 0;
            std::string data;
            std::vector<DeadSpawn> dead;

            Bytes Encode(bool withVersion = true) const
            {
                Writer w;
                w.U64(key).Id(party).U32(map).U8(difficulty).U32(withVersion ? version : 0).U32(mask).Str(data).U32(static_cast<uint32_t>(dead.size()));
                for (DeadSpawn const& d : dead)
                    w.U32(d.spawn).U32(d.respawnAt);
                return w.data;
            }
            static bool Decode(Reader& r, InstanceRecord& out)
            {
                out.key = r.U64();
                out.party = r.Id();
                out.map = r.U32();
                out.difficulty = r.U8();
                out.version = r.U32();
                out.mask = r.U32();
                out.data = r.Str(8000);
                uint32_t n = r.U32();
                if (!r.ok() || n > 5000)
                    return false;
                out.dead.clear();
                for (uint32_t i = 0; i < n; ++i)
                {
                    DeadSpawn d;
                    d.spawn = r.U32();
                    d.respawnAt = r.U32();
                    out.dead.push_back(d);
                }
                return r.ok();
            }
        };

        std::string DeadToText(std::vector<DeadSpawn> const& dead)
        {
            std::string out;
            for (DeadSpawn const& d : dead)
                out += (out.empty() ? "" : ",") + std::to_string(d.spawn) + ":" + std::to_string(d.respawnAt);
            return out;
        }

        std::vector<DeadSpawn> DeadFromText(std::string const& text)
        {
            std::vector<DeadSpawn> out;
            size_t at = 0;
            while (at < text.size())
            {
                size_t comma = text.find(',', at);
                std::string item = text.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                size_t colon = item.find(':');
                if (colon != std::string::npos)
                    out.push_back({uint32_t(strtoul(item.substr(0, colon).c_str(), nullptr, 10)), uint32_t(strtoul(item.substr(colon + 1).c_str(), nullptr, 10))});
                if (comma == std::string::npos)
                    break;
                at = comma + 1;
            }
            return out;
        }
    }

    class Instances : public Feature
    {
    public:
        static Instances& Get()
        {
            static Instances instances;
            return instances;
        }

        void OnLogin(Player* player) override
        {
            _timer = 0;
            _decider.clear();
            _members.clear();
            _sentAt.clear();
            _lastBody.clear();
            CharacterDatabase.DirectExecute("CREATE TABLE IF NOT EXISTS p2p_instance (ikey TEXT NOT NULL PRIMARY KEY, party TEXT NOT NULL, map INTEGER NOT NULL, difficulty INTEGER NOT NULL, "
                "instance_id INTEGER NOT NULL, version INTEGER NOT NULL, mask INTEGER NOT NULL, data TEXT NOT NULL, dead TEXT NOT NULL)");
        }

        // who decides the creatures of the party's instance the local player is in: the member whose server runs it
        Id128 Decider(Player* local)
        {
            Service& service = Service::Get();
            uint64_t key = service.Presence().instanceKey;
            if (!key)
                return Id128{};
            Group* g = local->GetGroup();
            if (!g)
                return service.Me().id;
            Id128 me = service.Me().id;
            auto inside = [&](Id128 const& id)
            {
                if (id == me)
                    return true;
                PeerInfo* p = service.FindPeer(id);
                return p && p->linked && p->presence.instanceKey == key && service.Responsive(id);
            };
            Id128& current = _decider[key];
            if (!(current == Id128{}) && inside(current))
                return current;   // (it stays the same for as long as that player is inside and responding)
            current = Id128{};
            Id128 leader = PartyLeader(local);
            if (!(leader == Id128{}) && inside(leader))
                return current = leader;
            for (auto const& slot : g->GetMemberSlots())
            {
                Id128 id = id128Of(slot.guid);
                if (!(id == Id128{}) && inside(id))
                    return current = id;
            }
            return current = me;
        }

        static Id128 id128Of(ObjectGuid guid)
        {
            Service& s = Service::Get();
            if (Player* l = s.Local(); l && l->GetGUID() == guid)
                return s.Me().id;
            if (KnownCharacter* k = Characters::Get().FindByGuid(static_cast<uint32_t>(guid.GetCounter())))
                return k->cert.id;
            return Id128{};
        }

        void Update(uint32_t diff, Player* local) override
        {
            _timer += diff;
            if (_timer < 2000)
                return;
            _timer = 0;
            Group* g = local->GetGroup();
            Id128 party = CurrentPartyId(local);
            if (!g || party == Id128{})
                return;
            Service& service = Service::Get();
            Id128 me = service.Me().id;
            std::vector<Id128> members;
            for (auto const& slot : g->GetMemberSlots())
            {
                Id128 id = id128Of(slot.guid);
                if (!(id == Id128{}) && id != me)
                    members.push_back(id);
            }
            // a member that joined (or came back) gets the whole record at once
            std::set<Id128> now(members.begin(), members.end());
            std::vector<Id128> fresh;
            for (Id128 const& m : now)
                if (!_members.count(m))
                    fresh.push_back(m);
            _members = now;
            int64_t t = service.NetNow();
            Map* map = local->GetMap();
            if (map->IsDungeon())
            {
                // inside: the decider tells every change of the record
                if (Decider(local) == me)
                    SendLive(local, map, party, members, t);
                else
                    ApplyLive(local, map);
            }
            else if (PartyLeader(local) == me)
            {
                // nobody is known to be inside: the leader's server sends what it stored to the members who lack it
                for (auto const& rec : Stored(party))
                {
                    bool due = t - _sentAt[rec.key] >= ResendMs;
                    std::vector<Id128> to = due ? members : fresh;
                    if (to.empty())
                        continue;
                    _sentAt[rec.key] = t;
                    Writer w;
                    w.U8(1).Raw(rec.Encode());
                    for (Id128 const& m : to)
                        service.Say(m, game::MsgInstance, w.data);
                }
            }
            else if (!fresh.empty() || !_asked)
            {
                // a member that has no record of the party's instances asks the leader
                _asked = true;
                Writer w;
                w.U8(2).Id(party);
                if (Id128 leader = PartyLeader(local); !(leader == Id128{}))
                    service.Say(leader, game::MsgInstance, w.data);
            }
            if (!fresh.empty())
                _fresh = fresh;
        }

        // the records of a party that this server holds
        std::vector<InstanceRecord> Stored(Id128 const& party)
        {
            std::vector<InstanceRecord> out;
            if (QueryResult r = CharacterDatabase.Query("SELECT ikey, map, difficulty, version, mask, data, dead FROM p2p_instance WHERE party = '{}'", Hex(party)))
                do
                {
                    Field* f = r->Fetch();
                    InstanceRecord rec;
                    rec.party = party;
                    Bytes raw;
                    FromHex(f[0].Get<std::string>(), raw);
                    if (raw.size() == 8)
                        memcpy(&rec.key, raw.data(), 8);
                    rec.map = f[1].Get<uint32>();
                    rec.difficulty = f[2].Get<uint8>();
                    rec.version = f[3].Get<uint32>();
                    rec.mask = f[4].Get<uint32>();
                    rec.data = f[5].Get<std::string>();
                    rec.dead = DeadFromText(f[6].Get<std::string>());
                    out.push_back(std::move(rec));
                }
                while (r->NextRow());
            return out;
        }

        // ------------------------------------------------------------ the decider, inside
        void SendLive(Player* local, Map* map, Id128 const& party, std::vector<Id128> const& members, int64_t now)
        {
            InstanceMap* instance = map->ToInstanceMap();
            InstanceScript* script = instance ? instance->GetInstanceScript() : nullptr;
            InstanceRecord rec;
            rec.key = Service::Get().Presence().instanceKey;
            rec.party = party;
            rec.map = map->GetId();
            rec.difficulty = static_cast<uint8_t>(map->GetDifficulty());
            rec.mask = script ? script->GetCompletedEncounterMask() : 0;
            rec.data = script ? script->GetSaveData() : std::string();
            time_t gameNow = GameTime::GetGameTime().count();
            for (auto const& [spawn, creature] : map->GetCreatureBySpawnIdStore())
                if (creature->isDead() && !creature->IsSummon() && rec.dead.size() < 3000)
                {
                    time_t at = creature->GetRespawnTime();
                    rec.dead.push_back({static_cast<uint32_t>(spawn), static_cast<uint32_t>(at > gameNow ? at : gameNow)});
                }
            std::sort(rec.dead.begin(), rec.dead.end(), [](DeadSpawn const& a, DeadSpawn const& b) { return a.spawn < b.spawn; });
            Bytes body = rec.Encode(false);
            bool changed = body != _lastBody[rec.key];
            if (!changed && now - _sentAt[rec.key] < ResendMs && _fresh.empty())
                return;
            if (changed)
            {
                _lastBody[rec.key] = body;
                rec.version = Version(rec.key) + 1;
            }
            else
                rec.version = Version(rec.key);
            Store(rec, map->GetInstanceId());
            _sentAt[rec.key] = now;
            Writer w;
            w.U8(1).Raw(rec.Encode());
            for (Id128 const& m : members)
                Service::Get().Say(m, game::MsgInstance, w.data);
            _fresh.clear();
        }

        uint32_t Version(uint64_t key)
        {
            if (QueryResult r = CharacterDatabase.Query("SELECT version FROM p2p_instance WHERE ikey = '{}'", KeyHex(key)))
                return r->Fetch()[0].Get<uint32>();
            return 0;
        }

        static std::string KeyHex(uint64_t key) { Bytes raw(8); memcpy(raw.data(), &key, 8); return Hex(raw); }

        // the record is kept in the module's table (the instance number it maps to: the one given, or none yet)
        void Store(InstanceRecord const& rec, uint32_t instanceId)
        {
            std::string data = rec.data;
            CharacterDatabase.EscapeString(data);
            CharacterDatabase.DirectExecute("REPLACE INTO p2p_instance (ikey, party, map, difficulty, instance_id, version, mask, data, dead) VALUES "
                "('{}', '{}', {}, {}, COALESCE((SELECT instance_id FROM p2p_instance WHERE ikey = '{}'), {}), {}, {}, '{}', '{}')", KeyHex(rec.key), Hex(rec.party), rec.map, rec.difficulty,
                KeyHex(rec.key), instanceId, rec.version, rec.mask, data, DeadToText(rec.dead));
            if (instanceId)
                CharacterDatabase.DirectExecute("UPDATE p2p_instance SET instance_id = {} WHERE ikey = '{}'", instanceId, KeyHex(rec.key));
        }

        // ------------------------------------------------------------ a record arrives
        void OnEvent(InEvent const& e, Player* local) override
        {
            if (e.kind != InEvent::Message || e.type != game::MsgInstance || !local)
                return;
            Reader r(e.data);
            uint8_t kind = r.U8();
            if (kind == 2)
            {
                // a member asks for the records of the party: only its leader answers
                Id128 party = r.Id();
                if (!r.ok() || PartyLeader(local) != Service::Get().Me().id || CurrentPartyId(local) != party)
                    return;
                for (auto const& rec : Stored(party))
                {
                    Writer w;
                    w.U8(1).Raw(rec.Encode());
                    Service::Get().Say(e.peer, game::MsgInstance, w.data);
                }
                return;
            }
            InstanceRecord rec;
            if (kind != 1 || !InstanceRecord::Decode(r, rec) || !local->GetGroup() || CurrentPartyId(local) != rec.party)
                return;
            // only a member of the party may tell it
            Group* g = local->GetGroup();
            ObjectGuid sender;
            if (KnownCharacter* k = Characters::Get().Find(e.peer))
                sender = ObjectGuid::Create<HighGuid::Player>(k->guid);
            if (!sender || !g->IsMember(sender))
                return;
            if (rec.version <= Version(rec.key))
                return;   // (the same or an older version: nothing changed)
            Store(rec, 0);
            Bind(local, rec);
            LOG_INFO("module", "[P2P] Instance record of map {} version {} from {}: {} creatures down, {} bytes saved", rec.map, rec.version, Service::Get().NameOf(e.peer), rec.dead.size(), rec.data.size());
        }

        // The record becomes the core's own: a save under a local instance number, the creatures that are down in its respawn table, the local player bound to it
        void Bind(Player* local, InstanceRecord const& rec)
        {
            MapEntry const* entry = sMapStore.LookupEntry(rec.map);
            if (!entry || !entry->IsDungeon())
                return;
            Difficulty difficulty = Difficulty(rec.difficulty);
            uint32 instanceId = 0;
            if (QueryResult r = CharacterDatabase.Query("SELECT instance_id FROM p2p_instance WHERE ikey = '{}'", KeyHex(rec.key)))
                instanceId = r->Fetch()[0].Get<uint32>();
            // inside that very map: the live path applies it (ApplyLive); the stored copy is what the next entry will load
            InstanceSave* save = instanceId ? sInstanceSaveMgr->GetInstanceSave(instanceId) : nullptr;
            if (!save)
            {
                instanceId = sMapMgr->GenerateInstanceId();
                save = sInstanceSaveMgr->AddInstanceSave(rec.map, instanceId, difficulty);
                if (!save)
                    return;
                CharacterDatabase.DirectExecute("UPDATE p2p_instance SET instance_id = {} WHERE ikey = '{}'", instanceId, KeyHex(rec.key));
            }
            save->SetInstanceData(rec.data);
            save->SetCompletedEncounterMask(rec.mask);
            std::string data = rec.data;
            CharacterDatabase.EscapeString(data);
            CharacterDatabase.Execute("UPDATE instance SET data = '{}', completedEncounters = {} WHERE id = {}", data, rec.mask, instanceId);
            for (DeadSpawn const& d : rec.dead)
                CharacterDatabase.Execute("REPLACE INTO creature_respawn (guid, respawnTime, mapId, instanceId) VALUES ({}, {}, {}, {})", d.spawn, d.respawnAt, rec.map, instanceId);
            InstancePlayerBind* existing = sInstanceSaveMgr->PlayerGetBoundInstance(local->GetGUID(), rec.map, difficulty);
            bool permanent = entry->IsRaid();
            if (existing && existing->perm && !permanent)
                return;
            if (!existing || existing->save != save)
                sInstanceSaveMgr->PlayerBindToInstance(local->GetGUID(), save, permanent, local);
        }

        // inside an instance that another member decides: its changes to the record are applied to the live map
        void ApplyLive(Player* local, Map* map)
        {
            uint64_t key = Service::Get().Presence().instanceKey;
            std::string keyHex = KeyHex(key);
            QueryResult r = CharacterDatabase.Query("SELECT mask, data, dead, version FROM p2p_instance WHERE ikey = '{}'", keyHex);
            if (!r)
                return;
            Field* f = r->Fetch();
            uint32 version = f[3].Get<uint32>();
            if (version == _applied[key])
                return;
            _applied[key] = version;
            InstanceMap* instance = map->ToInstanceMap();
            InstanceScript* script = instance ? instance->GetInstanceScript() : nullptr;
            std::string data = f[1].Get<std::string>();
            if (script && !data.empty() && _loaded.insert({map->GetId(), map->GetInstanceId()}).second)
                script->Load(data.c_str());
            for (DeadSpawn const& d : DeadFromText(f[2].Get<std::string>()))
            {
                auto range = map->GetCreatureBySpawnIdStore().equal_range(d.spawn);
                for (auto it = range.first; it != range.second; ++it)
                    if (it->second->IsAlive() && !it->second->IsInCombat())
                    {
                        it->second->SetPuppet(true);
                        Unit::Kill(nullptr, it->second, false);
                        it->second->RemoveDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
                    }
            }
        }

    private:
        uint32_t _timer = 0;
        bool _asked = false;
        std::map<uint64_t, Id128> _decider;
        std::set<Id128> _members;
        std::vector<Id128> _fresh;
        std::map<uint64_t, int64_t> _sentAt;
        std::map<uint64_t, Bytes> _lastBody;
        std::map<uint64_t, uint32_t> _applied;
        std::set<std::pair<uint32_t, uint32_t>> _loaded;
    };

    // the member whose server runs the party's instance the local player is in (zero: not in an instance)
    Id128 InstanceDecider(Player* local)
    {
        return Instances::Get().Decider(local);
    }

    void AddInstances()
    {
        Service::Get().AddFeature(&Instances::Get());
    }
}
