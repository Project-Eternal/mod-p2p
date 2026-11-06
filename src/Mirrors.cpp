#include "Mirrors.h"
#include "CharacterCache.h"
#include "Config.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SocialMgr.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Creature.h"
#include "core/Authority.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <unordered_map>

// Player::m_social is private and only the character's login sets it. The core's duel effect (Spell::EffectDuel) silently does nothing for a target without a social
// list, which a mirror never had, so the module gives a mirror an empty one (and takes it away before the mirror logs out). Reaching the member through an explicit
// instantiation keeps the core's headers untouched (a change there would rebuild the whole game library).
namespace
{
    struct SocialMember { using Type = PlayerSocial* Player::*; static Type pointer; };
    SocialMember::Type SocialMember::pointer = nullptr;
    template <SocialMember::Type P> struct SocialBinder { SocialBinder() { SocialMember::pointer = P; } static SocialBinder instance; };
    template <SocialMember::Type P> SocialBinder<P> SocialBinder<P>::instance;
    template struct SocialBinder<&Player::m_social>;

    void SetSocialOf(Player* player, PlayerSocial* social) { player->*SocialMember::pointer = social; }
}

AC_GAME_API extern void (*AuraTiming)(Aura const*, int);

namespace p2p
{
    bool WorldKeyOf(Creature const* c, EntityKey& key);                   // World.cpp
    Creature* WorldLookup(EntityKey const& key, Player* local);          // World.cpp
    bool WorldDecides(Creature const* c);                                 // World.cpp
    bool WorldTracked(Creature const* c);                                 // World.cpp

    extern thread_local int g_ignoreMirrorStores;   // Loot.cpp: items stored in a mirror's bags that are not a take of loot

    namespace
    {
        constexpr float MirrorRange = 150.0f;       // a player this near (yards) gets a mirror
        constexpr float MirrorRangeLeave = 200.0f;  // and loses it beyond this
        constexpr uint32_t MaxMirrors = 80;
        thread_local bool g_syncing = false;   // the world thread puts auras on a mirror because its owner's record lists them: that is not news to tell the owner

