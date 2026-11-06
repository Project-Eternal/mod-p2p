/*
 * The loot of a creature killed by a party whose members live on different servers. The server of the party's leader (else the server of the member who tapped
 * the creature) makes the loot with the core's own code and sends every member's server the loot record: the items with their random properties and flags,
 * the gold, the looter whose turn it is. Every server puts the record into the loot of its own copy of the corpse, and the core does the rest for its own player.
 * What changes in a corpse is announced to the party (a state message) and applied to every copy.
 *
 * Rolls run in the maker's core, with the mirrors of the members voting: the packets the core sends to a mirror are forwarded to the member's server, and the
 * member's choice comes back and is replayed through the mirror's session. An item a mirror wins is taken out of the mirror's bags and described to the member's
 * server, which makes the item itself (an award); the gold goes the same way. Two members who take one item at the same moment may both get it.
 *
 * Chests and other objects that hold loot (Objects below) work with the same record: the server of the player who opens the object makes it.
 */
#include "Characters.h"
#include "GameObject.h"
#include "Group.h"
#include "Items.h"
#include "Log.h"
#include "Map.h"
#include "Mirrors.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "P2PService.h"
#include "Player.h"
#include "Protocol.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Transfer.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "core/Authority.h"
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>

namespace p2p
{
    // Trade.cpp keeps a temporary copy of the other player's offered item in the mirror's bags: the core's hook for a new item in a mirror's bags (a won roll, a gift of the master looter) must not take it
    thread_local int g_ignoreMirrorStores = 0;

    // from World.cpp
    Creature* WorldLookup(EntityKey const& key, Player* local);
    bool WorldKeyOf(Creature const* c, EntityKey& key);
    Id128 WorldTapper(EntityKey const& key);
    // from Party.cpp
    Id128 PartyLeader(Player* local);

    namespace
    {
        enum Step : uint8_t { StepRecord = 1, StepState = 2, StepPacket = 3, StepAward = 4, StepRoll = 5, StepCannot = 6, StepOpen = 7 };
        enum StateKind : uint8_t { StItem = 0, StQuestItem = 1, StGold = 2, StRelease = 3 };
        enum AwardKind : uint8_t { AwardItem = 1, AwardMoney = 2 };
        // a guid on the wire: none, a creature (as its key), a player (as its character)
        enum GuidKind : uint8_t { GuidNone = 0, GuidCreature = 1, GuidPlayer = 2 };
        // the flags of a loot item in the record
        enum ItemFlags : uint8_t { ItLooted = 1, ItBlocked = 2, ItFreeForAll = 4, ItUnderThreshold = 8, ItNeedsQuest = 16, ItFollowRules = 32, ItCounted = 64 };

        constexpr int64_t StandInMs = 3000;      // the tapper's server makes the loot if the leader's has sent no record by then
        constexpr int64_t RepeatMs = 10000;      // the maker tells the record again (a member that came later, a message that was lost)
        constexpr int64_t PendingMs = 30000;     // a record that arrived before the death it belongs to is kept this long
        constexpr uint32 PollMs = 250;
        constexpr int64_t HoldMs = 1500;        // a member opens a chest only after the leader's server has answered, or this long
        constexpr int64_t CannotMs = 30000;      // the leader's "I cannot make the loot" is kept this long for a death that has not reached the looter's server

        thread_local bool g_lootMoney = false;   // a player is taking the gold of a corpse (the world thread, until the next update of the feature): what the core pays to a mirror is the member's

        // the layout of the packets that the core sends to a mirror and the member must see: G a creature's guid, g a player's, R a guid that is kept as it is, 4 and 1 numbers, * the rest,
        // M a count and that many players' guids. They are rewritten with the guids made as the member's server knows them.
        char const* LayoutOf(uint16_t opcode)
        {
            switch (opcode)
            {
                case SMSG_LOOT_MONEY_NOTIFY:     return "*";
                case SMSG_LOOT_CLEAR_MONEY:      return "*";
                // (the first guid of a roll is the guid of the item being rolled for: it means something to the maker only, which is given it back)
                case SMSG_LOOT_START_ROLL:       return "R44444441";
                case SMSG_LOOT_ROLL:             return "R4g444111";
                case SMSG_LOOT_ROLL_WON:         return "R4444g11";
                case SMSG_LOOT_ALL_PASSED:       return "R4444";
                // "X receives loot": the core broadcasts the take of an item to the group (the mirrors of the others), the player's guid first
                case SMSG_ITEM_PUSH_RESULT:      return "g*";
                default:                         return nullptr;
            }
        }

        // the identifier of a character as the servers know it, from a guid of this server (zero: not a player of the shared world)
        Id128 IdOf(ObjectGuid guid, Player* local)
        {
            if (guid.IsEmpty())
                return Id128{};
            if (guid == local->GetGUID())
                return Service::Get().Me().id;
            return Mirrors::Get().PeerOf(guid);
        }

        ObjectGuid GuidOf(Id128 const& id, Player* local)
        {
            if (id == Id128{})
                return ObjectGuid::Empty;
            if (id == Service::Get().Me().id)
                return local->GetGUID();
            Player* mirror = Mirrors::Get().MirrorOf(id);
            return mirror ? mirror->GetGUID() : ObjectGuid::Empty;
        }

        uint8_t FlagsOf(LootItem const& i)
        {
            return (i.is_looted ? ItLooted : 0) | (i.is_blocked ? ItBlocked : 0) | (i.freeforall ? ItFreeForAll : 0) | (i.is_underthreshold ? ItUnderThreshold : 0) |
                (i.needs_quest ? ItNeedsQuest : 0) | (i.follow_loot_rules ? ItFollowRules : 0) | (i.is_counted ? ItCounted : 0);
        }

        // what travels of a loot item
        void WriteItem(Writer& w, LootItem const& i)
        {
            w.U32(i.itemid).U32(i.count).U32(i.randomSuffix).U32(static_cast<uint32_t>(i.randomPropertyId)).U8(FlagsOf(i)).U8(i.groupid);
        }

        LootItem ReadItem(Reader& r, uint32 index)
        {
            LootItem i;
            i.itemid = r.U32();
            i.count = r.U32();
            i.itemIndex = index;
            i.randomSuffix = r.U32();
            i.randomPropertyId = static_cast<int32>(r.U32());
            uint8_t flags = r.U8();
            i.groupid = r.U8() & 0x7F;
            i.is_looted = flags & ItLooted;
            i.is_blocked = flags & ItBlocked;
            i.freeforall = flags & ItFreeForAll;
            i.is_underthreshold = flags & ItUnderThreshold;
            i.needs_quest = flags & ItNeedsQuest;
            i.follow_loot_rules = flags & ItFollowRules;
            i.is_counted = flags & ItCounted;
            return i;
        }

        // The slots of a corpse that are the same for every member (a free for all item and a quest item of one member are not): their state is announced.
        bool Shared(LootItem const& i, bool quest) { return !i.freeforall && (!quest || i.follow_loot_rules); }
        uint8_t StateBits(LootItem const& i) { return (i.is_looted ? ItLooted : 0) | (i.is_blocked ? ItBlocked : 0) | (i.is_underthreshold ? ItUnderThreshold : 0); }

