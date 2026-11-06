/*
 * Parties and raids. Every server keeps the core's own Group object, with the mirrors of the other players as members (and, for a member far away,
 * who has no mirror, a member slot without a Player). A party has a permanent random identifier and a private room named from it; a returning
 * player finds the party again through that room.
 *
 * The leader's server holds the party's state: it sends the party's whole record whenever it changes and every 30 seconds otherwise, and the
 * servers of the members make their copy of the group equal to it. What a player does to the group (invite, accept, leave, kick, hand over
 * leadership, loot rules, assistants, subgroups) is told to the other members' servers, which do the same to their copy at once; the record is
 * what repairs a copy that drifted. When the leader is gone, every member computes the next leader from the same record and that server takes over.
 *
 * Members are known wherever they are: each member's server sends the other members its player's state (position, zone, health, power, level,
 * auras, flags) over the links, every second on the same map and every 5 seconds otherwise, and the receiving server shows it in the party frames,
 * the map and the minimap as the packet the core sends for members it has.
 *
 * An invitation carries an identifier and the target's server answers it (shown, refused, busy). The inviting server clears the core's pending
 * invitation of the target when it is declined or refused, when no answer "shown" comes in time, when the game's own time for an answer passes, or
 * when the target is gone: the target can then be invited again at once.
 */
#include "Mirrors.h"
#include "Creature.h"
#include "core/Authority.h"
#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "QueryResult.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>

namespace p2p
{
    bool WorldKeyOf(Creature const* c, EntityKey& key);                   // World.cpp
    Creature* WorldLookup(EntityKey const& key, Player* local);          // World.cpp

    namespace
    {
        enum Kind : uint8_t { Invite = 1, Accept = 2, Decline = 3, Leave = 4, Kick = 5, SetLeader = 6, Loot = 7, Disband_ = 8, Raid = 9, Assist = 10, Sub = 11, PartyId = 12,
            InviteReply = 13, RecordMsg = 14, MemberStateMsg = 15, IconSet = 16 };
        enum Reply : uint8_t { Shown = 1, Declined = 2, Refused = 3, Busy = 4 };
        constexpr int64_t ShownWaitMs = 10000, InviteTimeMs = 60000, RecordEveryMs = 30000, LeaderGoneMs = 15000, RejoinWaitMs = 45000;   // RejoinWaitMs: a member in touch that announces another party (or none) this long is removed

        Id128 IdOfGuid(ObjectGuid guid)
        {
            Service& s = Service::Get();
            if (Player* l = s.Local(); l && l->GetGUID() == guid)
                return s.Me().id;
            if (KnownCharacter* k = Characters::Get().FindByGuid(static_cast<uint32_t>(guid.GetCounter())))
                return k->cert.id;
            return Id128{};
        }

        ObjectGuid GuidOfId(Id128 const& id);

        // A target icon of a party or raid: what it marks as the other servers know it: a character, or a creature by its spawn key. Kind 0 is none.
        void WriteIcon(Writer& w, ObjectGuid guid)
        {
            Service& s = Service::Get();
            Player* local = s.Local();
            if (guid.IsEmpty() || !local)
            {
                w.U8(0);
                return;
            }
            if (guid.IsPlayer())
            {
                Id128 id = IdOfGuid(guid);
                if (id == Id128{})
                    w.U8(0);
                else
                    w.U8(1).Id(id);
                return;
            }
            Creature* c = local->IsInWorld() ? local->GetMap()->GetCreature(guid) : nullptr;
            EntityKey key;
            if (c && WorldKeyOf(c, key))
            {
                w.U8(2);
                key.Write(w);
            }
            else
                w.U8(0);
        }

        // the guid on this server of a marked thing (empty when this server has not got it: the icon is then not shown)
        ObjectGuid ReadIcon(Reader& r)
        {
            uint8_t kind = r.U8();
            Player* local = Service::Get().Local();
            if (kind == 1)
            {
                Id128 id = r.Id();
                return r.ok() ? GuidOfId(id) : ObjectGuid::Empty;
            }
            if (kind == 2)
            {
                EntityKey key = EntityKey::Read(r);
                Creature* c = r.ok() && local ? WorldLookup(key, local) : nullptr;
                return c ? c->GetGUID() : ObjectGuid::Empty;
            }
            return ObjectGuid::Empty;
        }

        ObjectGuid GuidOfId(Id128 const& id)
        {
            Service& s = Service::Get();
            if (id == s.Me().id)
                return s.Local() ? s.Local()->GetGUID() : ObjectGuid::Empty;
            if (KnownCharacter* k = Characters::Get().Meet(id))
                return ObjectGuid::Create<HighGuid::Player>(k->guid);
            return ObjectGuid::Empty;
        }

        // The whole state of a party, as its leader's server sees it
        struct PartyRecord
        {
            std::vector<Bytes> icons;   // the eight target icons, each as WriteIcon wrote it (the record is made and applied where the group is)
            struct Member { Id128 id{}; uint8_t subgroup = 0, flags = 0; };
            Id128 party{};
            uint32_t version = 0;
            Id128 leader{}, master{};
            std::vector<Member> members;   // in the order they joined
            uint8_t raid = 0, lootMethod = 0, threshold = 0;