        float Distance2D(float ax, float ay, float bx, float by) { return std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by)); }

        // The moment an aura ends, in network time, is noted once when the aura gets its duration (made, set, refreshed, restacked, loaded) and kept: the snapshot of
        // a character, the state of a creature and its look digest all send that kept moment. Worked out again at each look (now + the remaining duration) it would
        // move by up to one update of the map every time, because the remaining duration falls in steps while the clock runs on.
        // (every map thread makes and destroys auras: the table is cut in shards by the aura's address, each with its own lock and a count that lets an empty shard be passed by)
        struct EndShard
        {
            std::mutex mutex;
            std::unordered_map<Aura const*, int64_t> end;
            std::atomic<size_t> count{0};
        };
        constexpr size_t EndShards = 32;
        EndShard g_endShards[EndShards];

        EndShard& ShardOf(Aura const* aura) { return g_endShards[(reinterpret_cast<uintptr_t>(aura) >> 6) % EndShards]; }

        // only the auras that are ever sent: those of players and creatures (a dynamic object's, or an item's, never)
        bool Tracked(Aura const* aura)
        {
            WorldObject const* owner = aura->GetOwner();
            return owner && (owner->IsPlayer() || owner->IsCreature());
        }

        int64_t EndOf(Aura const* aura, int64_t now)
        {
            int32 left = aura->GetDuration();
            if (left <= 0 || aura->GetMaxDuration() <= 0)
                return 0;
            EndShard& shard = ShardOf(aura);
            std::lock_guard<std::mutex> lock(shard.mutex);
            auto it = shard.end.find(aura);
            if (it == shard.end.end())
            {
                it = shard.end.emplace(aura, now + left).first;   // (an aura that was there before the hook: noted at the first look)
                shard.count.store(shard.end.size(), std::memory_order_relaxed);
            }
            return it->second;
        }

        void NoteEnd(Aura const* aura, int64_t end)
        {
            EndShard& shard = ShardOf(aura);
            std::lock_guard<std::mutex> lock(shard.mutex);
            shard.end[aura] = end;
            shard.count.store(shard.end.size(), std::memory_order_relaxed);
        }

        void ForgetEnd(Aura const* aura)
        {
            EndShard& shard = ShardOf(aura);
            if (!shard.count.load(std::memory_order_relaxed))
                return;
            std::lock_guard<std::mutex> lock(shard.mutex);
            shard.end.erase(aura);
            shard.count.store(shard.end.size(), std::memory_order_relaxed);
        }

        void AuraTimingHook(Aura const* aura, int what)
        {
            if (what == 1)
            {
                ForgetEnd(aura);
                return;
            }
            Service& service = Service::Get();
            if (!service.Enabled())
                return;
            int32 left = aura->GetDuration();
            if (left > 0 && aura->GetMaxDuration() > 0 && Tracked(aura))
                NoteEnd(aura, service.NetNow() + left);
            else
                ForgetEnd(aura);
        }
    }

    Mirrors& Mirrors::Get()
    {
        static Mirrors mirrors;
        return mirrors;
    }

    Player* Mirrors::MirrorOf(Id128 const& peer) const
    {
        auto it = _mirrors.find(peer);
        return it == _mirrors.end() ? nullptr : ObjectAccessor::FindPlayer(it->second.guid);
    }

    std::vector<std::string> Mirrors::Describe() const
    {
        std::vector<std::string> lines;
        for (auto const& [id, m] : _mirrors)
            if (Player* p = ObjectAccessor::FindPlayer(m.guid))
                lines.push_back(Acore::StringFormat("mirror {} (net {}, guid {:x}) level {} at {:.0f} {:.0f} {:.0f}, health {}/{}", p->GetName(), Hex(id).substr(0, 8), p->GetGUID().GetRawValue(), p->GetLevel(),
                    p->GetPositionX(), p->GetPositionY(), p->GetPositionZ(), p->GetHealth(), p->GetMaxHealth()));
        return lines;
    }

    Id128 Mirrors::PeerOf(ObjectGuid guid) const
    {
        for (auto const& [id, m] : _mirrors)
            if (m.guid == guid)
                return id;
        return Id128{};
    }

    // ---------------------------------------------------------------- what the local character looks like

    game::Snapshot Mirrors::SnapshotOf(Player* p)
    {
        game::Snapshot s;
        s.name = p->GetName();
        s.race = p->getRace();
        s.cls = p->getClass();
        s.gender = p->getGender();
        s.level = p->GetLevel();
        s.bytes = p->GetUInt32Value(PLAYER_BYTES);
        s.bytes2 = p->GetUInt32Value(PLAYER_BYTES_2);
        s.bytes3 = p->GetUInt32Value(PLAYER_BYTES_3);
        s.displayId = p->GetDisplayId();
        s.nativeDisplayId = p->GetNativeDisplayId();
        s.mountDisplayId = p->GetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID);
        s.faction = p->GetFaction();
        s.unitFlags = p->GetUInt32Value(UNIT_FIELD_FLAGS);
        s.playerFlags = p->GetUInt32Value(PLAYER_FLAGS);
        s.unitBytes2 = p->GetUInt32Value(UNIT_FIELD_BYTES_2);
        s.scale = p->GetObjectScale();
        s.health = p->GetHealth();
        s.maxHealth = p->GetMaxHealth();
        s.powerType = p->getPowerType();
        s.power = p->GetPower(p->getPowerType());
        s.maxPower = p->GetMaxPower(p->getPowerType());
        s.standState = p->getStandState();
        s.shapeshift = p->GetShapeshiftForm();
        for (int i = 0; i < game::EquipmentSlots; ++i)
        {
            s.equipment[i][0] = p->GetUInt32Value(PLAYER_VISIBLE_ITEM_1_ENTRYID + i * 2);
            s.equipment[i][1] = p->GetUInt16Value(PLAYER_VISIBLE_ITEM_1_ENCHANTMENT + i * 2, 0);
            s.equipment[i][2] = p->GetUInt16Value(PLAYER_VISIBLE_ITEM_1_ENCHANTMENT + i * 2, 1);
        }
        CollectAuras(p, s.auras);
        s.title = p->GetUInt32Value(PLAYER_CHOSEN_TITLE);
        s.x = p->GetPositionX();
        s.y = p->GetPositionY();
        s.z = p->GetPositionZ();
        s.o = p->GetOrientation();
        s.map = p->GetMapId();
        s.phaseMask = p->GetPhaseMask();
        s.zone = p->GetZoneId();
        s.area = p->GetAreaId();
        s.ground = p->GetMap()->GetHeight(p->GetPhaseMask(), s.x, s.y, s.z, true);
        s.moveFlags = p->GetUnitMovementFlags();
        s.runSpeed = p->GetSpeed(MOVE_RUN);
        s.flags = (p->isDead() ? 1 : 0) | (p->IsInCombat() ? 2 : 0);
        // the spell being cast and the spell being channelled: part of the record, so that a mirror that came into sight (or missed the start) shows it
        if (Spell* cast = p->GetCurrentSpell(CURRENT_GENERIC_SPELL); cast && !cast->IsTriggered() && cast->GetCastTime() > 0)
        {
            s.castSpell = cast->GetSpellInfo()->Id;
            s.castEnd = Service::Get().NetNow() + cast->GetCastTimeRemaining();
        }
        if (Spell* channel = p->GetCurrentSpell(CURRENT_CHANNELED_SPELL); channel && !channel->IsTriggered())
        {
            s.channelSpell = channel->GetSpellInfo()->Id;
            s.channelEnd = Service::Get().NetNow() + channel->GetCastTimeRemaining();
            s.channelTarget = CasterId(p->GetGuidValue(UNIT_FIELD_CHANNEL_OBJECT));
        }
        return s;
    }

    // the character that a guid is, for the other side: the local player is zero ("itself") when it is the owner of the record, a mirror is its peer
    bool Mirrors::Syncing() { return g_syncing; }

    void Mirrors::CollectAuras(Unit* p, std::vector<game::AuraRecord>& out)
    {
        int64_t now = Service::Get().NetNow();
        for (auto const& [id, app] : p->GetAppliedAuras())
        {
            if (out.size() >= 64)
                break;
            Aura const* aura = app->GetBase();
            SpellInfo const* info = aura->GetSpellInfo();
            if (info->IsPassive() || info->HasAttribute(SPELL_ATTR0_DO_NOT_DISPLAY))
                continue;
            game::AuraRecord a;
            a.spell = info->Id;
            a.maxDuration = aura->GetMaxDuration();
            a.expiry = EndOf(aura, now);
            a.stacks = aura->GetStackAmount();
            a.flags = app->IsPositive() ? 1 : 0;
            a.caster = aura->GetCasterGUID() == p->GetGUID() ? Id128{} : CasterId(aura->GetCasterGUID());
            out.push_back(a);
        }
    }

    Id128 Mirrors::CasterId(ObjectGuid guid)
    {
        Service& s = Service::Get();
        if (Player* l = s.Local(); l && l->GetGUID() == guid)
            return s.Me().id;
        return Get().PeerOf(guid);
    }

    void Mirrors::OnLogin(Player* player)
    {
        _haveLastSent = false;
        _timer = _checkTimer = 0;
        _seq = 0;
        _lastSentAt = 0;
    }

    void Mirrors::OnLogout(Player* player)
    {
        RemoveAll();
        _snapshots.clear();
    }

    // ---------------------------------------------------------------- what comes in

    void Mirrors::OnEvent(InEvent const& e, Player* local)
    {
        Service& service = Service::Get();
        switch (e.kind)
        {
            case InEvent::LinkUp:
                SendSnapshotTo(e.peer, true);
                break;
            case InEvent::LinkDown:
            case InEvent::Gone:
                Remove(e.peer);
                if (e.kind == InEvent::Gone)
                    _snapshots.erase(e.peer);
                break;
            case InEvent::Announced:
                break;
            case InEvent::Message:
                if (e.type == game::MsgSnapshot)
                {
                    if (PeerInfo* peer = service.FindPeer(e.peer))
                        OnSnapshot(*peer, e.data, local);
                }
                else if (e.type == game::MsgMove)
                    OnMove(e.peer, e.data);
                else if (e.type == game::MsgAura)
                    OnAura(e.peer, e.data, local);
                break;
            default:
                break;
        }
    }

    // The position a player announces must make sense in our own game: the same zone and area, and the ground within a few yards of the height
    // we know. A player who is not where its claims say (a modified area, a cheat) is not shown.
    bool Mirrors::Plausible(game::Snapshot const& snap, Player* local) const
    {
        Map* map = local->GetMap();
        if (snap.map != local->GetMapId() || Distance2D(snap.x, snap.y, local->GetPositionX(), local->GetPositionY()) > MirrorRange)
            return true;   // (we cannot tell from here: the game answers only near the local player)
        uint32 zone = 0, area = 0;
        map->GetZoneAndAreaId(snap.phaseMask, zone, area, snap.x, snap.y, snap.z);
        if (zone && snap.zone && zone != snap.zone)
            return false;
        float ground = map->GetHeight(snap.phaseMask, snap.x, snap.y, snap.z, true);
        if (ground > INVALID_HEIGHT && snap.ground > INVALID_HEIGHT && std::abs(ground - snap.ground) > 12.0f)
            return false;
        return true;
    }

    bool Mirrors::Wanted(PeerInfo const& peer, game::Snapshot const& snap, Player* local, bool check) const
    {
        if (!local || !peer.linked)
            return false;
        game::Presence const& pr = peer.presence;
        if (snap.map != local->GetMapId() || pr.instanceKey != Service::Get().Presence().instanceKey)
            return false;
        if ((snap.phaseMask & local->GetPhaseMask()) == 0)
            return false;
        return !check || Plausible(snap, local);   // (asking the game is for a mirror about to be made, not for each check of one that exists)
    }

    void Mirrors::OnSnapshot(PeerInfo& peer, Bytes const& data, Player* local)
    {
        game::Snapshot snap;
        if (!game::Snapshot::Decode(data, snap) || !Characters::ValidName(snap.name))
            return;
        // the identity of the character: a row, a name that is local
        Characters::Get().Learn(peer.cert, snap.name, snap.race, snap.cls, snap.gender, snap.level);
        _snapshots[peer.cert.id] = snap;
        auto it = _mirrors.find(peer.cert.id);
        if (it != _mirrors.end())
        {
            if (Player* player = ObjectAccessor::FindPlayer(it->second.guid))
            {
                it->second.snapshot = snap;
                it->second.haveSnapshot = true;
                Apply(it->second, player, snap, false);
                return;
            }
            _mirrors.erase(it);
        }
        if (local && Wanted(peer, snap, local, true) && Distance2D(snap.x, snap.y, local->GetPositionX(), local->GetPositionY()) < MirrorRange)
            Create(peer, snap, local);
    }

    void Mirrors::OnMove(Id128 const& peerId, Bytes const& data)
    {
        auto it = _mirrors.find(peerId);
        if (it == _mirrors.end())
            return;
        Player* player = ObjectAccessor::FindPlayer(it->second.guid);
        if (!player || !player->IsInWorld())
            return;
        Reader r(data);
        uint16_t opcode = r.U16();
        Bytes payload = r.Blob(256);
        if (!r.ok())
            return;
        switch (opcode)
        {
            case MSG_MOVE_START_FORWARD: case MSG_MOVE_START_BACKWARD: case MSG_MOVE_STOP: case MSG_MOVE_START_STRAFE_LEFT:
            case MSG_MOVE_START_STRAFE_RIGHT: case MSG_MOVE_STOP_STRAFE: case MSG_MOVE_JUMP: case MSG_MOVE_START_TURN_LEFT:
            case MSG_MOVE_START_TURN_RIGHT: case MSG_MOVE_STOP_TURN: case MSG_MOVE_START_PITCH_UP: case MSG_MOVE_START_PITCH_DOWN:
            case MSG_MOVE_STOP_PITCH: case MSG_MOVE_SET_RUN_MODE: case MSG_MOVE_SET_WALK_MODE: case MSG_MOVE_FALL_LAND:
            case MSG_MOVE_START_SWIM: case MSG_MOVE_STOP_SWIM: case MSG_MOVE_SET_FACING: case MSG_MOVE_SET_PITCH:
            case MSG_MOVE_HEARTBEAT: case CMSG_MOVE_FALL_RESET: case CMSG_MOVE_SET_FLY: case MSG_MOVE_START_ASCEND:
            case MSG_MOVE_STOP_ASCEND: case MSG_MOVE_START_DESCEND: case CMSG_MOVE_NOT_ACTIVE_MOVER: case MSG_MOVE_UPDATE_CAN_FLY:
                break;
            default:
                return;   // (only the movement of the character itself: no acknowledgements, no teleports)
        }
        // the packet as the client sent it, with the mirror's identifier in front
        WorldPacket packet(opcode, payload.size() + 8);
        packet << player->GetGUID().WriteAsPacked();
        packet.append(payload.data(), payload.size());
        player->GetSession()->HandleMovementOpcodes(packet);
    }

    // ---------------------------------------------------------------- the mirror itself

    Player* Mirrors::Create(PeerInfo& peer, game::Snapshot const& snap, Player* local)
    {
        if (_mirrors.size() >= MaxMirrors)
            return nullptr;
        KnownCharacter* known = Characters::Get().Meet(peer.cert.id);
        if (!known)
            return nullptr;
        Map* map = local->GetMap();
        // (this account has no real connection; the session gives the Player the account data it asks for)
        WorldSession* session = new WorldSession(Characters::Get().MirrorAccount(), "mirror", 0, nullptr, SEC_PLAYER, EXPANSION_WRATH_OF_THE_LICH_KING,
            0, LOCALE_enUS, 0, false, true, 0);
        Player* player = new Player(session);
        player->GetMotionMaster()->Initialize();
        CharacterCreateInfo info(known->localName, snap.race, snap.cls, snap.gender, snap.bytes & 0xFF, (snap.bytes >> 8) & 0xFF,
            (snap.bytes >> 16) & 0xFF, (snap.bytes >> 24) & 0xFF, snap.bytes2 & 0xFF);
        // (the core's character creation gives the starting items, the hearthstone among them: no loot, and never passed on to the player as one)
        ++g_ignoreMirrorStores;
        bool created = player->Create(known->guid, &info);
        --g_ignoreMirrorStores;
        if (!created)
        {
            LOG_ERROR("module", "[P2P] Cannot make the mirror of {}", known->localName);
            delete player;
            delete session;
            return nullptr;
        }
        {
            PlayerSocial* social = sSocialMgr->LoadFromDB(nullptr, player->GetGUID());
            social->SetAccountId(session->GetAccountId());
            SetSocialOf(player, social);
        }
        player->SetMap(map);
        player->Relocate(snap.x, snap.y, snap.z, snap.o);
        player->SetPhaseMask(snap.phaseMask, false);
        session->SetPlayer(player);
        ObjectAccessor::AddObject(player);
        Mirror mirror;
        mirror.guid = player->GetGUID();
        mirror.session = session;
        mirror.snapshot = snap;
        mirror.haveSnapshot = true;
        Apply(mirror, player, snap, true);
        if (!map->AddPlayerToMap(player))
        {
            ObjectAccessor::RemoveObject(player);
            session->SetPlayer(nullptr);
            delete player;
            delete session;
            return nullptr;
        }
        _mirrors[peer.cert.id] = mirror;
        Service::Get().Meet(peer.cert.id);
        return player;
    }

    void Mirrors::Apply(Mirror& mirror, Player* p, game::Snapshot const& s, bool first)
    {
        if (first || p->GetLevel() != s.level)
            p->SetUInt32Value(UNIT_FIELD_LEVEL, s.level);
        p->SetUInt32Value(PLAYER_BYTES, s.bytes);
        p->SetUInt32Value(PLAYER_BYTES_2, s.bytes2);
        p->SetUInt32Value(PLAYER_BYTES_3, s.bytes3);
        p->SetObjectScale(s.scale);
        p->SetFaction(s.faction);
        // (the owner's fight is the mirror's: the flag comes from the record, and the core does not clear it by its timer for a mirror)
        p->SetUInt32Value(UNIT_FIELD_FLAGS, (s.unitFlags & ~(UNIT_FLAG_IN_COMBAT | UNIT_FLAG_DISABLE_MOVE | UNIT_FLAG_STUNNED)) | ((s.flags & 2) ? UNIT_FLAG_IN_COMBAT : 0));
        p->SetUInt32Value(PLAYER_FLAGS, s.playerFlags & ~PLAYER_FLAGS_AFK);
        p->SetUInt32Value(UNIT_FIELD_BYTES_2, s.unitBytes2);
        if (p->GetDisplayId() != s.displayId)
            p->SetDisplayId(s.displayId);
        p->SetNativeDisplayId(s.nativeDisplayId);
        p->SetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID, s.mountDisplayId);
        p->SetUInt32Value(PLAYER_CHOSEN_TITLE, s.title);
        p->setPowerType(Powers(s.powerType));
        p->SetMaxHealth(s.maxHealth ? s.maxHealth : 1);
        p->SetHealth(s.health ? s.health : 1);
        p->SetMaxPower(Powers(s.powerType), s.maxPower);
        p->SetPower(Powers(s.powerType), s.power);
        for (int i = 0; i < game::EquipmentSlots; ++i)
        {
            p->SetUInt32Value(PLAYER_VISIBLE_ITEM_1_ENTRYID + i * 2, s.equipment[i][0]);
            p->SetUInt16Value(PLAYER_VISIBLE_ITEM_1_ENCHANTMENT + i * 2, 0, s.equipment[i][1]);
            p->SetUInt16Value(PLAYER_VISIBLE_ITEM_1_ENCHANTMENT + i * 2, 1, s.equipment[i][2]);
        }
        if (p->getStandState() != s.standState)
            p->SetStandState(s.standState);
        if (p->GetPhaseMask() != s.phaseMask)
            p->SetPhaseMask(s.phaseMask, true);
        // too far from where the owner says it is (it teleported, or movement packets were lost): put it there
        if (!first && p->IsInWorld() && Distance2D(p->GetPositionX(), p->GetPositionY(), s.x, s.y) > 25.0f)
            p->UpdatePosition(s.x, s.y, s.z, s.o, true);
        if (p->IsInWorld())
        {
            SyncAuras(p, s.auras);
            SyncMovement(p, s);
            SyncChannel(mirror, p, s);
        }
    }

    // A channel in the record that the mirror does not show comes (its fields, which the client draws the bar and the animation from); one that the record
    // does not have any more goes if it was set from a record.
    void Mirrors::SyncChannel(Mirror& mirror, Player* p, game::Snapshot const& s)
    {
        int64_t now = Service::Get().NetNow();
        uint32 shown = p->GetUInt32Value(UNIT_CHANNEL_SPELL);
        if (s.channelSpell && s.channelEnd > now + 300)
        {
            if (shown != s.channelSpell)
            {
                p->SetUInt32Value(UNIT_CHANNEL_SPELL, s.channelSpell);
                Unit* target = nullptr;
                if (s.channelTarget == Service::Get().Me().id)
                    target = Service::Get().Local();
                else if (!(s.channelTarget == Id128{}))
                    target = MirrorOf(s.channelTarget);
                if (target)
                    p->SetGuidValue(UNIT_FIELD_CHANNEL_OBJECT, target->GetGUID());
                mirror.channelFromRecord = true;
            }
        }
        else if (shown && mirror.channelFromRecord)
        {
            p->SetGuidValue(UNIT_FIELD_CHANNEL_OBJECT, ObjectGuid::Empty);
            p->SetUInt32Value(UNIT_CHANNEL_SPELL, 0);
            mirror.channelFromRecord = false;
        }
        else if (!shown)
            mirror.channelFromRecord = false;
    }

    // The auras the record lists are on the mirror with their remaining time, those it lacks come, those it should not have go. An aura that a local caster
    // has just put on the mirror stays a few seconds: its owner has not told us of it yet.
    void Mirrors::SyncAuras(Unit* p, std::vector<game::AuraRecord> const& records)
    {
        int64_t now = Service::Get().NetNow();
        Service& service = Service::Get();
        g_syncing = true;
        struct Reset { ~Reset() { g_syncing = false; } } reset;
        std::set<uint32_t> wanted;
        for (game::AuraRecord const& rec : records)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(rec.spell);
            if (!info)
                continue;
            int32 left = rec.expiry ? static_cast<int32>(std::min<int64_t>(rec.expiry - now, 24 * 3600 * 1000)) : -1;
            if (rec.expiry && left <= 0)
                continue;   // (it ended while the record was on its way)
            wanted.insert(rec.spell);
            // (an aura that damages over time is shown on the mirror like any other; the core does not tick it there: Player::IsMirror)
            Aura* aura = nullptr;
            auto range = p->GetOwnedAuras().equal_range(rec.spell);
            if (range.first != range.second)
                aura = range.first->second;
            else
            {
                Unit* caster = p;
                if (!(rec.caster == Id128{}))
                {
                    if (rec.caster == service.Me().id)
                        caster = service.Local();
                    else if (Player* m = MirrorOf(rec.caster))
                        caster = m;
                }
                aura = caster ? caster->AddAura(info, MAX_EFFECT_MASK, p) : nullptr;
            }
            if (!aura)
                continue;
            if (left > 0)
            {
                if (std::abs(aura->GetDuration() - left) > 1500 || aura->GetMaxDuration() != rec.maxDuration)
                {
                    aura->SetMaxDuration(rec.maxDuration > 0 ? rec.maxDuration : left);
                    aura->SetDuration(left);
                }
                NoteEnd(aura, rec.expiry);   // (the copy ends when its owner's aura does)
            }
            else if (left < 0 && !aura->IsPermanent())
            {
                aura->SetMaxDuration(-1);
                aura->SetDuration(-1);
            }
            if (rec.stacks > 1 && aura->GetStackAmount() != rec.stacks)
                aura->SetStackAmount(rec.stacks);
        }
        std::vector<uint32_t> extra;
        {
            std::lock_guard<std::mutex> lock(_freshMutex);
            for (auto it = _fresh.begin(); it != _fresh.end();)
                it = now - it->second > 5000 ? _fresh.erase(it) : std::next(it);
            for (auto const& [id, aura] : p->GetOwnedAuras())
                if (!wanted.count(id) && !aura->GetSpellInfo()->IsPassive() && !aura->GetSpellInfo()->HasAttribute(SPELL_ATTR0_DO_NOT_DISPLAY) &&
                    !_fresh.count({p->GetGUID().GetRawValue(), id}))
                    extra.push_back(id);
        }
        for (uint32_t id : extra)
            p->RemoveAurasDueToSpell(id);
    }

    // A mirror that moves while the record says its character stands is stopped where the record puts it; one that stands while the record says it moves
    // is set moving. The movement packets are the fast path and show the way; this is the repair when one of them was lost or came late.
    void Mirrors::SyncMovement(Player* p, game::Snapshot const& s)
    {
        uint32 mask = MOVEMENTFLAG_MASK_MOVING_OR_TURN | MOVEMENTFLAG_SWIMMING | MOVEMENTFLAG_FLYING | MOVEMENTFLAG_FALLING;
        uint32 want = s.moveFlags & mask, have = p->GetUnitMovementFlags() & mask;
        if (want == have)
            return;
        MovementInfo mi;
        mi.guid = p->GetGUID();
        mi.flags = (p->GetUnitMovementFlags() & ~mask) | want;
        mi.flags2 = 0;
        mi.time = getMSTime();
        mi.pos.Relocate(s.x, s.y, s.z, s.o);
        mi.fallTime = 0;
        WorldPacket packet(want ? MSG_MOVE_HEARTBEAT : MSG_MOVE_STOP, 64);
        p->GetSession()->WriteMovementInfo(&packet, &mi);
        p->GetSession()->HandleMovementOpcodes(packet);
    }

    // ---------------------------------------------------------------- auras between servers

    // The local player (or a creature this server decides) put an aura on a mirror: that aura is the mirror's owner's business. The hook runs on a map thread.
    void Mirrors::LocalAura(Player* mirror, Aura* aura)
    {
        Service& service = Service::Get();
        Player* local = service.Local();
        Unit* caster = aura->GetCaster();
        if (g_syncing || !local || !caster || aura->GetSpellInfo()->IsPassive() || aura->GetSpellInfo()->HasAttribute(SPELL_ATTR0_DO_NOT_DISPLAY))
            return;
        bool byLocal = caster == local || caster->GetCharmerOrOwnerGUID() == local->GetGUID();
        // what a creature that this server decides casts is its decider's cast: the aura it puts on a mirror goes to the mirror's owner like a hit
        Creature const* creature = byLocal ? nullptr : caster->ToCreature();
        if (creature && (creature->IsPuppet() || !WorldTracked(creature)))
            creature = nullptr;
        if (!byLocal && !creature)
            return;
        ObjectGuid guid = mirror->GetGUID();
        ObjectGuid casterGuid = creature ? creature->GetGUID() : ObjectGuid::Empty;
        uint32 spell = aura->GetSpellInfo()->Id;
        int32 left = aura->GetDuration(), max = aura->GetMaxDuration();
        uint8 stacks = aura->GetStackAmount(), beneficial = aura->GetSpellInfo()->IsPositive() ? 1 : 0;
        {
            std::lock_guard<std::mutex> lock(_freshMutex);
            _fresh[{guid.GetRawValue(), spell}] = service.NetNow();
        }
        service.Defer([this, guid, casterGuid, spell, left, max, stacks, beneficial]
        {
            Id128 peer = PeerOf(guid);
            if (peer == Id128{})
                return;
            Writer w;
            w.U32(spell).U32(static_cast<uint32_t>(left)).U32(static_cast<uint32_t>(max)).U8(stacks).U8(beneficial);
            if (!casterGuid.IsEmpty())
            {
                // (the creature that cast it, by its key: the owner's copy of it is the caster there)
                Player* l = Service::Get().Local();
                Creature* c = l && l->IsInWorld() ? l->GetMap()->GetCreature(casterGuid) : nullptr;
                EntityKey key;
                if (!c || !WorldDecides(c) || !WorldKeyOf(c, key))
                    return;
                w.U8(1);
                key.Write(w);
            }
            Service::Get().Say(peer, game::MsgAura, w.data);
        });
    }

    // an aura that another server's player (or creature) put on the local player
    void Mirrors::OnAura(Id128 const& peerId, Bytes const& data, Player* local)
    {
        Unit* caster = MirrorOf(peerId);
        Reader r(data);
        uint32 spell = r.U32();
        int32 left = static_cast<int32>(r.U32()), max = static_cast<int32>(r.U32());
        uint8 stacks = r.U8(), beneficial = r.U8();
        if (r.ok() && !r.end() && r.U8() == 1)
        {
            // cast by a creature that the sender decides: here its copy casts nothing, the aura is put with the copy as the caster
            EntityKey key = EntityKey::Read(r);
            caster = r.ok() && local ? WorldLookup(key, local) : nullptr;
        }
        SpellInfo const* info = r.ok() ? sSpellMgr->GetSpellInfo(spell) : nullptr;
        if (!caster || !local || !local->IsInWorld() || !info || !local->IsAlive() || caster->GetExactDist2d(local) > 120.0f)
            return;
        if (left > 3600 * 1000 || max > 3600 * 1000 || info->IsPassive())
            return;
        // the game's own rules at this moment: a beneficial aura only from one who may assist the player, a harmful one only from one who may attack it
        // (a duel that has just ended, a faction that is not hostile, a player who is not in range any more)
        if (beneficial ? !caster->IsValidAssistTarget(local) : !caster->IsValidAttackTarget(local, info))
        {
            LOG_INFO("module", "[P2P] Refused aura {} from {}: not allowed now", spell, caster->GetName());
            return;
        }
        Aura* aura = caster->AddAura(info, MAX_EFFECT_MASK, local);
        if (!aura)
            return;
        if (left > 0)
        {
            aura->SetMaxDuration(max > 0 ? max : left);
            aura->SetDuration(left);
        }
        if (stacks > 1)
            aura->SetStackAmount(stacks);
    }

    void Mirrors::Remove(Id128 const& peer)
    {
        auto it = _mirrors.find(peer);
        if (it == _mirrors.end())
            return;
        Mirror mirror = it->second;
        _mirrors.erase(it);
        if (Player* player = ObjectAccessor::FindPlayer(mirror.guid))
        {
            SetSocialOf(player, nullptr);   // (as before the module gave it one: the logout then neither tells friends nor looks for a list)
            sSocialMgr->RemovePlayerSocial(mirror.guid);
            mirror.session->LogoutPlayer(false);
        }
        delete mirror.session;
    }

    void Mirrors::RemoveAll()
    {
        std::vector<Id128> all;
        for (auto const& [id, m] : _mirrors)
            all.push_back(id);
        for (Id128 const& id : all)
            Remove(id);
    }

    // ---------------------------------------------------------------- what goes out

    void Mirrors::SendSnapshotTo(Id128 const& peer, bool force)
    {
        Player* local = Service::Get().Local();
        if (!local || !local->IsInWorld())
            return;
        game::Snapshot s = SnapshotOf(local);
        s.instanceKey = Service::Get().Presence().instanceKey;
        s.seq = _seq;
        Service::Get().SayKeyed(peer, game::KeySnapshot, game::MsgSnapshot, s.Encode());
    }

    void Mirrors::LocalMovement(Player* player, uint16_t opcode, Bytes const& payload, bool heartbeat)
    {
        Service& service = Service::Get();
        Writer w;
        w.U16(opcode).Blob(payload);
        uint32_t map = player->GetMapId();
        uint32_t phase = player->GetPhaseMask();
        uint64_t key = service.Presence().instanceKey;
        // (the keyed channel: a movement that a later one replaces is not waited for, a stop is a movement too)
        (void)heartbeat;
        service.SayAllKeyed([&](PeerInfo const& p)
        {
            return p.presence.map == map && p.presence.instanceKey == key && (p.presence.phaseMask & phase) != 0;
        }, game::KeyMovement, game::MsgMove, w.data);
    }

    void Mirrors::Update(uint32_t diff, Player* local)
    {
        if (!local->IsInWorld())
            return;
        _timer += diff;
        _checkTimer += diff;
        Service& service = Service::Get();
        if (_timer >= 1000)
        {
            _timer = 0;
            game::Snapshot s = SnapshotOf(local);
            s.instanceKey = service.Presence().instanceKey;
            game::Snapshot changed = s;
            changed.x = changed.y = changed.z = changed.o = 0;
            changed.seq = 0;
            game::Snapshot before = _lastSent;
            before.x = before.y = before.z = before.o = 0;
            before.seq = 0;
            int64_t now = service.NetNow();
            bool jumped = _haveLastSent && Distance2D(s.x, s.y, _lastSent.x, _lastSent.y) > 8.0f && !(s.moveFlags & MOVEMENTFLAG_MASK_MOVING);
            // when anything in it changed, and again every 5 seconds whether it changed or not: the receiver sets its mirror to it
            if (!_haveLastSent || changed.Encode() != before.Encode() || jumped || now - _lastSentAt >= 5000)
            {
                s.seq = ++_seq;
                _lastSent = s;
                _lastSentAt = now;
                _haveLastSent = true;
                service.SayAllKeyed(nullptr, game::KeySnapshot, game::MsgSnapshot, s.Encode());
            }
        }
        if (_checkTimer >= 500)
        {
            _checkTimer = 0;
            // who gets a mirror, who loses it
            std::vector<Id128> remove;
            for (auto const& [id, mirror] : _mirrors)
            {
                PeerInfo* peer = service.FindPeer(id);
                Player* p = ObjectAccessor::FindPlayer(mirror.guid);
                if (!peer || !p || !p->IsInWorld() || p->GetMap() != local->GetMap() || !Wanted(*peer, mirror.snapshot, local) ||
                    p->GetDistance2d(local) > MirrorRangeLeave)
                    remove.push_back(id);
            }
            for (Id128 const& id : remove)
                Remove(id);
            for (auto const& [id, snap] : _snapshots)
            {
                if (_mirrors.count(id))
                    continue;
                PeerInfo* peer = service.FindPeer(id);
                int64_t now = service.NetNow();
                if (peer && Distance2D(snap.x, snap.y, local->GetPositionX(), local->GetPositionY()) < MirrorRange && _rejected[id] < now)
                {
                    if (!Wanted(*peer, snap, local, true))
                    {
                        _rejected[id] = now + 5000;   // (not asked again for a while: each check asks the game about the ground)
                        continue;
                    }
                    Create(*peer, snap, local);
                    if (_mirrors.size() >= MaxMirrors)
                        break;
                }
            }
        }
    }

    // the hook: the local player's own movement goes to those who can see it
    class MirrorMovement : public MovementHandlerScript
    {
    public:
        MirrorMovement() : MovementHandlerScript("P2PMirrorMovement") { }

        void OnPlayerMove(Player* player, MovementInfo movementInfo, uint32 opcode) override
        {
            Service& service = Service::Get();
            Player* local = service.Enabled() ? service.Local() : nullptr;
            if (!local || player->IsMirror() || player != local)
                return;
            WorldPacket data(static_cast<uint16>(opcode), 64);
            player->GetSession()->WriteMovementInfo(&data, &movementInfo);
            // (the packed identifier of the mover goes: the other side puts its mirror's)
            ObjectGuid ignored;
            data >> ignored.ReadAsPacked();
            Bytes payload(data.contents() + data.rpos(), data.contents() + data.size());
            Mirrors::Get().LocalMovement(player, static_cast<uint16_t>(opcode), payload, opcode == MSG_MOVE_HEARTBEAT);
        }
    };

    // the auras of the local player (and of its creatures) that land on a mirror are told to the mirror's owner
    class MirrorAuras : public UnitScript
    {
    public:
        MirrorAuras() : UnitScript("P2PMirrorAuras", true, { UNITHOOK_ON_AURA_APPLY }) { }
        void OnAuraApply(Unit* unit, Aura* aura) override
        {
            if (!Service::Get().Enabled() || !unit->IsPlayer() || !unit->ToPlayer()->IsMirror())
                return;
            Mirrors::Get().LocalAura(unit->ToPlayer(), aura);
        }
    };

    // D4, a measurement: N auras of a long spell made and destroyed on the player, with the timing hook of the module and without it (one thread; the hook's shards are for the contention
    // of many map threads, which this does not make)
    std::string TestAuraBench(Player* local, uint32 count)
    {
        if (!local || count == 0 || count > 200000)
            return "usage: .p2p test aurabench N (1 to 200000)";
        SpellInfo const* info = sSpellMgr->GetSpellInfo(1126);   // Mark of the Wild: 30 minutes
        if (!info)
            return "no spell 1126";
        auto run = [&]() -> double
        {
            auto start = std::chrono::steady_clock::now();
            for (uint32 i = 0; i < count; ++i)
            {
                if (Aura* aura = local->AddAura(1126, local))
                    aura->Remove();
            }
            return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / count;
        };
        auto hook = ::AuraTiming;
        run();   // (warm up)
        double with = 1e18, without = 1e18;
        for (int round = 0; round < 5; ++round)   // (alternating, and the best of each: the noise of a shared machine only adds)
        {
            ::AuraTiming = hook;
            with = std::min(with, run());
            ::AuraTiming = nullptr;
            without = std::min(without, run());
        }
        ::AuraTiming = hook;
        return Acore::StringFormat("aurabench {}: {:.2f} us per aura made and removed with the module's timing hook, {:.2f} us without ({:+.2f} us)", count, with, without, with - without);
    }

    void AddMirrors()
    {
        Service::Get().AddFeature(&Mirrors::Get());
        new MirrorMovement();
        new MirrorAuras();
        ::AuraTiming = &AuraTimingHook;
    }
}