        // Items that every player loots for themselves (the core's free for all items) are never in the record: each server takes them out of the loot it
        // has and makes its own player's, from the loot table, behind the shared ones (so the indexes of the shared items are the same everywhere).
        size_t SharedCount(std::vector<LootItem> const& list)
        {
            size_t n = 0;
            for (LootItem const& i : list)
                if (!i.freeforall)
                    ++n;
            return n;
        }

        void StripOwn(::Loot& loot)
        {
            // (the core counted each free for all item of the local player once, and keeps per-player lists of slot numbers that the erasing makes wrong: both are made again
            // by the core's own FillNotNormalLootFor once AddOwn has put the items back)
            uint32 own = 0;
            for (std::vector<LootItem>* list : { &loot.items, &loot.quest_items })
                for (LootItem const& i : *list)
                    if (i.freeforall && !i.is_looted)
                        ++own;
            loot.unlootedCount = static_cast<uint8>(loot.unlootedCount > own ? loot.unlootedCount - own : 0);
            for (QuestItemMap const* map : { &loot.GetPlayerQuestItems(), &loot.GetPlayerFFAItems(), &loot.GetPlayerNonQuestNonFFAConditionalItems() })
            {
                QuestItemMap& m = const_cast<QuestItemMap&>(*map);
                for (auto& entry : m)
                    delete entry.second;
                m.clear();
            }
            for (std::vector<LootItem>* list : { &loot.items, &loot.quest_items })
            {
                list->erase(std::remove_if(list->begin(), list->end(), [](LootItem const& i) { return i.freeforall; }), list->end());
                for (size_t i = 0; i < list->size(); ++i)
                    (*list)[i].itemIndex = uint32(i);
            }
        }

        void AddOwn(::Loot& loot, WorldObject* source, Player* local)
        {
            LootStore const* store = nullptr;
            uint32 lootId = 0;
            uint16 mode = LOOT_MODE_DEFAULT;
            if (Creature* c = source->ToCreature())
            {
                store = &LootTemplates_Creature;
                lootId = c->GetCreatureTemplate()->lootid;
                mode = c->GetLootMode();
            }
            else if (GameObject* go = source->ToGameObject())
            {
                store = &LootTemplates_Gameobject;
                lootId = go->GetGOInfo()->GetLootId();
            }
            if (!store || !lootId)
                return;
            ::Loot own;
            own.FillLoot(lootId, *store, local, false, true, mode, source);
            for (std::vector<LootItem>* pair : { &own.items, &own.quest_items })
            {
                std::vector<LootItem>& to = pair == &own.items ? loot.items : loot.quest_items;
                for (LootItem& i : *pair)
                    if (i.freeforall && to.size() < 255)
                    {
                        i.itemIndex = uint32(to.size());
                        to.push_back(i);
                    }
            }
        }

        // one corpse (or object) whose loot is shared
        struct Shared_
        {
            ObjectGuid guid;
            Id128 party{};
            Id128 makerId{};                         // the server whose record this is (zero: this server made it, or none yet)
            bool object = false;                     // a chest or another game object, not a corpse
            bool maker = false;                      // this server made the loot
            bool native = false;                     // the loot was made by the core here and is held back until the leader's record comes or the stand-in time has passed
            int64_t standIn = 0;                     // network time at which the tapper's server makes the loot itself (0: not waiting)
            uint32_t version = 0;
            int64_t told = 0;                        // the maker told the record at this network time
            bool goldGone = false, hadLooter = false;
            std::vector<uint8_t> items, quest;       // the state of every slot as last announced or applied (StateBits)
        };

        struct Pending
        {
            Id128 from{};
            Bytes data;
            int64_t at = 0;
        };

        class Loot : public Feature
        {
        public:
            static Loot& Get()
            {
                static Loot loot;
                return loot;
            }

            void OnLogin(Player*) override { Reset(); }
            void OnLogout(Player*) override { Reset(); }

            // ------------------------------------------------------------ a kill (World.cpp)
            // The creature of 'key' died and the core has filled its loot ('native') for the party of the local player, whose servers must all see the same loot.
            // 'tapperHere': this server's player tapped it.
            void Killed(EntityKey const& key, ObjectGuid guid, bool native, bool tapperHere)
            {
                Player* local = Service::Get().Local();
                Id128 party = Service::Get().Presence().party;
                if (!local || party == Id128{})
                    return;   // (alone, or not in a party: the core's own loot, for the tapper only)
                Creature* c = local->IsInWorld() ? local->GetMap()->GetCreature(guid) : nullptr;
                if (!c)
                    return;
                Shared_& s = _corpses[key];
                s = Shared_{};
                s.guid = guid;
                s.party = party;
                bool leader = PartyLeader(local) == Service::Get().Me().id;
                if (leader && native)
                {
                    LOG_INFO("module", "[P2P] The loot of creature {} is made by the leader's server", key.spawn);
                    Make(key, s, c, local);
                    return;
                }
                if (leader)
                    Cannot(key, party);   // (the looter's server need not wait)
                if (native && tapperHere)
                {
                    // the loot is held back: the leader's server makes the party's, and this server stands in for it if it does not
                    c->RemoveDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
                    s.native = true;
                    s.standIn = Service::Get().NetNow() + StandInMs;
                    if (_cannot.erase(key))
                        s.standIn = Service::Get().NetNow();
                }
                auto pending = _pending.find(key);
                if (pending != _pending.end())
                {
                    Pending p = std::move(pending->second);
                    _pending.erase(pending);
                    OnRecord(p.from, p.data, local);
                }
            }

            // A creature that a state showed dead before its death arrived (so the core never ran its kill here): the leader's server makes the loot the core would have,
            // for the tapper's party, unless it was made already. (The turn of the looters of a round robin moves on as for any kill.)
            void KilledLate(EntityKey const& key, ObjectGuid guid, Id128 const& tapper)
            {
                Player* local = Service::Get().Local();
                Id128 party = Service::Get().Presence().party;
                Creature* c = local && local->IsInWorld() ? local->GetMap()->GetCreature(guid) : nullptr;
                if (!c || party == Id128{} || PartyLeader(local) != Service::Get().Me().id)
                    return;
                auto known = _corpses.find(key);
                if (known != _corpses.end() && known->second.version)
                    return;
                Player* mirror = Mirrors::Get().MirrorOf(tapper);
                Player* looter = mirror ? mirror : local;
                if (!local->IsAlive() || !local->IsAtGroupRewardDistance(c) || !c->GetCreatureTemplate()->lootid)
                {
                    Cannot(key, party);
                    return;
                }
                // as the core's kill does it, in the same order: the turn of the looters is moved on (a late kill moves it like any other), the looter whose
                // turn it is gets the loot, and after the loot is made the turn is advanced if there was any
                Group* group = local->GetGroup();
                bool hasLooter = false;
                if (group)
                {
                    group->UpdateLooterGuid(c, true);
                    if (group->GetLooterGuid() && group->GetLootMethod() != FREE_FOR_ALL)
                        if (Player* turn = ObjectAccessor::FindPlayer(group->GetLooterGuid()))
                        {
                            looter = turn;
                            hasLooter = true;
                            c->SetLootRecipient(turn);
                        }
                }
                c->loot.clear();
                c->loot.FillLoot(c->GetCreatureTemplate()->lootid, LootTemplates_Creature, looter, false, false, c->GetLootMode(), c);
                if (c->GetLootMode())
                    c->loot.generateMoneyLoot(c->GetCreatureTemplate()->mingold, c->GetCreatureTemplate()->maxgold);
                if (group)
                {
                    group->SendLooter(c, hasLooter ? looter : nullptr);
                    if (!c->loot.empty())
                        group->UpdateLooterGuid(c);
                }
                Shared_& s = _corpses[key];
                s = Shared_{};
                s.guid = guid;
                s.party = party;
                LOG_INFO("module", "[P2P] The loot of creature {} is made by the leader's server (the death was seen as a state first)", key.spawn);
                Make(key, s, c, local);
            }