            Bytes Encode(bool withVersion = true) const
            {
                Writer w;
                w.Id(party).U32(withVersion ? version : 0).Id(leader).Id(master).U8(raid).U8(lootMethod).U8(threshold).U32(static_cast<uint32_t>(members.size()));
                for (Member const& m : members)
                    w.Id(m.id).U8(m.subgroup).U8(m.flags);
                for (uint32_t i = 0; i < 8; ++i)
                    w.Raw(i < icons.size() ? icons[i] : Bytes{0});
                return w.data;
            }
            static bool Decode(Reader& r, PartyRecord& out)
            {
                out.party = r.Id();
                out.version = r.U32();
                out.leader = r.Id();
                out.master = r.Id();
                out.raid = r.U8();
                out.lootMethod = r.U8();
                out.threshold = r.U8();
                uint32_t n = r.U32();
                if (!r.ok() || n > 40)
                    return false;
                out.members.clear();
                for (uint32_t i = 0; i < n; ++i)
                {
                    Member m;
                    m.id = r.Id();
                    m.subgroup = r.U8();
                    m.flags = r.U8();
                    out.members.push_back(m);
                }
                out.icons.clear();
                for (int i = 0; i < 8; ++i)
                {
                    // (a token: its kind, then nothing, a character's identifier, or a creature's key)
                    Writer one;
                    uint8_t kind = r.U8();
                    one.U8(kind);
                    if (kind == 1)
                        one.Id(r.Id());
                    else if (kind == 2)
                        EntityKey::Read(r).Write(one);
                    out.icons.push_back(one.data);
                }
                return r.ok();
            }
        };

        // What a member's server says of its player to the other members
        struct MemberState
        {
            Id128 party{};
            uint32_t map = 0, zone = 0;
            float x = 0, y = 0;
            uint32_t health = 0, maxHealth = 0, power = 0, maxPower = 0;
            uint8_t powerType = 0, level = 1, flags = 0;   // flags: 1 dead, 2 ghost, 4 pvp, 8 afk, 16 dnd
            std::vector<std::pair<uint32_t, uint8_t>> auras;

            Bytes Encode() const
            {
                Writer w;
                w.Id(party).U32(map).U32(zone).F32(x).F32(y).U32(health).U32(maxHealth).U32(power).U32(maxPower).U8(powerType).U8(level).U8(flags);
                w.U8(static_cast<uint8_t>(auras.size()));
                for (auto const& [spell, f] : auras)
                    w.U32(spell).U8(f);
                return w.data;
            }
            static bool Decode(Reader& r, MemberState& s)
            {
                s.party = r.Id();
                s.map = r.U32(); s.zone = r.U32(); s.x = r.F32(); s.y = r.F32();
                s.health = r.U32(); s.maxHealth = r.U32(); s.power = r.U32(); s.maxPower = r.U32();
                s.powerType = r.U8(); s.level = r.U8(); s.flags = r.U8();
                uint8_t n = r.U8();
                if (!r.ok() || n > 32)
                    return false;
                s.auras.clear();
                for (uint8_t i = 0; i < n; ++i)
                {
                    uint32_t spell = r.U32();
                    uint8_t f = r.U8();
                    s.auras.emplace_back(spell, f);
                }
                return r.ok();
            }
        };
    }

    class Party : public Feature
    {
    public:
        static Party& Get()
        {
            static Party party;
            return party;
        }

        void OnLogin(Player* player) override
        {
            _ids.clear();
            _announced.clear();
            _invites.clear();
            _states.clear();
            _version.clear();
            _lastRecord.clear();
            _leaderGoneSince = 0;
            _pendingParty = Id128{};
            _incomingParty = Id128{};
            _inviter = Id128{};
            _timer = _stateTimer = 0;
            _lastSentAt = _lastRecordAt = 0;
            CharacterDatabase.DirectExecute("CREATE TABLE IF NOT EXISTS p2p_party (group_guid INTEGER NOT NULL PRIMARY KEY, party_id TEXT NOT NULL)");
            if (QueryResult r = CharacterDatabase.Query("SELECT group_guid, party_id FROM p2p_party"))
                do
                {
                    Bytes raw;
                    if (FromHex(r->Fetch()[1].Get<std::string>(), raw) && raw.size() == 16)
                    {
                        Id128 id{};
                        memcpy(id.data(), raw.data(), 16);
                        _ids[r->Fetch()[0].Get<uint32>()] = id;
                    }
                }
                while (r->NextRow());
        }

        // the party id of the local player's group, if it has one
        Id128 CurrentParty(Player* local)
        {
            Group* g = local ? local->GetGroup() : nullptr;
            if (!g || g->isBGGroup() || g->isBFGroup())
                return Id128{};
            auto it = _ids.find(static_cast<uint32_t>(g->GetGUID().GetCounter()));
            return it == _ids.end() ? Id128{} : it->second;
        }

        void Presence(Player* local, game::Presence& presence) override { presence.party = CurrentParty(local); }

        void Rooms(Player* local, std::vector<Room>& rooms) override
        {
            Id128 id = CurrentParty(local);
            if (!(id == Id128{}))
                rooms.push_back(PrivateRoom(Service::Get().RoomScope(), "party", PartySecret(id)));
        }

        // the other members of the local player's group, by net identity
        std::vector<Id128> Members(Player* local, bool includeMe = false)
        {
            std::vector<Id128> out;
            Group* group = local ? local->GetGroup() : nullptr;
            if (group)
                for (auto const& slot : group->GetMemberSlots())
                {
                    Id128 id = IdOfGuid(slot.guid);
                    if (!(id == Id128{}) && (includeMe || id != Service::Get().Me().id))
                        out.push_back(id);
                }
            return out;
        }

        void Update(uint32_t diff, Player* local) override
        {
            _timer += diff;
            _stateTimer += diff;
            if (_timer < 500)
                return;
            _timer = 0;
            int64_t now = Service::Get().NetNow();
            ExpireInvites(local, now);
            Group* g = local->GetGroup();
            if (!g || g->isBGGroup() || g->isBFGroup())
                return;
            uint32_t guid = static_cast<uint32_t>(g->GetGUID().GetCounter());
            if (!_ids.count(guid))
            {
                Id128 id = !(_incomingParty == Id128{}) ? _incomingParty : !(_pendingParty == Id128{}) ? _pendingParty : crypto::RandomId();
                Adopt(guid, id);
            }
            Id128 id = _ids[guid];
            // members whose servers may not know the party yet hear it (the lower identifier wins if two were made)
            for (Id128 const& member : Members(local))
            {
                PeerInfo* p = Service::Get().FindPeer(member);
                if (p && p->linked && _announced[member] != id)
                {
                    _announced[member] = id;
                    Writer w;
                    w.U8(PartyId).Id(id).Id(Id128{}).U32(0).U32(0).U8(0);
                    Service::Get().Say(member, game::MsgParty, w.data);
                }
            }
            LinkMirrors(local, g);
            if (g->IsLeader(local->GetGUID()))
            {
                RemoveAbsent(local, g, id, now);
                SendRecord(local, g, id, now);
            }
            else
                CheckLeader(local, g, now);
            if (_stateTimer >= 1000)
            {
                _stateTimer = 0;
                SendMemberState(local, g, id, now);
            }
        }