            // ------------------------------------------------------------ the world thread, every update
            void Update(uint32_t diff, Player* local) override
            {
                g_lootMoney = false;
                _timer += diff;
                if (_timer < PollMs)
                    return;
                _timer = 0;
                if (!local || !local->IsInWorld())
                    return;
                int64_t now = Service::Get().NetNow();
                {
                    std::lock_guard<std::mutex> lock(_heldLock);
                    if (_held && now >= _held->deadline)
                        ReleaseHeldLocked(local);
                }
                for (auto it = _cannot.begin(); it != _cannot.end();)
                    it = now - it->second > CannotMs ? _cannot.erase(it) : std::next(it);
                for (auto it = _pending.begin(); it != _pending.end();)
                {
                    EntityKey key = it->first;
                    Creature* c = WorldLookup(key, local);
                    if (c && c->isDead())
                    {
                        Pending p = std::move(it->second);
                        it = _pending.erase(it);
                        OnRecord(p.from, p.data, local);
                    }
                    else if (now - it->second.at > PendingMs)
                        it = _pending.erase(it);
                    else
                        ++it;
                }
                for (auto it = _corpses.begin(); it != _corpses.end();)
                {
                    Shared_& s = it->second;
                    WorldObject* source = s.object ? static_cast<WorldObject*>(local->GetMap()->GetGameObject(s.guid)) : local->GetMap()->GetCreature(s.guid);
                    if (!source)
                    {
                        it = _corpses.erase(it);
                        continue;
                    }
                    Creature* c = source->ToCreature();
                    if (c && s.standIn && now >= s.standIn)
                    {
                        // no record came from the leader's server: this one makes the party's loot, from the loot the core made here
                        s.standIn = 0;
                        if (c->isDead() && !c->loot.isLooted())
                        {
                            LOG_INFO("module", "[P2P] The loot of creature {} is made here, the leader's server sent none", it->first.spawn);
                            Make(it->first, s, c, local);
                        }
                    }
                    if (s.version && !Watch(it->first, s, source, local, now))
                    {
                        it = _corpses.erase(it);
                        continue;
                    }
                    ++it;
                }
            }

            // a message of another server
            void OnEvent(InEvent const& e, Player* local) override
            {
                if (e.kind != InEvent::Message || e.type != game::MsgLoot || !local)
                    return;
                if (!InMyParty(e.peer))
                    return;
                Reader r(e.data);
                uint8_t step = r.U8();
                switch (step)
                {
                    case StepRecord:
                    {
                        OnRecord(e.peer, e.data, local);
                        Reader k(e.data);
                        k.U8();
                        EntityKey key = EntityKey::Read(k);
                        if (k.ok())
                            ReleaseHeld(key, local);
                        break;
                    }
                    case StepState: OnState(e.peer, r, local); break;
                    case StepPacket: OnPacket(e.peer, r, local); break;
                    case StepAward: OnAward(e.peer, r, local); break;
                    case StepRoll: OnRoll(e.peer, r); break;
                    case StepCannot: OnCannot(e.peer, r, local); break;
                    case StepOpen: OnOpen(e.peer, r, local); break;
                    default: break;
                }
            }

            // ------------------------------------------------------------ the maker: what the core sends to a mirror
            // any thread: a packet to a session without a connection; if it is a mirror's and one of the loot's, the member gets it
            void MirrorPacket(WorldSession* session, WorldPacket const& packet)
            {
                char const* layout = LayoutOf(packet.GetOpcode());
                Player* mirror = session->GetPlayer();
                if (!layout || !mirror || !mirror->IsMirror())
                    return;
                ObjectGuid mirrorGuid = mirror->GetGUID();
                uint16_t opcode = packet.GetOpcode();
                Bytes body(packet.contents(), packet.contents() + packet.size());
                Service::Get().Defer([this, mirrorGuid, opcode, body]
                {
                    Id128 peer = Mirrors::Get().PeerOf(mirrorGuid);
                    Player* local = Service::Get().Local();
                    if (peer == Id128{} || !local)
                        return;
                    Writer w;
                    w.U8(StepPacket).U16(opcode);
                    if (!Encode(opcode, body, local, w))
                        return;
                    Service::Get().Say(peer, game::MsgLoot, w.data);
                });
            }

            // any thread: the core stored a new item in a mirror's bags (a won roll, the master looter's gift): it is the member's, who makes it from its description
            void MirrorStored(Player* mirror, Item* item, uint32 count)
            {
                ObjectGuid mirrorGuid = mirror->GetGUID(), itemGuid = item->GetGUID();
                Service::Get().Defer([this, mirrorGuid, itemGuid]
                {
                    Player* local = Service::Get().Local();
                    Id128 peer = Mirrors::Get().PeerOf(mirrorGuid);
                    Player* mirror = Mirrors::Get().MirrorOf(peer);
                    Item* item = mirror ? mirror->GetItemByGuid(itemGuid) : nullptr;
                    if (!local || !item)
                        return;
                    Writer w;
                    w.U8(StepAward).U8(AwardItem);
                    Write(w, Describe(item));
                    mirror->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
                    Service::Get().Say(peer, game::MsgLoot, w.data);
                });
            }

            // the gold of a corpse is being taken (by the local player: the core pays the members near it)
            static void TakingGold() { g_lootMoney = true; }

            // any thread: the core pays money to a mirror (the member's share of the gold): it goes to the member's server, which pays its own player
            bool MirrorPaid(Player* mirror, int32 amount)
            {
                if (!g_lootMoney || amount <= 0)
                    return false;
                ObjectGuid mirrorGuid = mirror->GetGUID();
                Service::Get().Defer([this, mirrorGuid, amount]
                {
                    Id128 peer = Mirrors::Get().PeerOf(mirrorGuid);
                    if (peer == Id128{})
                        return;
                    Writer w;
                    w.U8(StepAward).U8(AwardMoney).U32(static_cast<uint32_t>(amount));
                    Service::Get().Say(peer, game::MsgLoot, w.data);
                });
                return true;
            }

            // ------------------------------------------------------------ the member: its roll
            // true: the packet is handled here and must not reach the core
            bool LocalPacket(WorldSession* session, WorldPacket const& packet)
            {
                Service& service = Service::Get();
                Player* local = service.Local();
                if (!service.Enabled() || !local || session != local->GetSession() || packet.GetOpcode() != CMSG_LOOT_ROLL)
                    return false;
                // (a roll is answered without a window: the item is the maker's, by the guid it gave)
                WorldPacket p(packet);
                p.rpos(0);
                ObjectGuid guid;
                uint32 slot;
                uint8 type;
                p >> guid >> slot >> type;
                auto roll = _rolls.find(guid.GetRawValue());
                if (roll == _rolls.end())
                    return false;
                Writer rw;
                rw.U8(StepRoll).U64(guid.GetRawValue()).U32(slot).U8(type);
                service.Say(roll->second, game::MsgLoot, rw.data);
                return true;
            }