        void Adopt(uint32_t groupGuid, Id128 const& id)
        {
            _ids[groupGuid] = id;
            _joinedAt[id] = Service::Get().NetNow();
            CharacterDatabase.Execute("REPLACE INTO p2p_party (group_guid, party_id) VALUES ({}, '{}')", groupGuid, Hex(id));
            Service::Get().WantRefresh();
        }

        // a mirror of a member is linked to the group (the group knows it as a Player: loot, range, rewards)
        void LinkMirrors(Player* local, Group* g)
        {
            for (auto const& slot : g->GetMemberSlots())
            {
                if (slot.guid == local->GetGUID())
                    continue;
                Id128 id = IdOfGuid(slot.guid);
                Player* mirror = id == Id128{} ? nullptr : Mirrors::Get().MirrorOf(id);
                if (mirror && mirror->IsInWorld() && mirror->GetGroup() != g)
                    mirror->SetGroup(g, slot.group);
            }
        }

        // A member whose server is in touch and announces another party, or none, is not in this one: a member that came online and did not ask to rejoin is removed
        // (by the leader's server, so by the next record from every server); an invited character is not a member until the record says so
        void RemoveAbsent(Player* local, Group* g, Id128 const& party, int64_t now)
        {
            std::vector<ObjectGuid> remove;
            for (auto const& slot : g->GetMemberSlots())
            {
                if (slot.guid == local->GetGUID())
                    continue;
                Id128 m = IdOfGuid(slot.guid);
                PeerInfo* p = m == Id128{} ? nullptr : Service::Get().FindPeer(m);
                bool outside = p && p->inTouch && p->compatible && p->announcedAt && p->presence.party != party && !_invites.count(m);
                int64_t& since = _outside[m];
                if (!outside)
                {
                    since = 0;
                    continue;
                }
                if (!since)
                    since = now;
                if (now - since >= RejoinWaitMs)
                    remove.push_back(slot.guid);
            }
            for (ObjectGuid const& guid : remove)
            {
                LOG_INFO("module", "[P2P] {} is online but is not in the party: removed from it", Service::Get().NameOf(IdOfGuid(guid)));
                _outside.erase(IdOfGuid(guid));
                g->RemoveMember(guid, GROUP_REMOVEMETHOD_KICK);
                g->SendUpdate();
            }
        }

        // The state to show of a member that has no Player here: what its server told, or until that arrives what its presence says (never coordinates 0 0 as if it were there)
        bool StateOf(Id128 const& id, MemberState& out)
        {
            auto it = _states.find(id);
            if (it != _states.end() && (it->second.x != 0 || it->second.y != 0 || it->second.zone != 0))
            {
                out = it->second;
                return true;
            }
            PeerInfo* p = id == Id128{} ? nullptr : Service::Get().FindPeer(id);
            if (!p || !p->inTouch || !p->compatible || !p->announcedAt)
                return false;
            out = MemberState();
            out.party = p->presence.party;
            out.map = p->presence.map;
            out.zone = p->presence.zone;
            out.x = p->presence.x;
            out.y = p->presence.y;
            out.level = p->presence.level;
            out.health = out.maxHealth = 100;
            out.flags = ((p->presence.status & 1) ? 8 : 0) | ((p->presence.status & 2) ? 16 : 0);
            return true;
        }

        // ------------------------------------------------------------ the record of the party (the leader's server)
        PartyRecord BuildRecord(Player* local, Group* g, Id128 const& party)
        {
            PartyRecord rec;
            rec.party = party;
            rec.leader = IdOfGuid(g->GetLeaderGUID());
            rec.master = IdOfGuid(g->GetMasterLooterGuid());
            rec.raid = g->isRaidGroup() ? 1 : 0;
            rec.lootMethod = static_cast<uint8_t>(g->GetLootMethod());
            rec.threshold = static_cast<uint8_t>(g->GetLootThreshold());
            for (uint8 i = 0; i < TARGETICONCOUNT; ++i)
            {
                Writer one;
                WriteIcon(one, g->GetTargetIcon(i));
                rec.icons.push_back(one.data);
            }
            for (auto const& slot : g->GetMemberSlots())
            {
                Id128 id = IdOfGuid(slot.guid);
                if (id == Id128{})
                    continue;   // (a member this server has no identity for cannot be told to the others)
                PartyRecord::Member m;
                m.id = id;
                m.subgroup = slot.group;
                m.flags = slot.flags;
                rec.members.push_back(m);
            }
            return rec;
        }

        void SendRecord(Player* local, Group* g, Id128 const& party, int64_t now)
        {
            PartyRecord rec = BuildRecord(local, g, party);
            Bytes body = rec.Encode(false);
            bool changed = body != _lastRecord[party];
            if (!changed && now - _lastRecordAt < RecordEveryMs)
                return;
            if (changed)
                _lastRecord[party] = body;
            uint32_t& version = _version[party];
            if (changed)
                ++version;
            rec.version = version;
            _lastRecordAt = now;
            Writer w;
            w.U8(RecordMsg).Raw(rec.Encode());
            for (Id128 const& member : Members(local))
                Service::Get().Say(member, game::MsgParty, w.data);
        }

        // the copy of the group is made equal to the record of its leader
        void ApplyRecord(Id128 const& from, PartyRecord const& rec, Player* local)
        {
            Group* g = local->GetGroup();
            if (!g || g->isBGGroup() || g->isBFGroup() || rec.leader != from)
                return;
            Id128 mine = CurrentParty(local);
            if (mine == Id128{} || mine != rec.party)
                return;
            Id128 me = Service::Get().Me().id;
            ObjectGuid leaderGuid = GuidOfId(rec.leader);
            uint32_t& seen = _version[rec.party];
            if (!leaderGuid || !g->IsMember(leaderGuid))
                return;
            if (g->GetLeaderGUID() != leaderGuid && rec.version <= seen)
                return;   // (an older record of a leader that has handed over)
            seen = std::max(seen, rec.version);
            bool listed = false;
            std::set<Id128> want;
            for (PartyRecord::Member const& m : rec.members)
            {
                want.insert(m.id);
                listed = listed || m.id == me;
            }
            if (!listed)
            {
                // the leader's record does not hold the local player: it was removed (kicked, or the group dissolved). A record built before the leader heard of
                // the acceptance may still be on its way to a member who has just joined: that is waited out.
                if (Service::Get().NetNow() - _joinedAt[rec.party] < 10000)
                    return;
                g->RemoveMember(local->GetGUID(), GROUP_REMOVEMETHOD_KICK);
                return;
            }
            // members that are missing come, those that should not be there go
            for (PartyRecord::Member const& m : rec.members)
            {
                ObjectGuid guid = GuidOfId(m.id);
                if (!guid || g->IsMember(guid))
                    continue;
                Player* mirror = m.id == me ? local : Mirrors::Get().MirrorOf(m.id);
                if (mirror && !mirror->GetGroup() && !mirror->GetGroupInvite())
                    g->AddMember(mirror);
                else if (!mirror)
                    g->LoadMemberFromDB(guid.GetCounter(), m.flags, m.subgroup, 0);
            }
            std::vector<ObjectGuid> remove;
            for (auto const& slot : g->GetMemberSlots())
            {
                Id128 id = IdOfGuid(slot.guid);
                if (slot.guid != local->GetGUID() && !want.count(id))
                    remove.push_back(slot.guid);
            }
            for (ObjectGuid const& guid : remove)
                g->RemoveMember(guid, GROUP_REMOVEMETHOD_LEAVE);
            if (!(g = local->GetGroup()))
                return;
            if (g->GetLeaderGUID() != leaderGuid)
            {
                g->ChangeLeader(leaderGuid);
                g->SendUpdate();   // (the core's own handler does it: ChangeLeader alone tells the chat, the group list sets the crown in the party frame)
            }
            if (rec.raid && !g->isRaidGroup())
                g->ConvertToRaid();
            if (g->GetLootMethod() != LootMethod(rec.lootMethod) || g->GetLootThreshold() != ItemQualities(rec.threshold) || g->GetMasterLooterGuid() != GuidOfId(rec.master))
            {
                g->SetLootMethod(LootMethod(rec.lootMethod));
                g->SetLooterGuid(g->GetLooterGuid());
                g->SetMasterLooterGuid(GuidOfId(rec.master));
                g->SetLootThreshold(ItemQualities(rec.threshold));
                g->SendUpdate();
            }
            for (size_t i = 0; i < rec.icons.size() && i < TARGETICONCOUNT; ++i)
            {
                Reader token(rec.icons[i]);
                ObjectGuid want = ReadIcon(token);
                if (rec.icons[i].size() > 1 && want.IsEmpty())
                    continue;   // (marks something this server has not got)
                if (want != g->GetTargetIcon(static_cast<uint8>(i)))
                    g->SetTargetIcon(static_cast<uint8>(i), leaderGuid, want);
            }
            for (PartyRecord::Member const& m : rec.members)
            {
                ObjectGuid guid = GuidOfId(m.id);
                if (!guid || !g->IsMember(guid))
                    continue;
                if (g->GetMemberGroup(guid) != m.subgroup)
                    g->ChangeMembersGroup(guid, m.subgroup);
                bool assistant = (m.flags & MEMBER_FLAG_ASSISTANT) != 0;
                if (g->IsAssistant(guid) != assistant)
                {
                    g->SetGroupMemberFlag(guid, assistant, MEMBER_FLAG_ASSISTANT);
                    g->SendUpdate();
                }
            }
        }

        // When the leader is gone for good, every member computes the next leader from the same list; that server takes over
        void CheckLeader(Player* local, Group* g, int64_t now)
        {
            Id128 leader = IdOfGuid(g->GetLeaderGUID());
            PeerInfo* p = leader == Id128{} ? nullptr : Service::Get().FindPeer(leader);
            bool gone = leader != Service::Get().Me().id && (!p || !p->inTouch);
            if (!gone)
            {
                _leaderGoneSince = 0;
                return;
            }
            if (!_leaderGoneSince)
                _leaderGoneSince = now;
            if (now - _leaderGoneSince < LeaderGoneMs)
                return;
            for (auto const& slot : g->GetMemberSlots())
            {
                Id128 id = IdOfGuid(slot.guid);
                if (id == Id128{} || id == leader)
                    continue;
                PeerInfo* m = id == Service::Get().Me().id ? nullptr : Service::Get().FindPeer(id);
                if (id == Service::Get().Me().id || (m && m->inTouch))
                {
                    g->ChangeLeader(slot.guid);
                    g->SendUpdate();
                    LOG_INFO("module", "[P2P] The leader of the party is gone: {} leads now", Service::Get().NameOf(id));
                    break;
                }
            }
            _leaderGoneSince = 0;
        }