            // the core's own check of the opening spell on this chest, as it would run it when the cast is made: false when it would fail
            static bool OpeningAllowed(Player* local, GameObject* go, uint32 spellId)
            {
                SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
                if (!info)
                    return false;
                Spell* spell = new Spell(local, info, TRIGGERED_NONE);
                spell->m_targets.SetGOTarget(go);
                SpellCastResult result = spell->CheckCast(true);
                delete spell;
                return result == SPELL_CAST_OK;
            }

            // A member opens a chest that follows the party's rules while the leader is on another server: the leader's server makes the loot if its world has the chest
            // (the member's server holds the opening back until the record or "cannot" comes, or HoldMs have passed). true: the packet is held and must not reach the core.
            bool HoldUse(WorldSession* session, WorldPacket const& packet)
            {
                Service& service = Service::Get();
                Player* local = service.Local();
                bool cast = packet.GetOpcode() == CMSG_CAST_SPELL;
                if ((packet.GetOpcode() != CMSG_GAMEOBJ_USE && !cast) || !service.Enabled() || !local || session != local->GetSession())
                    return false;
                // (the client opens a chest by casting its opening spell at it; the packet of the use is only sent for other objects)
                ObjectGuid target;
                uint32 spellOf = 0;
                if (cast)
                {
                    WorldPacket p(packet);
                    p.rpos(0);
                    if (p.size() < 11)
                        return false;
                    uint8 count, flags;
                    uint32 spellId, mask;
                    p >> count >> spellId >> flags >> mask;
                    spellOf = spellId;
                    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
                    if (!info || !info->HasEffect(SPELL_EFFECT_OPEN_LOCK) || !(mask & TARGET_FLAG_GAMEOBJECT))
                        return false;
                    try
                    {
                        p >> target.ReadAsPacked();
                    }
                    catch (ByteBufferException const&)
                    {
                        return false;
                    }
                    if (!target.IsGameObject())
                        return false;
                }
                {
                    // (what the real game sends to open an object, whoever is asked about it afterwards)
                    ObjectGuid seen = target;
                    if (!cast && packet.size() >= 8)
                    {
                        WorldPacket p0(packet);
                        p0.rpos(0);
                        p0 >> seen;
                    }
                    if (GameObject* seenGo = seen.IsGameObject() ? local->GetMap()->GetGameObject(seen) : nullptr)
                        LOG_INFO("module", "[P2P] The game sent {} to open the object {} (entry {})", cast ? "CMSG_CAST_SPELL (spell " + std::to_string(spellOf) + ")" : std::string("CMSG_GAMEOBJ_USE"), seen.GetCounter(), seenGo->GetEntry());
                }
                if (_replaying.exchange(false))
                    return false;
                Id128 leader = PartyLeader(local);
                PeerInfo* lp = service.FindPeer(leader);
                if (service.Presence().party == Id128{} || leader == Id128{} || leader == service.Me().id || !lp || !lp->linked || !InMyParty(leader) || !local->IsInWorld())
                    return false;
                ObjectGuid guid = target;
                if (!cast)
                {
                    WorldPacket p(packet);
                    p.rpos(0);
                    p >> guid;
                }
                GameObject* go = guid.IsGameObject() ? local->GetMap()->GetGameObject(guid) : nullptr;
                if (!go || !go->GetSpawnId() || go->GetGoType() != GAMEOBJECT_TYPE_CHEST || !go->GetGOInfo()->chest.groupLootRules || go->getLootState() != GO_READY || !go->isSpawned())
                    return false;
                // The game's own check of the opening comes first: the lock, the key, the skill, the distance. An opening that fails is let through and fails as on a
                // normal server; no loot is made for the party. (An opening by the use of the object, without a spell, is checked for the distance and the player's state only.)
                if (cast)
                {
                    if (!OpeningAllowed(local, go, spellOf))
                        return false;
                }
                else if (!local->IsAlive() || !go->IsWithinDistInMap(local, INTERACTION_DISTANCE))
                    return false;
                std::lock_guard<std::mutex> lock(_heldLock);
                if (_held)
                    return true;   // (the click is repeated: one is enough)
                Held held;
                held.key = ObjectKey(go);
                held.packet = std::make_unique<WorldPacket>(packet);
                held.packet->rpos(0);
                held.deadline = service.NetNow() + HoldMs;
                Writer w;
                w.U8(StepOpen);
                held.key.Write(w);
                _held = std::move(held);
                service.Defer([leader, data = w.data] { Service::Get().Say(leader, game::MsgLoot, data); });
                return true;
            }

            // the item or the gold that was awarded to the local player
            void Deliver(Id128 const& from, OfferContent const& loot, Player* local);

            // ------------------------------------------------------------ objects that hold loot (chests, gathered objects)
            // The local player opened an object whose loot the core has just made. If the player is in a party and the object follows the party's rules, the loot is the
            // party's: this server is its maker. Called from the hook of the core's own loot window.
            void ObjectOpened(GameObject* go, Player* player)
            {
                Player* local = Service::Get().Local();
                Id128 party = Service::Get().Presence().party;
                if (!local || player != local || party == Id128{} || !go->GetSpawnId() || go->loot.loot_type == LOOT_NONE || go->GetGoType() != GAMEOBJECT_TYPE_CHEST ||
                    !go->GetGOInfo()->chest.groupLootRules)
                    return;   // (herbs, ore and fishing give their loot to the gatherer alone; a chest without the party's rules gives each opener its own)
                EntityKey key = ObjectKey(go);
                Shared_& s = _corpses[key];
                if (s.version)
                    return;   // (already shared: the record came from the server that opened it first)
                s = Shared_{};
                s.guid = go->GetGUID();
                s.party = party;
                s.object = true;
                MakeRecordFor(key, s, local, go->loot, go);
            }

        private:
            struct Held
            {
                EntityKey key;
                std::unique_ptr<WorldPacket> packet;
                int64_t deadline = 0;
            };

            static EntityKey ObjectKey(GameObject* go)
            {
                EntityKey key;
                key.kind = 2;
                key.map = go->GetMapId();
                key.instance = Service::Get().Presence().instanceKey;
                key.spawn = static_cast<uint32_t>(go->GetSpawnId());
                return key;
            }

            // the held opening goes on: it is queued again and the core handles it, with the party's record (or without) in the chest
            void ReleaseHeldLocked(Player* local)
            {
                if (!_held)
                    return;
                _replaying = true;
                std::unique_ptr<WorldPacket> packet = std::move(_held->packet);
                _held.reset();
                local->GetSession()->QueuePacket(packet.release());
            }

            void ReleaseHeld(EntityKey const& key, Player* local)
            {
                std::lock_guard<std::mutex> lock(_heldLock);
                if (_held && _held->key == key)
                    ReleaseHeldLocked(local);
            }