        // ------------------------------------------------------------ members wherever they are
        void SendMemberState(Player* local, Group* g, Id128 const& party, int64_t now)
        {
            MemberState s;
            s.party = party;
            s.map = local->GetMapId();
            s.zone = local->GetZoneId();
            s.x = local->GetPositionX();
            s.y = local->GetPositionY();
            s.health = local->GetHealth();
            s.maxHealth = local->GetMaxHealth();
            s.powerType = local->getPowerType();
            s.power = local->GetPower(local->getPowerType());
            s.maxPower = local->GetMaxPower(local->getPowerType());
            s.level = local->GetLevel();
            s.flags = (!local->IsAlive() ? (local->HasPlayerFlag(PLAYER_FLAGS_GHOST) ? 2 : 1) : 0) | (local->IsPvP() ? 4 : 0) | (local->isAFK() ? 8 : 0) | (local->isDND() ? 16 : 0);
            for (uint8 i = 0; i < 32; ++i)
                if (AuraApplication const* app = local->GetVisibleAura(i))
                    s.auras.emplace_back(app->GetBase()->GetId(), app->GetFlags());
            bool bigChange = s.maxHealth && std::abs(int(s.health) - int(_lastHealth)) * 10 >= int(s.maxHealth);
            Bytes body = s.Encode();
            for (Id128 const& member : Members(local))
            {
                PeerInfo* p = Service::Get().FindPeer(member);
                if (!p || !p->linked)
                    continue;
                // every second on the same map, every 5 seconds otherwise, at once on a large change of health
                bool same = p->presence.map == s.map;
                int64_t& last = _statesSentAt[member];
                if (now - last < (same ? 1000 : 5000) && !bigChange)
                    continue;
                last = now;
                Writer w;
                w.U8(MemberStateMsg).Raw(body);
                Service::Get().SayKeyed(member, game::KeyMemberState, game::MsgParty, w.data);
            }
            _lastHealth = s.health;
        }

        // the state of a member whose server told it: the packet the core sends for a member it has
        void ShowMember(Id128 const& id, MemberState const& s, bool online, Player* local)
        {
            ObjectGuid guid = GuidOfId(id);
            if (!guid || !local->GetGroup() || !local->GetGroup()->IsMember(guid))
                return;
            if (Mirrors::Get().MirrorOf(id))
                return;   // (the core sends for a member it has)
            WorldPacket data(SMSG_PARTY_MEMBER_STATS_FULL, 64);
            data << uint8(0);
            data << guid.WriteAsPacked();
            if (!online)
            {
                data << uint32(GROUP_UPDATE_FLAG_STATUS);
                data << uint16(MEMBER_STATUS_OFFLINE);
                local->SendDirectMessage(&data);
                return;
            }
            uint32 flags = GROUP_UPDATE_FLAG_STATUS | GROUP_UPDATE_FLAG_CUR_HP | GROUP_UPDATE_FLAG_MAX_HP | GROUP_UPDATE_FLAG_CUR_POWER | GROUP_UPDATE_FLAG_MAX_POWER
                | GROUP_UPDATE_FLAG_LEVEL | GROUP_UPDATE_FLAG_ZONE | GROUP_UPDATE_FLAG_POSITION | GROUP_UPDATE_FLAG_AURAS | GROUP_UPDATE_FLAG_PET_NAME
                | GROUP_UPDATE_FLAG_PET_MODEL_ID | GROUP_UPDATE_FLAG_PET_AURAS;
            if (s.powerType != POWER_MANA)
                flags |= GROUP_UPDATE_FLAG_POWER_TYPE;
            uint16 status = MEMBER_STATUS_ONLINE;
            if (s.flags & 1) status |= MEMBER_STATUS_DEAD;
            if (s.flags & 2) status |= MEMBER_STATUS_GHOST;
            if (s.flags & 4) status |= MEMBER_STATUS_PVP;
            if (s.flags & 8) status |= MEMBER_STATUS_AFK;
            if (s.flags & 16) status |= MEMBER_STATUS_DND;
            data << uint32(flags);
            data << uint16(status);
            data << uint32(s.health);
            data << uint32(s.maxHealth);
            if (flags & GROUP_UPDATE_FLAG_POWER_TYPE)
                data << uint8(s.powerType);
            data << uint16(s.power);
            data << uint16(s.maxPower);
            data << uint16(s.level);
            data << uint16(s.zone);
            data << uint16(s.x);
            data << uint16(s.y);
            uint64 auraMask = 0;
            std::size_t maskPos = data.wpos();
            data << uint64(0);
            for (size_t i = 0; i < s.auras.size() && i < MAX_AURAS_GROUP_UPDATE; ++i)
            {
                auraMask |= uint64(1) << i;
                data << uint32(s.auras[i].first);
                data << uint8(s.auras[i].second);
            }
            data.put<uint64>(maskPos, auraMask);
            data << std::string();      // pet name
            data << uint16(0);          // pet model
            data << uint64(0);          // pet auras
            local->SendDirectMessage(&data);
        }

        // ------------------------------------------------------------ invitations
        void ExpireInvites(Player* local, int64_t now)
        {
            for (auto it = _invites.begin(); it != _invites.end();)
            {
                Invitation& inv = it->second;
                PeerInfo* p = Service::Get().FindPeer(it->first);
                Player* mirror = Mirrors::Get().MirrorOf(it->first);
                bool pending = mirror && mirror->GetGroupInvite();
                bool gone = !p || !p->linked;
                if (!pending || (!inv.shown && now - inv.at > ShownWaitMs) || now - inv.at > InviteTimeMs || gone)
                {
                    // the core still holds the invitation on the target's row: it is declined for it, so that the target can be invited again at once
                    if (pending)
                    {
                        WorldPacket data(CMSG_GROUP_DECLINE, 0);
                        mirror->GetSession()->HandleGroupDeclineOpcode(data);
                    }
                    it = _invites.erase(it);
                }
                else
                    ++it;
            }
        }

        // ------------------------------------------------------------ what the local player does
        // true: the packet must not reach the core
        bool LocalPacket(WorldSession* session, WorldPacket const& packet)
        {
            Service& service = Service::Get();
            Player* local = service.Local();
            if (!service.Enabled() || !local || session != local->GetSession())
                return false;
            WorldPacket p(packet);
            p.rpos(0);
            Writer w;
            std::vector<Id128> to;
            Group* group = local->GetGroup();
            auto members = [&] { return Members(local); };
            switch (packet.GetOpcode())
            {
                case CMSG_GROUP_INVITE:
                {
                    std::string name;
                    p >> name;
                    ObjectGuid g = sCharacterCache->GetCharacterGuidByName(name);
                    Id128 target = IdOfGuid(g);
                    if (target == Id128{})
                        return false;
                    PeerInfo* info = service.FindPeer(target);
                    if (info && !(info->presence.party == Id128{}) && info->presence.party != CurrentParty(local))
                    {
                        // the target is in a party of its own (known from its presence): refused at once with the game's own message
                        local->GetSession()->SendPartyResult(PARTY_OP_INVITE, name, ERR_ALREADY_IN_GROUP_S);
                        return true;
                    }
                    if (!group && _pendingParty == Id128{})
                        _pendingParty = crypto::RandomId();
                    Id128 party = group ? CurrentParty(local) : _pendingParty;
                    if (party == Id128{})
                        party = _pendingParty = _pendingParty == Id128{} ? crypto::RandomId() : _pendingParty;
                    Id128 inviteId = crypto::RandomId();
                    _invites[target] = Invitation{inviteId, service.NetNow(), false};
                    w.U8(Invite).Id(party).Id(target).U32(0).U32(0).U8(0).Id(inviteId);
                    to = {target};
                    break;
                }
                case CMSG_GROUP_ACCEPT:
                    if (_inviter == Id128{})
                        return false;
                    w.U8(Accept).Id(_incomingParty).Id(Id128{}).U32(0).U32(0).U8(0);
                    to = {_inviter};
                    if (group)
                        for (Id128 const& m : members()) to.push_back(m);
                    break;
                case CMSG_GROUP_DECLINE:
                    if (_inviter == Id128{})
                        return false;
                    w.U8(Decline).Id(Id128{}).Id(Id128{}).U32(0).U32(0).U8(0);
                    to = {_inviter};
                    _inviter = Id128{};
                    break;
                case CMSG_GROUP_DISBAND:
                    if (!group) return false;
                    w.U8(Leave).Id(Id128{}).Id(Id128{}).U32(0).U32(0).U8(0);
                    to = members();
                    break;
                case CMSG_GROUP_UNINVITE:
                case CMSG_GROUP_UNINVITE_GUID:
                {
                    ObjectGuid g;
                    if (packet.GetOpcode() == CMSG_GROUP_UNINVITE)
                    {
                        std::string name;
                        p >> name;
                        g = sCharacterCache->GetCharacterGuidByName(name);
                    }
                    else
                        p >> g;
                    Id128 target = IdOfGuid(g);
                    if (!group || target == Id128{} || !group->IsLeader(local->GetGUID()))
                        return false;
                    w.U8(Kick).Id(Id128{}).Id(target).U32(0).U32(0).U8(0);
                    to = members();
                    break;
                }
                case CMSG_GROUP_SET_LEADER:
                {
                    ObjectGuid g;
                    p >> g;
                    Id128 target = IdOfGuid(g);
                    if (!group || target == Id128{} || !group->IsLeader(local->GetGUID()))
                        return false;
                    w.U8(SetLeader).Id(Id128{}).Id(target).U32(0).U32(0).U8(0);
                    to = members();
                    break;
                }
                case CMSG_LOOT_METHOD:
                {
                    uint32 method, threshold;
                    ObjectGuid master;
                    p >> method >> master >> threshold;
                    if (!group || !group->IsLeader(local->GetGUID()))
                        return false;
                    w.U8(Loot).Id(Id128{}).Id(IdOfGuid(master)).U32(method).U32(threshold).U8(0);
                    to = members();
                    break;
                }
                case CMSG_GROUP_RAID_CONVERT:
                    if (!group || !group->IsLeader(local->GetGUID())) return false;
                    w.U8(Raid).Id(Id128{}).Id(Id128{}).U32(0).U32(0).U8(0);
                    to = members();
                    break;
                case CMSG_GROUP_ASSISTANT_LEADER:
                {
                    ObjectGuid g;
                    bool apply;
                    p >> g >> apply;
                    Id128 target = IdOfGuid(g);
                    if (!group || target == Id128{} || !group->IsLeader(local->GetGUID()))
                        return false;
                    w.U8(Assist).Id(Id128{}).Id(target).U32(0).U32(0).U8(apply ? 1 : 0);
                    to = members();
                    break;
                }
                case CMSG_GROUP_CHANGE_SUB_GROUP:
                {
                    std::string name;
                    uint8 sub;
                    p >> name >> sub;
                    Id128 target = IdOfGuid(sCharacterCache->GetCharacterGuidByName(name));
                    if (!group || target == Id128{})
                        return false;
                    w.U8(Sub).Id(Id128{}).Id(target).U32(sub).U32(0).U8(0);
                    to = members();
                    break;
                }
                case MSG_RAID_TARGET_UPDATE:
                {
                    // the local player marks (or clears) a target: the other members' servers do the same on their copy of the group; the core does it here
                    uint8 icon;
                    p >> icon;
                    if (!group || icon >= TARGETICONCOUNT || p.size() - p.rpos() < 8)
                        return false;
                    ObjectGuid target;
                    p >> target;
                    w.U8(IconSet).Id(Id128{}).Id(Id128{}).U32(icon).U32(0).U8(0);
                    WriteIcon(w, target);
                    to = members();
                    break;
                }
                case CMSG_REQUEST_PARTY_MEMBER_STATS:
                {
                    // a member far away has no Player here: the core would answer "offline"; the last state its server told is shown instead
                    ObjectGuid g;
                    p >> g;
                    Id128 id = IdOfGuid(g);
                    MemberState state;
                    if (id != Id128{} && !Mirrors::Get().MirrorOf(id) && StateOf(id, state))
                    {
                        ShowMember(id, state, true, local);
                        return true;
                    }
                    return false;
                }
                default:
                    return false;
            }
            for (Id128 const& id : to)
                service.Say(id, game::MsgParty, w.data);
            return false;
        }