            // the leader's server: a member opens a chest. The loot is made here, with the core's own rules, if this world has the chest.
            void OnOpen(Id128 const& from, Reader& r, Player* local)
            {
                EntityKey key = EntityKey::Read(r);
                Id128 party = Service::Get().Presence().party;
                if (!r.ok() || key.kind != 2 || !InMyParty(from) || PartyLeader(local) != Service::Get().Me().id || !local->IsInWorld())
                    return;
                GameObject* go = nullptr;
                if (key.map == local->GetMapId())
                {
                    auto range = local->GetMap()->GetGameObjectBySpawnIdStore().equal_range(key.spawn);
                    if (range.first != range.second)
                        go = range.first->second;
                }
                auto known = _corpses.find(key);
                if (go && known != _corpses.end() && known->second.version && known->second.maker)
                {
                    SendRecord(key, known->second, local, go->loot, go, from);   // (made already: the member gets it again)
                    return;
                }
                Group* group = local->GetGroup();
                uint32 lootId = go ? go->GetGOInfo()->GetLootId() : 0;
                if (!go || !group || !lootId || go->GetGoType() != GAMEOBJECT_TYPE_CHEST || !go->GetGOInfo()->chest.groupLootRules || go->getLootState() != GO_READY || !go->isSpawned())
                {
                    Cannot(key, party, from);
                    return;
                }
                LOG_INFO("module", "[P2P] The loot of the chest {} is made by the leader's server", key.spawn);
                ::Loot& loot = go->loot;
                loot.clear();
                group->UpdateLooterGuid(go, true);
                loot.FillLoot(lootId, LootTemplates_Gameobject, local, false, false, go->GetLootMode(), go);
                go->SetLootGenerationTime();
                if (!loot.empty())
                    group->UpdateLooterGuid(go);
                if (GameObjectTemplateAddon const* addon = go->GetTemplateAddon())
                    loot.generateMoneyLoot(addon->mingold, addon->maxgold);
                PrepareRolls(group, loot, go);
                go->SetLootState(GO_ACTIVATED, local);
                loot.loot_type = LOOT_CORPSE;
                loot.sourceWorldObjectGUID = go->GetGUID();
                Shared_& s = _corpses[key];
                s = Shared_{};
                s.guid = go->GetGUID();
                s.party = party;
                s.object = true;
                MakeRecordFor(key, s, local, loot, go);
            }

            void Reset()
            {
                {
                    std::lock_guard<std::mutex> lock(_heldLock);
                    _held.reset();
                }
                _corpses.clear();
                _pending.clear();
                _rolls.clear();
                _cannot.clear();
                _timer = 0;
            }

            // the leader's server tells the party that it cannot make the loot of a corpse (it is not near, or the core made none there)
            void Cannot(EntityKey const& key, Id128 const& party, Id128 const& only = Id128{})
            {
                Writer w;
                w.U8(StepCannot);
                key.Write(w);
                if (!(only == Id128{}))
                {
                    Service::Get().Say(only, game::MsgLoot, w.data);
                    return;
                }
                Service::Get().SayAll([=](PeerInfo const& p) { return p.linked && p.presence.party == party; }, game::MsgLoot, w.data);
            }

            void OnCannot(Id128 const& from, Reader& r, Player* local)
            {
                EntityKey key = EntityKey::Read(r);
                if (!r.ok() || from != PartyLeader(local))
                    return;
                ReleaseHeld(key, local);
                auto known = _corpses.find(key);
                if (known != _corpses.end() && known->second.version)
                    return;
                if (known != _corpses.end() && known->second.standIn)
                    known->second.standIn = Service::Get().NetNow();
                else
                    _cannot[key] = Service::Get().NetNow();
            }

            bool InMyParty(Id128 const& peer) const
            {
                PeerInfo* p = Service::Get().FindPeer(peer);
                Id128 mine = Service::Get().Presence().party;
                return p && !(mine == Id128{}) && p->presence.party == mine;
            }

            // the loot of the corpse is made: the rolls and the master loot are prepared, as the core does at the first opening, and the party is told
            void Make(EntityKey const& key, Shared_& s, Creature* c, Player* local)
            {
                Group* group = local->GetGroup();
                LOG_INFO("module", "[P2P] Loot of creature {} to be shared: gold {}, {} items, {} quest items", key.spawn, c->loot.gold, c->loot.items.size(), c->loot.quest_items.size());
                PrepareRolls(group, c->loot, c);
                c->loot.loot_type = LOOT_CORPSE;
                c->loot.sourceWorldObjectGUID = c->GetGUID();
                if (!c->loot.isLooted())
                    c->SetDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
                MakeRecordFor(key, s, local, c->loot, c);
            }

            static void PrepareRolls(Group* group, ::Loot& loot, WorldObject* source)
            {
                if (!group)
                    return;
                switch (group->GetLootMethod())
                {
                    case GROUP_LOOT:         group->GroupLoot(&loot, source); break;
                    case NEED_BEFORE_GREED:  group->NeedBeforeGreed(&loot, source); break;
                    case MASTER_LOOT:        group->MasterLoot(&loot, source); break;
                    default: break;
                }
            }

            void MakeRecordFor(EntityKey const& key, Shared_& s, Player* local, ::Loot& loot, WorldObject* source)
            {
                s.maker = true;
                s.native = false;
                s.standIn = 0;
                s.version = 1;
                s.goldGone = loot.gold == 0;
                s.hadLooter = !loot.roundRobinPlayer.IsEmpty();
                StripOwn(loot);
                Snapshot(s, loot);
                SendRecord(key, s, local, loot, source);
                AddOwn(loot, source, local);
                Snapshot(s, loot);
                if (local->IsAtLootRewardDistance(source))
                    loot.FillNotNormalLootFor(local);
            }

            static void Snapshot(Shared_& s, ::Loot const& loot)
            {
                s.items.clear();
                s.quest.clear();
                for (LootItem const& i : loot.items)
                    s.items.push_back(StateBits(i));
                for (LootItem const& i : loot.quest_items)
                    s.quest.push_back(StateBits(i));
            }

            void SendRecord(EntityKey const& key, Shared_& s, Player* local, ::Loot const& loot, WorldObject* source, Id128 const& only = Id128{})
            {
                Group* group = local->GetGroup();
                Writer w;
                w.U8(StepRecord);
                key.Write(w);
                w.U32(s.version).Id(s.party).U8(group ? uint8(group->GetLootMethod()) : 0);
                w.U32(loot.gold);
                w.Id(IdOf(loot.roundRobinPlayer, local)).Id(group ? IdOf(group->GetLooterGuid(), local) : Id128{});
                w.U32(loot.unlootedCount);
                // (the items that every player loots for themselves are each server's own, not in the record)
                w.U8(static_cast<uint8_t>(std::min<size_t>(SharedCount(loot.items), 255)));
                for (size_t i = 0; i < loot.items.size() && i < 255; ++i)
                    if (!loot.items[i].freeforall)
                        WriteItem(w, loot.items[i]);
                w.U8(static_cast<uint8_t>(std::min<size_t>(SharedCount(loot.quest_items), 255)));
                for (size_t i = 0; i < loot.quest_items.size() && i < 255; ++i)
                    if (!loot.quest_items[i].freeforall)
                        WriteItem(w, loot.quest_items[i]);
                s.told = Service::Get().NetNow();
                if (!(only == Id128{}))
                {
                    Service::Get().Say(only, game::MsgLoot, w.data);
                    return;
                }
                Id128 party = s.party;
                Service::Get().SayAll([=](PeerInfo const& p) { return p.linked && p.presence.party == party; }, game::MsgLoot, w.data);
            }