        // the local player leaves the group other ways (the core removed it): tell the others
        void LocalLeft(Group* group, ObjectGuid guid)
        {
            Service& service = Service::Get();
            Player* local = service.Local();
            if (!local || guid != local->GetGUID() || !group)
                return;
            Writer w;
            w.U8(Leave).Id(Id128{}).Id(Id128{}).U32(0).U32(0).U8(0);
            for (auto const& slot : group->GetMemberSlots())
            {
                Id128 id = IdOfGuid(slot.guid);
                if (!(id == Id128{}) && id != service.Me().id)
                    service.Say(id, game::MsgParty, w.data);
            }
        }

        // ------------------------------------------------------------ what the others do
        void OnEvent(InEvent const& e, Player* local) override
        {
            if (e.kind == InEvent::InTouch && local)
            {
                // a member of the party comes online or goes offline: the party frame says so, once
                if (Group* g = local->GetGroup())
                    if (ObjectGuid m = GuidOfId(e.peer); m && g->IsMember(m))
                    {
                        MemberState state;
                        if (!e.flag)
                            ShowMember(e.peer, MemberState(), false, local);
                        else if (StateOf(e.peer, state))
                            ShowMember(e.peer, state, true, local);
                    }
                return;
            }
            if (e.kind == InEvent::Gone || e.kind == InEvent::LinkDown)
            {
                _invites.erase(e.peer);
                return;
            }
            if (e.kind != InEvent::Message || e.type != game::MsgParty || !local || !local->IsInWorld())
                return;
            Reader r(e.data);
            Kind kind = static_cast<Kind>(r.U8());
            if (kind == RecordMsg)
            {
                PartyRecord rec;
                if (PartyRecord::Decode(r, rec))
                    ApplyRecord(e.peer, rec, local);
                return;
            }
            if (kind == MemberStateMsg)
            {
                MemberState s;
                if (!MemberState::Decode(r, s))
                    return;
                Group* g = local->GetGroup();
                ObjectGuid m = GuidOfId(e.peer);
                if (!g || !m || !g->IsMember(m) || CurrentParty(local) != s.party)
                    return;
                _states[e.peer] = s;
                PeerInfo* info = Service::Get().FindPeer(e.peer);
                ShowMember(e.peer, s, info && info->inTouch, local);
                return;
            }
            Id128 party = r.Id(), target = r.Id();
            uint32_t a = r.U32(), b = r.U32();
            uint8_t flag = r.U8();
            if (!r.ok())
                return;
            Player* actor = Mirrors::Get().MirrorOf(e.peer);
            ObjectGuid actorGuid = GuidOfId(e.peer);
            Group* group = local->GetGroup();
            switch (kind)
            {
                case Invite:
                    if (target == Service::Get().Me().id && !actor)
                        LOG_INFO("module", "[P2P] {} invites this player to a party, but its mirror is not here: dropped", Service::Get().NameOf(e.peer));
                    if (target == Service::Get().Me().id && actor)
                    {
                        Id128 inviteId = r.Id();
                        LOG_INFO("module", "[P2P] {} invites this player to a party", Service::Get().NameOf(e.peer));
                        // as if the inviter's client had asked: the core makes the group and asks the local player
                        _inviter = e.peer;
                        _incomingParty = party;
                        WorldPacket data(CMSG_GROUP_INVITE, 16);
                        data << local->GetName() << uint32(0);
                        actor->GetSession()->HandleGroupInviteOpcode(data);
                        Id128 result{};
                        result[0] = local->GetGroupInvite() ? Shown : (local->GetGroup() ? Refused : Busy);
                        LOG_INFO("module", "[P2P] The invitation of {} is {}", Service::Get().NameOf(e.peer), result[0] == Shown ? "shown to the player" : result[0] == Refused ? "refused (already in a group)" : "not shown (busy)");
                        Writer w;
                        w.U8(InviteReply).Id(inviteId).Id(result).U32(0).U32(0).U8(0);
                        Service::Get().Say(e.peer, game::MsgParty, w.data);
                        if (!local->GetGroupInvite())
                        {
                            _inviter = Id128{};
                            _incomingParty = Id128{};
                        }
                    }
                    break;
                case InviteReply:
                {
                    // 'party' holds the invitation's identifier here, 'target' the result in its first byte
                    auto it = _invites.find(e.peer);
                    if (it == _invites.end() || !(it->second.id == party))
                        break;
                    uint8_t result = target[0];
                    if (result == Shown)
                        it->second.shown = true;
                    else
                    {
                        if (actor && actor->GetGroupInvite())
                        {
                            WorldPacket data(CMSG_GROUP_DECLINE, 0);
                            actor->GetSession()->HandleGroupDeclineOpcode(data);
                        }
                        local->GetSession()->SendPartyResult(PARTY_OP_INVITE, [&e] { KnownCharacter* k = Characters::Get().Meet(e.peer); return k ? k->localName : Service::Get().NameOf(e.peer); }(), result == Busy ? ERR_BAD_PLAYER_NAME_S : ERR_ALREADY_IN_GROUP_S);
                        _invites.erase(it);
                    }
                    break;
                }
                case Accept:
                    _invites.erase(e.peer);
                    if (actor)
                    {
                        if (actor->GetGroupInvite())
                        {
                            WorldPacket data(CMSG_GROUP_ACCEPT, 4);
                            data << uint32(0);
                            actor->GetSession()->HandleGroupAcceptOpcode(data);
                            if (group = local->GetGroup())
                                if (!(party == Id128{}))
                                    Adopt(static_cast<uint32_t>(group->GetGUID().GetCounter()), CurrentParty(local) == Id128{} ? _pendingParty : CurrentParty(local));
                        }
                        // (another member's server: the newcomer joins its copy of the group when the leader's record says so, not on the news of an invitation that was
                        // accepted: a character outside the party is never shown inside it)
                    }
                    break;
                case Decline:
                    _invites.erase(e.peer);
                    if (actor && actor->GetGroupInvite())
                    {
                        WorldPacket data(CMSG_GROUP_DECLINE, 0);
                        actor->GetSession()->HandleGroupDeclineOpcode(data);
                    }
                    break;
                case Leave:
                    if (group && actorGuid && group->IsMember(actorGuid))
                        group->RemoveMember(actorGuid, GROUP_REMOVEMETHOD_LEAVE);
                    break;
                case Kick:
                case SetLeader:
                case Loot:
                case Raid:
                case Assist:
                case Sub:
                {
                    if (!group || !actorGuid || !group->IsLeader(actorGuid))
                        break;   // (only the leader may: it is checked on this copy of the group as well)
                    ObjectGuid t = GuidOfId(target);
                    if (kind == Kick && t && group->IsMember(t))
                        group->RemoveMember(t, GROUP_REMOVEMETHOD_KICK, actorGuid);
                    else if (kind == SetLeader && t && group->IsMember(t))
                    {
                        group->ChangeLeader(t);
                        group->SendUpdate();
                    }
                    else if (kind == Loot)
                    {
                        group->SetLootMethod(LootMethod(a));
                        group->SetLooterGuid(group->GetLooterGuid());
                        group->SetMasterLooterGuid(t);
                        group->SetLootThreshold(ItemQualities(b));
                        group->SendUpdate();
                    }
                    else if (kind == Raid && !group->isRaidGroup())
                        group->ConvertToRaid();
                    else if (kind == Assist && t && group->IsMember(t))
                    {
                        group->SetGroupMemberFlag(t, flag != 0, MEMBER_FLAG_ASSISTANT);
                        group->SendUpdate();
                    }
                    else if (kind == Sub && t && group->IsMember(t))
                        group->ChangeMembersGroup(t, static_cast<uint8>(a));
                    break;
                }
                case IconSet:
                    if (group && actorGuid && group->IsMember(actorGuid) && a < TARGETICONCOUNT)
                    {
                        ObjectGuid want = ReadIcon(r);
                        group->SetTargetIcon(static_cast<uint8>(a), actorGuid, want);
                    }
                    break;
                case PartyId:
                    if (group && !(party == Id128{}) && !group->isBGGroup())
                    {
                        uint32_t guid = static_cast<uint32_t>(group->GetGUID().GetCounter());
                        auto it = _ids.find(guid);
                        if (it == _ids.end() || party < it->second)
                            Adopt(guid, party);
                    }
                    else if (!group && !(party == Id128{}))
                        _incomingParty = party;
                    break;
                default:
                    break;
            }
        }