            // ------------------------------------------------------------ a record arrives
            void OnRecord(Id128 const& from, Bytes const& data, Player* local)
            {
                Reader r(data);
                if (r.U8() != StepRecord)
                    return;
                EntityKey key = EntityKey::Read(r);
                uint32_t version = r.U32();
                Id128 party = r.Id();
                uint8_t method = r.U8();
                uint32_t gold = r.U32();
                Id128 roundRobin = r.Id(), groupLooter = r.Id();
                uint32_t unlooted = r.U32();
                std::vector<LootItem> items, quest;
                uint8_t n = r.U8();
                for (uint8_t i = 0; i < n && r.ok(); ++i)
                    items.push_back(ReadItem(r, i));
                n = r.U8();
                for (uint8_t i = 0; i < n && r.ok(); ++i)
                    quest.push_back(ReadItem(r, i));
                if (!r.ok() || !(party == Service::Get().Presence().party) || key.map != local->GetMapId())
                    return;
                auto known = _corpses.find(key);
                bool fromLeader = from == PartyLeader(local);
                if (known != _corpses.end())
                {
                    Shared_& s = known->second;
                    // the leader's record outranks a stand-in's; of one maker's, the newest is kept
                    if (s.maker && !fromLeader)
                        return;
                    if (!s.maker && s.version && s.makerId == from && version <= s.version)
                        return;
                }
                WorldObject* source = nullptr;
                Creature* c = nullptr;
                GameObject* go = nullptr;
                if (key.kind == 2)
                {
                    auto range = local->GetMap()->GetGameObjectBySpawnIdStore().equal_range(key.spawn);
                    if (range.first != range.second)
                        go = range.first->second;
                    source = go;
                }
                else
                {
                    c = WorldLookup(key, local);
                    source = c;
                    if (c && !c->isDead())
                        c = nullptr;   // (the death has not reached this server yet)
                }
                if (!source || (key.kind != 2 && !c))
                {
                    Pending& p = _pending[key];
                    p.from = from;
                    p.data = data;
                    p.at = Service::Get().NetNow();
                    return;
                }
                ::Loot& loot = c ? c->loot : go->loot;
                Shared_& s = _corpses[key];
                if (!s.version || s.makerId != from)
                {
                    s = Shared_{};
                    s.guid = source->GetGUID();
                    s.party = party;
                    s.object = go != nullptr;
                }
                else if (loot.loot_type == LOOT_CORPSE && SharedCount(loot.items) == items.size() && SharedCount(loot.quest_items) == quest.size())
                {
                    // the same maker's record again: what changed is applied to the loot as it is (a window may be open on it)
                    for (size_t i = 0; i < items.size(); ++i)
                        if (Shared(items[i], false))
                            ApplySlot(loot, s, StItem, uint8_t(i), StateBits(items[i]));
                    for (size_t i = 0; i < quest.size(); ++i)
                        if (Shared(quest[i], true))
                            ApplySlot(loot, s, StQuestItem, uint8_t(i), StateBits(quest[i]));
                    if (gold == 0)
                        ApplyGold(loot, s);
                    if (roundRobin == Id128{})
                        ApplyRelease(loot, s, c, local);
                    s.version = std::max(s.version, version);
                    if (c && loot.isLooted())
                        c->RemoveDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
                    return;
                }
                // what is already taken here stays taken (the record may be older than a state message)
                std::vector<uint8_t> oldItems = s.items, oldQuest = s.quest;
                loot.clear();
                loot.items = std::move(items);
                loot.quest_items = std::move(quest);
                for (size_t i = 0; i < loot.items.size() && i < oldItems.size(); ++i)
                    if ((oldItems[i] & ItLooted) && !loot.items[i].is_looted)
                        loot.items[i].is_looted = true;
                for (size_t i = 0; i < loot.quest_items.size() && i < oldQuest.size(); ++i)
                    if ((oldQuest[i] & ItLooted) && !loot.quest_items[i].is_looted)
                        loot.quest_items[i].is_looted = true;
                loot.gold = s.goldGone && s.version ? 0 : gold;
                loot.unlootedCount = static_cast<uint8>(std::min<uint32_t>(unlooted, 255));
                loot.roundRobinPlayer = GuidOf(roundRobin, local);
                loot.loot_type = LOOT_CORPSE;
                loot.sourceWorldObjectGUID = source->GetGUID();
                loot.lootOwnerGUID = local->GetGUID();
                Group* group = local->GetGroup();
                if (group)
                {
                    if (!(groupLooter == Id128{}))
                        group->SetLooterGuid(GuidOf(groupLooter, local));
                    (void)method;   // (the group's loot method is the party record's: Party.cpp)
                }
                s.maker = false;
                s.native = false;
                s.standIn = 0;
                s.makerId = from;
                s.version = version;
                s.goldGone = loot.gold == 0;
                s.hadLooter = !loot.roundRobinPlayer.IsEmpty();
                AddOwn(loot, source, local);
                Snapshot(s, loot);
                if (local->IsAtLootRewardDistance(source))
                    loot.FillNotNormalLootFor(local);   // (this player's quest and free for all slots; the core fills them where it makes the loot, here the record replaced it)
                if (c)
                {
                    c->SetLootRecipient(local);
                    if (!loot.isLooted())
                        c->SetDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
                    else
                        c->RemoveDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
                    if (group)
                    {
                        // the corpse names who may really loot it: the looter taking turns in a round robin or the looter of the loot method, on every server
                        Player* shown = loot.roundRobinPlayer.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(loot.roundRobinPlayer);
                        if (!shown && !(groupLooter == Id128{}) && group->GetLootMethod() != FREE_FOR_ALL)
                            shown = ObjectAccessor::FindPlayer(GuidOf(groupLooter, local));
                        group->SendLooter(c, shown);
                    }
                }
                else if (go)
                    go->SetLootState(GO_ACTIVATED, local);   // (a window opened here must not make the loot again)
            }

            // ------------------------------------------------------------ the state of a slot changes
            // true while the corpse is worth watching
            bool Watch(EntityKey const& key, Shared_& s, WorldObject* source, Player* local, int64_t now)
            {
                ::Loot* loot = nullptr;
                if (Creature* c = source->ToCreature())
                    loot = &c->loot;
                else if (GameObject* go = source->ToGameObject())
                    loot = &go->loot;
                if (!loot || loot->loot_type == LOOT_NONE || (loot->items.empty() && loot->quest_items.empty() && loot->gold == 0))
                    return false;   // (looted and cleared, or never had anything)
                bool changed = false;
                for (size_t i = 0; i < loot->items.size() && i < s.items.size(); ++i)
                {
                    LootItem const& item = loot->items[i];
                    if (!Shared(item, false))
                        continue;
                    uint8_t bits = StateBits(item);
                    if (bits != s.items[i])
                    {
                        s.items[i] = bits;
                        Announce(key, s, StItem, uint8_t(i), bits);
                        changed = true;
                    }
                }
                for (size_t i = 0; i < loot->quest_items.size() && i < s.quest.size(); ++i)
                {
                    LootItem const& item = loot->quest_items[i];
                    if (!Shared(item, true))
                        continue;
                    uint8_t bits = StateBits(item);
                    if (bits != s.quest[i])
                    {
                        s.quest[i] = bits;
                        Announce(key, s, StQuestItem, uint8_t(i), bits);
                        changed = true;
                    }
                }
                if (loot->gold == 0 && !s.goldGone)
                {
                    s.goldGone = true;
                    Announce(key, s, StGold, 0, 0);
                    changed = true;
                }
                if (s.hadLooter && loot->roundRobinPlayer.IsEmpty())
                {
                    s.hadLooter = false;
                    Announce(key, s, StRelease, 0, 0);
                    changed = true;
                }
                // the maker repeats the whole record now and then
                if (s.maker && (changed || now - s.told >= RepeatMs))
                {
                    ++s.version;
                    if (!changed)
                        SendRecord(key, s, local, *loot, source);
                }
                return true;
            }

            void Announce(EntityKey const& key, Shared_ const& s, uint8_t kind, uint8_t index, uint8_t bits)
            {
                Writer w;
                w.U8(StepState);
                key.Write(w);
                w.U8(kind).U8(index).U8(bits);
                Id128 party = s.party;
                Service::Get().SayAll([=](PeerInfo const& p) { return p.linked && p.presence.party == party; }, game::MsgLoot, w.data);
            }

            // a slot of the loot has a new state (taken, rolled for, rolled for no more): the windows that are open are told
            static void ApplySlot(::Loot& loot, Shared_& s, uint8_t kind, uint8_t index, uint8_t bits)
            {
                std::vector<LootItem>& list = kind == StItem ? loot.items : loot.quest_items;
                std::vector<uint8_t>& seen = kind == StItem ? s.items : s.quest;
                if (index >= list.size() || index >= seen.size())
                    return;
                LootItem& item = list[index];
                bool wasLooted = item.is_looted;
                item.is_blocked = bits & ItBlocked;
                item.is_underthreshold = bits & ItUnderThreshold;
                if ((bits & ItLooted) && !wasLooted)
                {
                    item.is_looted = true;
                    if (loot.unlootedCount)
                        --loot.unlootedCount;
                    if (kind == StItem)
                        loot.NotifyItemRemoved(index);
                    else
                        loot.NotifyQuestItemRemoved(index);
                }
                seen[index] = StateBits(item);   // (what is applied here is not announced again)
            }

            static void ApplyGold(::Loot& loot, Shared_& s)
            {
                if (loot.gold)
                {
                    loot.NotifyMoneyRemoved();
                    loot.gold = 0;
                }
                s.goldGone = true;
            }

            // the looter of a round robin closed the window: what remains is free for all
            static void ApplyRelease(::Loot& loot, Shared_& s, Creature* c, Player* local)
            {
                loot.roundRobinPlayer.Clear();
                s.hadLooter = false;
                if (c)
                    if (Group* group = local->GetGroup())
                        group->SendLooter(c, nullptr);
            }

            void OnState(Id128 const& from, Reader& r, Player* local)
            {
                EntityKey key = EntityKey::Read(r);
                uint8_t kind = r.U8(), index = r.U8(), bits = r.U8();
                auto known = _corpses.find(key);
                if (!r.ok() || known == _corpses.end() || known->second.party != Service::Get().Presence().party)
                    return;
                Shared_& s = known->second;
                Creature* c = s.object ? nullptr : local->GetMap()->GetCreature(s.guid);
                GameObject* go = s.object ? local->GetMap()->GetGameObject(s.guid) : nullptr;
                ::Loot* loot = c ? &c->loot : go ? &go->loot : nullptr;
                if (!loot)
                    return;
                switch (kind)
                {
                    case StItem:
                    case StQuestItem:
                        ApplySlot(*loot, s, kind, index, bits);
                        break;
                    case StGold:
                        ApplyGold(*loot, s);
                        break;
                    case StRelease:
                        ApplyRelease(*loot, s, c, local);
                        break;
                    default:
                        return;
                }
                if (c)
                {
                    if (loot->isLooted())
                        c->RemoveDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
                    c->ForceValuesUpdateAtIndex(UNIT_DYNAMIC_FLAGS);   // (who may loot it changes with the looter)
                }
                (void)from;
            }

            // ------------------------------------------------------------ the guids of a packet as they travel
            static void WriteGuid(ObjectGuid guid, Player* local, Writer& w)
            {
                if (guid.IsCreatureOrVehicle())
                {
                    Creature* c = local->GetMap()->GetCreature(guid);
                    EntityKey key;
                    if (c && WorldKeyOf(c, key))
                    {
                        w.U8(GuidCreature);
                        key.Write(w);
                        return;
                    }
                }
                else if (guid.IsPlayer())
                {
                    Id128 id = IdOf(guid, local);
                    if (!(id == Id128{}))
                    {
                        w.U8(GuidPlayer).Id(id);
                        return;
                    }
                }
                w.U8(GuidNone);
            }

            static ObjectGuid ReadGuid(Reader& r, Player* local)
            {
                uint8_t kind = r.U8();
                if (kind == GuidCreature)
                {
                    EntityKey key = EntityKey::Read(r);
                    Creature* c = r.ok() ? WorldLookup(key, local) : nullptr;
                    return c ? c->GetGUID() : ObjectGuid::Empty;
                }
                if (kind == GuidPlayer)
                {
                    Id128 id = r.Id();
                    return r.ok() ? GuidOf(id, local) : ObjectGuid::Empty;
                }
                return ObjectGuid::Empty;
            }

            // the packet of the core (bytes) in the form it travels in; false when it cannot be understood
            static bool Encode(uint16_t opcode, Bytes const& body, Player* local, Writer& w)
            {
                char const* layout = LayoutOf(opcode);
                WorldPacket p(opcode);
                p.append(body.data(), body.size());
                p.rpos(0);
                for (char const* c = layout; *c; ++c)
                {
                    if (*c == 'G' || *c == 'g')
                    {
                        if (p.size() - p.rpos() < 8)
                            return false;
                        ObjectGuid guid;
                        p >> guid;
                        WriteGuid(guid, local, w);
                    }
                    else if (*c == '4' || *c == '1' || *c == 'R')
                    {
                        size_t n = *c == '4' ? 4 : *c == 'R' ? 8 : 1;
                        if (p.size() - p.rpos() < n)
                            return false;
                        w.Raw(Bytes(p.contents() + p.rpos(), p.contents() + p.rpos() + n));
                        p.rpos(p.rpos() + n);
                    }
                    else if (*c == '*')
                    {
                        w.Raw(Bytes(p.contents() + p.rpos(), p.contents() + p.size()));
                        p.rpos(p.size());
                    }
                }
                return true;
            }

            // the member's side: the packet with the guids of this server
            static bool Decode(uint16_t opcode, Reader& r, Player* local, WorldPacket& out)
            {
                for (char const* c = LayoutOf(opcode); *c; ++c)
                {
                    if (*c == 'G' || *c == 'g')
                        out << ReadGuid(r, local);
                    else if (*c == 'R')
                        out << r.U64();
                    else if (*c == '4')
                        out << r.U32();
                    else if (*c == '1')
                        out << r.U8();
                    else if (*c == '*')
                    {
                        Bytes rest = r.Rest();
                        out.append(rest.data(), rest.size());
                    }
                }
                return r.ok();
            }