    private:
        struct Invitation { Id128 id{}; int64_t at = 0; bool shown = false; };
        std::map<uint32_t, Id128> _ids;       // local group guid -> party identifier
        std::map<Id128, Id128> _announced;
        std::map<Id128, Invitation> _invites; // by target
        std::map<Id128, MemberState> _states; // the last state of each member that told one
        std::map<Id128, int64_t> _statesSentAt;
        std::map<Id128, uint32_t> _version;
        std::map<Id128, int64_t> _outside;    // members in touch that announce another party: since when (RemoveAbsent)
        std::map<Id128, int64_t> _joinedAt;   // when this server first knew the party (a record made before the leader heard of an acceptance is waited out)
        std::map<Id128, Bytes> _lastRecord;
        Id128 _pendingParty{}, _incomingParty{}, _inviter{};
        uint32_t _timer = 0, _stateTimer = 0, _lastHealth = 0;
        int64_t _lastSentAt = 0, _lastRecordAt = 0, _leaderGoneSince = 0;
    };

    class PartyPackets : public ServerScript
    {
    public:
        PartyPackets() : ServerScript("P2PParty", { SERVERHOOK_CAN_PACKET_RECEIVE }) { }
        bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
        {
            switch (packet.GetOpcode())
            {
                case CMSG_GROUP_INVITE: case CMSG_GROUP_ACCEPT: case CMSG_GROUP_DECLINE: case CMSG_GROUP_DISBAND: case CMSG_GROUP_UNINVITE:
                case CMSG_GROUP_UNINVITE_GUID: case CMSG_GROUP_SET_LEADER: case CMSG_LOOT_METHOD: case CMSG_GROUP_RAID_CONVERT:
                case CMSG_GROUP_ASSISTANT_LEADER: case CMSG_GROUP_CHANGE_SUB_GROUP: case CMSG_REQUEST_PARTY_MEMBER_STATS: case MSG_RAID_TARGET_UPDATE:
                    return !Party::Get().LocalPacket(session, packet);
                default:
                    break;
            }
            return true;
        }
    };

    class PartyGroups : public GroupScript
    {
    public:
        PartyGroups() : GroupScript("P2PGroups", { GROUPHOOK_ON_REMOVE_MEMBER }) { }
        void OnRemoveMember(Group* group, ObjectGuid guid, RemoveMethod method, ObjectGuid kicker, char const* reason) override
        {
            if (method == GROUP_REMOVEMETHOD_LEAVE || method == GROUP_REMOVEMETHOD_DEFAULT)
                Party::Get().LocalLeft(group, guid);
        }
    };

    // the identifier of the character that leads the local player's group (zero: no group, or the leader is not known)
    Id128 PartyLeader(Player* local)
    {
        Group* g = local ? local->GetGroup() : nullptr;
        return g ? IdOfGuid(g->GetLeaderGUID()) : Id128{};
    }

    // the party identifier of the local player's group (zero: none)
    Id128 CurrentPartyId(Player* local) { return Party::Get().CurrentParty(local); }

    void AddParty()
    {
        Service::Get().AddFeature(&Party::Get());
        new PartyPackets();
        new PartyGroups();
    }
}