            // ---- the member: what the maker sent
            void OnPacket(Id128 const& peer, Reader& r, Player* local)
            {
                uint16_t opcode = r.U16();
                if (!r.ok() || !LayoutOf(opcode))
                    return;
                WorldPacket out(opcode);
                if (!Decode(opcode, r, local, out))
                    return;
                if (opcode == SMSG_LOOT_START_ROLL && out.size() >= 8)
                {
                    uint64 item;
                    memcpy(&item, out.contents(), 8);
                    _rolls[item] = peer;
                }
                local->SendDirectMessage(&out);
            }

            // ---- the maker: a member's roll (need, greed, pass) for an item: replayed through the mirror's session
            void OnRoll(Id128 const& peer, Reader& r)
            {
                uint64 item = r.U64();
                uint32 slot = r.U32();
                uint8 type = r.U8();
                Player* mirror = Mirrors::Get().MirrorOf(peer);
                if (!r.ok() || !mirror || !mirror->IsInWorld())
                    return;
                WorldPacket p(CMSG_LOOT_ROLL, 13);
                p << ObjectGuid(item) << slot << type;
                mirror->GetSession()->HandleLootRoll(p);
            }

            // ---- the member: an item won or given, or a share of the gold
            void OnAward(Id128 const& from, Reader& r, Player* local)
            {
                uint8_t kind = r.U8();
                OfferContent content;
                if (kind == AwardItem)
                {
                    ItemData d = ReadItem_(r);
                    if (!r.ok())
                        return;
                    content.items.push_back(d);
                }
                else if (kind == AwardMoney)
                {
                    content.money = r.U32();
                    if (!r.ok())
                        return;
                }
                else
                    return;
                Deliver(from, content, local);
            }

            static ItemData ReadItem_(Reader& r) { return p2p::ReadItem(r); }

            std::map<EntityKey, Shared_> _corpses;        // the corpses and objects whose loot is shared with the party
            std::map<EntityKey, Pending> _pending;        // records that arrived before the death they belong to
            std::map<EntityKey, int64_t> _cannot;         // corpses of which the leader's server said it cannot make the loot, before this server knew of the death
            std::map<uint64, Id128> _rolls;               // the items being rolled for (by the maker's guid of the item) and the server that runs the roll
            std::mutex _heldLock;
            std::optional<Held> _held;                    // the opening of a chest waits for the leader's server (any thread: the session's)
            std::atomic<bool> _replaying{false};          // the next opening is the held one coming back
            uint32_t _timer = 0;
        };

        // what the core sends to sessions without a connection, and what it stores in the mirrors' bags and pays them
        class LootSink : public ServerScript
        {
        public:
            LootSink() : ServerScript("P2PLootSink", { SERVERHOOK_ON_PACKET_SENT, SERVERHOOK_CAN_PACKET_RECEIVE }) { }
            void OnPacketSent(WorldSession* session, WorldPacket const& packet) override
            {
                if (session->IsHeadless() && Service::Get().Enabled())
                    Loot::Get().MirrorPacket(session, packet);
            }
            bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
            {
                if (packet.GetOpcode() == CMSG_LOOT_ROLL)
                    return !Loot::Get().LocalPacket(session, packet);
                if (packet.GetOpcode() == CMSG_GAMEOBJ_USE || packet.GetOpcode() == CMSG_CAST_SPELL)
                    return !Loot::Get().HoldUse(session, packet);
                return true;
            }
        };

        class LootPlayer : public PlayerScript
        {
        public:
            LootPlayer() : PlayerScript("P2PLootPlayer", { PLAYERHOOK_ON_STORE_NEW_ITEM, PLAYERHOOK_ON_MONEY_CHANGED, PLAYERHOOK_ON_BEFORE_LOOT_MONEY, PLAYERHOOK_ON_BEFORE_SEND_LOOT }) { }
            // the loot of a chest has just been made by the core for the local player: if the chest follows the party's rules, it is the party's
            void OnPlayerBeforeSendLoot(Player* player, ObjectGuid lootGuid, ::Loot*) override
            {
                if (lootGuid.IsGameObject() && Service::Get().Enabled() && !player->IsMirror())
                    if (GameObject* go = player->GetMap()->GetGameObject(lootGuid))
                        Loot::Get().ObjectOpened(go, player);
            }
            void OnPlayerBeforeLootMoney(Player*, ::Loot*) override { Loot::TakingGold(); }
            void OnPlayerStoreNewItem(Player* player, Item* item, uint32 count) override
            {
                if (player->IsMirror() && Service::Get().Enabled() && !g_ignoreMirrorStores)
                    Loot::Get().MirrorStored(player, item, count);
            }
            void OnPlayerMoneyChanged(Player* player, int32& amount) override
            {
                if (player->IsMirror() && Service::Get().Enabled() && Loot::Get().MirrorPaid(player, amount))
                    amount = 0;
            }
        };

        void Loot::Deliver(Id128 const& from, OfferContent const& loot, Player* local)
        {
            OfferContent mailed;
            for (ItemData const& d : loot.items)
            {
                ItemPosCountVec dest;
                if (Plausible(d) && local->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, d.entry, d.count) == EQUIP_ERR_OK)
                    if (Item* item = Create(local, d))
                    {
                        item = local->StoreItem(dest, item, true);
                        local->SendNewItem(item, d.count, true, false, false);
                        continue;
                    }
                mailed.items.push_back(d);
            }
            if (loot.money)
                local->ModifyMoney(loot.money);
            if (!mailed.items.empty())
            {
                KnownCharacter* k = Characters::Get().Meet(from);
                mailed.subject = "Loot";
                mailed.body = "Here is what did not fit into your bags.";
                Transfers::SendMailTo(local->GetGUID().GetCounter(), k ? k->guid : 0, mailed, 0, 0);
            }
        }
    }

    // test aid (game masters): the loot rules of the local player's group, without the client's restriction on the threshold (green at least)
    std::string TestLootRules(Player* local, uint32 method, uint32 threshold)
    {
        Group* group = local->GetGroup();
        if (!group)
            return "not in a group";
        group->SetLootMethod(LootMethod(method));
        group->SetLooterGuid(local->GetGUID());
        group->SetMasterLooterGuid(local->GetGUID());
        group->SetLootThreshold(ItemQualities(threshold));
        group->SendUpdate();
        return Acore::StringFormat("loot method {}, threshold {}, master looter {}", method, threshold, local->GetName());
    }

    // World.cpp: a creature died and the core made its loot for the local player's party
    void LootKilled(EntityKey const& key, ObjectGuid guid, bool native, bool tapperHere)
    {
        if (key.kind != 1 && key.kind != 3)
            return;   // (a creature of the world database or a shared summon; an object has its own paths)
        Loot::Get().Killed(key, guid, native, tapperHere);
    }

    // World.cpp: a state showed the creature dead before its death arrived; the leader's server makes the party's loot all the same
    void LootKilledLate(EntityKey const& key, ObjectGuid guid, Id128 const& tapper)
    {
        if (key.kind != 1 && key.kind != 3)
            return;
        Loot::Get().KilledLate(key, guid, tapper);
    }

    void AddLoot()
    {
        Service::Get().AddFeature(&Loot::Get());
        new LootSink();
        new LootPlayer();
    }
}
