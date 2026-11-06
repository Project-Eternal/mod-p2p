/*
 * What other players do, shown as it happens: the packets that the player's own game sends about the player to those around it are forwarded
 * to the other servers, which show them on the mirror to their players. The core sends them through one function (Player::SendMessageToSetInRange, where the module's
 * hook PlayerBroadcast sees them); the ones of the list below are encoded with network identities in place of the guids of this server (the player itself, another
 * player by its character, a creature by its spawn key), and the receiving server makes them again with the guids it has and sends them to the players around the
 * mirror. A packet that names something the receiver does not have is dropped (a list of targets loses the entry only).
 *
 *   spell casts and their missiles (SMSG_SPELL_START, SMSG_SPELL_GO), the visuals of spells, heals and power given (the logs), attacking and being hit (the start and the
 *   end of an attack, the blow), emotes with their voice.
 *
 * The receiving server also gives the spell's credit effects (the kill credit of an event, shared by the party as the spell says) to its own player: the caster is the
 * mirror and the target the local creature, and the core's own rules decide who gets what. No other effect is run here: damage, healing and auras are the business of the
 * caster's server and the creature's decider (they arrive as events).
 *
 * A channel is told apart (it is part of the snapshot too, for a player who comes into sight during it): its fields are set on the mirror.
 */
#include "Mirrors.h"
#include "core/Authority.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "CreatureTextMgr.h"
#include "Map.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "GameTime.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SmartScriptMgr.h"
#include "SpellMgr.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "GameObjectAI.h"
#include "ObjectMgr.h"
#include "Vehicle.h"
#include "ConditionMgr.h"
#include "QuestDef.h"
#include "Group.h"
#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

AC_GAME_API extern void (*PlayerBroadcast)(Player const*, WorldPacket const*, bool);
AC_GAME_API extern bool (*ReactionAction)(Creature*, Unit*, uint32, bool);
AC_GAME_API extern bool (*ReactionSpellHit)(Spell*, Unit*);
AC_GAME_API extern void (*ReactionSpell)(Spell*, int);
AC_GAME_API extern bool (*ReactionEffect)(WorldObject const*, int);
AC_GAME_API extern bool (*EffectOnMirror)(Unit*, int, float, float, float, float, float);
AC_GAME_API extern void (*PacketHandling)(WorldSession*, WorldPacket const&, bool);
AC_GAME_API extern int (*ReactionEventGate)(SmartScriptHolder const&, int);
AC_COMMON_API extern thread_local bool g_seededRandomOn;
AC_COMMON_API extern thread_local uint64 g_seededRandomState;
AC_GAME_API extern bool (*ObjectBroadcast)(WorldObject const*, WorldPacket const*);
AC_GAME_API extern bool (*ObjectRadiusBroadcast)(WorldObject const*, WorldPacket const*, float);
AC_GAME_API extern bool (*CreatureTextOut)(Creature*, uint8, uint32, ChatMsg, Language, CreatureTextRange, TeamId, bool, WorldObject const*);
AC_GAME_API void CreatureTextSayExact(Creature*, uint8, uint32, ChatMsg, Language, CreatureTextRange, TeamId, bool, WorldObject const*);

namespace p2p
{
    bool WorldKeyOf(Creature const* c, EntityKey& key);                   // World.cpp
    Creature* WorldLookup(EntityKey const& key, Player* local);          // World.cpp
    bool WorldDecides(Creature const* c);                                 // World.cpp
    bool WorldTracked(Creature const* c);                                 // World.cpp
    bool WorldDeciderOf(Creature const* c, Id128& server);                // World.cpp

    namespace
    {
        enum Event : uint8_t { ChannelStart = 3, ChannelEnd = 4 };   // (values 1 and 2 are not used)
        enum How : uint8_t { Completed = 1, Cancelled = 2, Interrupted = 3 };
    }

    class Spells : public Feature
    {
    public:
        static Spells& Get()
        {
            static Spells spells;
            return spells;
        }

        void OnLogin(Player* player) override
        {
            _pending.clear();
            _channel = 0;
            _channelEnd = 0;
        }

        void OnLogout(Player* player) override { Flush(true); }

        static Id128 TargetId(ObjectGuid guid)
        {
            Service& s = Service::Get();
            if (Player* l = s.Local(); l && l->GetGUID() == guid)
                return s.Me().id;
            return s.Local() ? Mirrors::Get().PeerOf(guid) : Id128{};
        }

        // what the local player casts and channels, looked at each update (the maps are idle then)
        void Update(uint32_t diff, Player* local) override
        {
            Flush(false);
            RunDelayedHits(local);
            Service& service = Service::Get();
            Spell* channel = local->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
            uint32 channelId = channel && !channel->IsTriggered() ? channel->GetSpellInfo()->Id : 0;
            int64_t now = service.NetNow();
            if (channelId != _channel)
            {
                if (_channel)
                {
                    // how it ended: the whole length passed, or it was cancelled (by the caster) or interrupted (by something else)
                    How how = now >= _channelEnd - 300 ? Completed : (local->isMoving() || !local->IsAlive() ? Interrupted : Cancelled);
                    Writer w;
                    w.U8(ChannelEnd).U32(_channel).U8(how);
                    service.SayAll(nullptr, game::MsgSpell, w.data);
                }
                if (channelId)
                {
                    int32 length = channel->GetCastTimeRemaining();
                    _channelEnd = now + length;
                    Writer w;
                    w.U8(ChannelStart).U32(channelId).U32(length).Id(TargetId(channel->m_targets.GetUnitTargetGUID()));
                    service.SayAll(nullptr, game::MsgSpell, w.data);
                }
                _channel = channelId;
            }
        }

        void OnEvent(InEvent const& e, Player* local) override
        {
            // a peer that links gets the channel that is going on
            if (e.kind == InEvent::LinkUp && _channel && local)
            {
                int64_t left = std::max<int64_t>(0, _channelEnd - Service::Get().NetNow());
                if (left > 300)
                {
                    Writer w;
                    w.U8(ChannelStart).U32(_channel).U32(static_cast<uint32_t>(left)).Id(Id128{});
                    Service::Get().Say(e.peer, game::MsgSpell, w.data);
                }
                return;
            }
            if (e.kind == InEvent::Message && e.type == game::MsgPacket && local && local->IsInWorld())
            {
                OnPacket(e.peer, e.data, local);
                return;
            }
            if (e.kind == InEvent::Message && e.type == game::MsgEffect && local && local->IsInWorld())
            {
                OnEffect(e.peer, e.data, local);
                return;
            }
            if (e.kind == InEvent::Message && e.type == game::MsgCreaturePacket && local && local->IsInWorld())
            {
                OnCreaturePacket(e.peer, e.data, local);
                return;
            }
            if (e.kind == InEvent::Message && e.type == game::MsgCreatureText && local && local->IsInWorld())
            {
                OnCreatureText(e.peer, e.data, local);
                return;
            }
            if (e.kind != InEvent::Message || e.type != game::MsgSpell || !local || !local->IsInWorld())
                return;
            Reader r(e.data);
            uint8_t kind = r.U8();
            uint32 id = r.U32();
            Player* mirror = Mirrors::Get().MirrorOf(e.peer);
            SpellInfo const* info = r.ok() ? sSpellMgr->GetSpellInfo(id) : nullptr;
            if (!mirror || !info || _pending.size() > 64)
                return;
            switch (kind)
            {
                // (there are no cast events: the forwarded SMSG_SPELL_START and SMSG_SPELL_GO show the cast, the missile and the end)
                case ChannelStart:
                {
                    uint32 length = r.U32();
                    Id128 target = r.Id();
                    if (!r.ok() || length > 600000)
                        return;
                    EndChannel(mirror, e.peer, id);
                    Spell* spell = Make(mirror, info, target, local);
                    spell->SendChannelStart(length);   // (the spell's own SMSG_SPELL_GO is forwarded as a packet)
                    _pending.push_back({spell, e.peer, id, GameTime::GetGameTimeMS().count() + length, true});
                    break;
                }
                case ChannelEnd:
                    EndChannel(mirror, e.peer, id);
                    break;
            }
        }

        // ------------------------------------------------------------ the packets of the local player's game, forwarded
        enum TokenKind : uint8_t { TokenNone = 0, TokenSelf = 1, TokenPlayer = 2, TokenCreature = 3 };

        // the layout of a forwarded packet (see the top of the file): i the guid of the cast item or the caster (the caster), p a packed guid, g a guid, u a 32 bit number,
        // b a byte, h two bytes, f a float, s a string, T the targets of a spell, H the list of the targets hit, M the list of those missed, * the rest
        static char const* LayoutOf(uint16_t opcode)
        {
            switch (opcode)
            {
                case SMSG_SPELL_START:          return "ipbuuuT*";
                case SMSG_SPELL_GO:             return "ipbuuuHMT*";
                case SMSG_PLAY_SPELL_VISUAL:    return "gu";
                case SMSG_PLAY_SPELL_IMPACT:    return "gu";
                case SMSG_SPELLHEALLOG:         return "ppuuuubb";
                case SMSG_SPELLENERGIZELOG:     return "ppuuu";
                case SMSG_ATTACKSTART:          return "gg";
                case SMSG_ATTACKSTOP:           return "ppu";
                case SMSG_ATTACKERSTATEUPDATE:  return "upp*";
                case SMSG_EMOTE:                return "ug";
                case SMSG_TEXT_EMOTE:           return "guu*";
                case SMSG_PLAY_SOUND:           return "u";
                case SMSG_PLAY_OBJECT_SOUND:    return "ug";
                case SMSG_PLAY_MUSIC:           return "u";
                case SMSG_GAMEOBJECT_CUSTOM_ANIM: return "gu";
                case SMSG_SPELLNONMELEEDAMAGELOG: return "ppu*";
                default:                        return nullptr;
            }
        }

        // what only a creature's decider forwards (a player's sounds are its own, and the damage of its spells arrives as events)
        static bool CreatureOnly(uint16_t opcode) { return opcode == SMSG_PLAY_SOUND || opcode == SMSG_PLAY_OBJECT_SOUND || opcode == SMSG_PLAY_MUSIC || opcode == SMSG_GAMEOBJECT_CUSTOM_ANIM || opcode == SMSG_SPELLNONMELEEDAMAGELOG; }

        static bool WriteToken(ObjectGuid guid, Player* local, Writer& w, ObjectGuid self)
        {
            if (guid.IsEmpty())
            {
                w.U8(TokenNone);
                return true;
            }
            if (guid == self)
            {
                w.U8(TokenSelf);
                return true;
            }
            if (guid == local->GetGUID())
            {
                w.U8(TokenPlayer).Id(Service::Get().Me().id);   // (the local player, when the packet is about a creature)
                return true;
            }
            if (guid.IsPlayer())
            {
                Id128 id = Mirrors::Get().PeerOf(guid);
                if (id == Id128{})
                    return false;
                w.U8(TokenPlayer).Id(id);
                return true;
            }
            if (guid.IsCreatureOrVehicle())
            {
                Creature* c = local->GetMap()->GetCreature(guid);
                EntityKey key;
                if (!c || !WorldKeyOf(c, key))
                    return false;
                w.U8(TokenCreature);
                key.Write(w);
                return true;
            }
            return false;
        }

        static bool ReadToken(Reader& r, ObjectGuid self, Player* local, ObjectGuid& out)
        {
            uint8_t kind = r.U8();
            out = ObjectGuid::Empty;
            if (!r.ok())
                return false;
            switch (kind)
            {
                case TokenNone:
                    return true;
                case TokenSelf:
                    out = self;
                    return true;
                case TokenPlayer:
                {
                    Id128 id = r.Id();
                    if (!r.ok())
                        return false;
                    if (id == Service::Get().Me().id)
                    {
                        out = local->GetGUID();
                        return true;
                    }
                    Player* m = Mirrors::Get().MirrorOf(id);
                    if (!m)
                        return false;
                    out = m->GetGUID();
                    return true;
                }
                case TokenCreature:
                {
                    EntityKey key = EntityKey::Read(r);
                    Creature* c = r.ok() ? WorldLookup(key, local) : nullptr;
                    if (!c)
                        return false;
                    out = c->GetGUID();
                    return true;
                }
            }
            return false;
        }

        static void CopyFixed(WorldPacket& p, Writer& w, size_t n)
        {
            Bytes b(n);
            p.read(b.data(), n);
            w.Raw(b);
        }

        // the packet of the core in the form it travels in; false when it cannot be sent (something in it has no network identity, or it is not as expected)
        static bool Encode(uint16_t opcode, WorldPacket const& source, Player* local, Writer& w, ObjectGuid self)
        {
            char const* layout = LayoutOf(opcode);
            if (!layout)
                return false;
            WorldPacket p(source);
            p.rpos(0);
            try
            {
                for (char const* c = layout; *c; ++c)
                {
                    switch (*c)
                    {
                        case 'i':
                        {
                            ObjectGuid ignored;
                            p >> ignored.ReadAsPacked();
                            w.U8(TokenSelf);   // (an item cast: the caster, which is the local player here)
                            break;
                        }
                        case 'p':
                        {
                            ObjectGuid g;
                            p >> g.ReadAsPacked();
                            if (!WriteToken(g, local, w, self))
                                return false;
                            break;
                        }
                        case 'g':
                        {
                            ObjectGuid g;
                            p >> g;
                            if (!WriteToken(g, local, w, self))
                                return false;
                            break;
                        }
                        case 'u': CopyFixed(p, w, 4); break;
                        case 'b': CopyFixed(p, w, 1); break;
                        case 'h': CopyFixed(p, w, 2); break;
                        case 'f': CopyFixed(p, w, 4); break;
                        case 's':
                        {
                            std::string text;
                            p >> text;
                            w.Str(text);
                            break;
                        }
                        case 'T':
                        {
                            uint32 mask;
                            p >> mask;
                            w.U32(mask);
                            if (mask & (TARGET_FLAG_UNIT | TARGET_FLAG_CORPSE_ALLY | TARGET_FLAG_GAMEOBJECT | TARGET_FLAG_CORPSE_ENEMY | TARGET_FLAG_UNIT_MINIPET))
                            {
                                ObjectGuid g;
                                p >> g.ReadAsPacked();
                                if (!WriteToken(g, local, w, self))
                                    w.U8(TokenNone);   // (a target the receiver cannot have: the cast is shown without it)
                            }
                            if (mask & (TARGET_FLAG_ITEM | TARGET_FLAG_TRADE_ITEM))
                            {
                                ObjectGuid ignored;
                                p >> ignored.ReadAsPacked();   // (items are the player's own)
                            }
                            for (uint32 flag : {uint32(TARGET_FLAG_SOURCE_LOCATION), uint32(TARGET_FLAG_DEST_LOCATION)})
                                if (mask & flag)
                                {
                                    ObjectGuid transport;
                                    p >> transport.ReadAsPacked();
                                    if (!transport.IsEmpty())
                                        return false;   // (a place on a transport: not shown)
                                    CopyFixed(p, w, 12);
                                }
                            if (mask & TARGET_FLAG_STRING)
                            {
                                std::string text;
                                p >> text;
                                w.Str(text);
                            }
                            break;
                        }
                        case 'H':
                        {
                            uint8 n;
                            p >> n;
                            std::vector<Bytes> kept;
                            for (uint8 i = 0; i < n; ++i)
                            {
                                ObjectGuid g;
                                p >> g;
                                Writer one;
                                if (WriteToken(g, local, one, self))
                                    kept.push_back(one.data);
                            }
                            w.U8(static_cast<uint8_t>(kept.size()));
                            for (Bytes const& b : kept)
                                w.Raw(b);
                            break;
                        }
                        case 'M':
                        {
                            uint8 n;
                            p >> n;
                            std::vector<Bytes> kept;
                            for (uint8 i = 0; i < n; ++i)
                            {
                                ObjectGuid g;
                                uint8 condition, reflect = 0;
                                p >> g >> condition;
                                if (condition == SPELL_MISS_REFLECT)
                                    p >> reflect;
                                Writer one;
                                if (WriteToken(g, local, one, self))
                                {
                                    one.U8(condition);
                                    if (condition == SPELL_MISS_REFLECT)
                                        one.U8(reflect);
                                    kept.push_back(one.data);
                                }
                            }
                            w.U8(static_cast<uint8_t>(kept.size()));
                            for (Bytes const& b : kept)
                                w.Raw(b);
                            break;
                        }
                        case '*':
                        {
                            size_t rest = p.size() - p.rpos();
                            Bytes b(rest);
                            if (rest)
                                p.read(b.data(), rest);
                            w.Blob(b);
                            break;
                        }
                    }
                }
            }
            catch (ByteBufferException const&)
            {
                return false;
            }
            return true;
        }

        // the receiving side: the packet with the guids of this server, as the mirror's owner's game would have sent it
        static bool Decode(uint16_t opcode, Reader& r, ObjectGuid self, Player* local, WorldPacket& out)
        {
            char const* layout = LayoutOf(opcode);
            if (!layout)
                return false;
            for (char const* c = layout; *c; ++c)
            {
                switch (*c)
                {
                    case 'i':
                    case 'p':
                    {
                        ObjectGuid g;
                        if (!ReadToken(r, self, local, g))
                            return false;
                        out << g.WriteAsPacked();
                        break;
                    }
                    case 'g':
                    {
                        ObjectGuid g;
                        if (!ReadToken(r, self, local, g))
                            return false;
                        out << g;
                        break;
                    }
                    case 'u': out << r.U32(); break;
                    case 'b': out << r.U8(); break;
                    case 'h': out << r.U16(); break;
                    case 'f': out << r.F32(); break;
                    case 's': out << r.Str(512); break;
                    case 'T':
                    {
                        uint32 mask = r.U32();
                        out << mask;
                        if (mask & (TARGET_FLAG_UNIT | TARGET_FLAG_CORPSE_ALLY | TARGET_FLAG_GAMEOBJECT | TARGET_FLAG_CORPSE_ENEMY | TARGET_FLAG_UNIT_MINIPET))
                        {
                            ObjectGuid g;
                            if (!ReadToken(r, self, local, g))
                                g = ObjectGuid::Empty;
                            out << g.WriteAsPacked();
                        }
                        if (mask & (TARGET_FLAG_ITEM | TARGET_FLAG_TRADE_ITEM))
                            out << ObjectGuid::Empty.WriteAsPacked();
                        for (uint32 flag : {uint32(TARGET_FLAG_SOURCE_LOCATION), uint32(TARGET_FLAG_DEST_LOCATION)})
                            if (mask & flag)
                            {
                                out << ObjectGuid::Empty.WriteAsPacked();
                                out << r.F32() << r.F32() << r.F32();
                            }
                        if (mask & TARGET_FLAG_STRING)
                            out << r.Str(512);
                        break;
                    }
                    case 'H':
                    {
                        uint8 n = r.U8();
                        std::vector<ObjectGuid> hits;
                        for (uint8 i = 0; i < n && r.ok(); ++i)
                        {
                            ObjectGuid g;
                            if (ReadToken(r, self, local, g) && !g.IsEmpty())
                                hits.push_back(g);
                        }
                        out << uint8(hits.size());
                        for (ObjectGuid const& g : hits)
                            out << g;
                        break;
                    }
                    case 'M':
                    {
                        uint8 n = r.U8();
                        Bytes entries;
                        uint8 count = 0;
                        WorldPacket tmp;
                        for (uint8 i = 0; i < n && r.ok(); ++i)
                        {
                            ObjectGuid g;
                            bool ok = ReadToken(r, self, local, g) && !g.IsEmpty();
                            uint8 condition = r.U8(), reflect = condition == SPELL_MISS_REFLECT ? r.U8() : 0;
                            if (ok)
                            {
                                tmp << g << condition;
                                if (condition == SPELL_MISS_REFLECT)
                                    tmp << reflect;
                                ++count;
                            }
                        }
                        out << count;
                        if (tmp.size())
                            out.append(tmp.contents(), tmp.size());
                        break;
                    }
                    case '*':
                    {
                        Bytes rest = r.Blob(1 << 14);
                        if (!rest.empty())
                            out.append(rest.data(), rest.size());
                        break;
                    }
                }
            }
            return r.ok();
        }

        // ---- the hook of the core, on whatever thread the player's game sends a packet to the set around it
        static constexpr int64_t ReorderMs = 300;   // replayed acts and spell hits wait this long after their network time, and run in the order of their times
        struct Launch { uint32_t counter; int64_t time; };
        static inline std::atomic<uint32_t> _actCounter{0};   // the act numbers of this player: its acts and its spells
        static inline std::mutex _launchMutex;
        static inline std::unordered_map<Spell const*, Launch> _launch;   // by the spell object: two casts of one spell in flight keep their own numbers
        static inline std::atomic<int> _launchCount{0};
        thread_local static inline Spell const* _goSpell = nullptr;      // the spell whose go packet is being sent

        static Launch LaunchOf(Spell const* spell)
        {
            std::lock_guard<std::mutex> lock(_launchMutex);
            auto it = _launch.find(spell);
            return it == _launch.end() ? Launch{0, 0} : it->second;
        }

        static void Broadcast(Player const* player, WorldPacket const* data, bool)
        {
            uint16_t opcode = data->GetOpcode();
            if (!LayoutOf(opcode) || CreatureOnly(opcode))
                return;
            Service& service = Service::Get();
            if (!service.Enabled() || player->IsMirror())
                return;
            Player* local = service.Local();
            if (!local || player != local)
                return;
            Bytes body(data->contents(), data->contents() + data->size());
            // a spell is an act like the others: its number and its network time are taken when it leaves, here, so that the answer of the creature it hits on this
            // server (which comes in the same cast) and the replays on the other servers use the same ones
            uint32_t counter = 0;
            int64_t launched = 0;
            if (opcode == SMSG_SPELL_GO)
            {
                try
                {
                    WorldPacket p(*data);
                    p.rpos(0);
                    ObjectGuid ignored;
                    uint8 count;
                    uint32 spell;
                    p >> ignored.ReadAsPacked() >> ignored.ReadAsPacked() >> count >> spell;
                    counter = ++_actCounter;
                    launched = service.NetNow();
                    if (_goSpell)
                    {
                        std::lock_guard<std::mutex> lock(_launchMutex);
                        _launch[_goSpell] = {counter, launched};
                        _launchCount.store(int(_launch.size()), std::memory_order_relaxed);
                    }
                }
                catch (ByteBufferException const&)
                {
                    counter = 0;
                }
            }
            service.Defer([opcode, body, counter, launched]
            {
                Service& svc = Service::Get();
                Player* l = svc.Local();
                if (!l || !l->IsInWorld())
                    return;
                WorldPacket packet(opcode);
                packet.append(body.data(), body.size());
                Writer w;
                w.U16(opcode);
                if (!Encode(opcode, packet, l, w, l->GetGUID()))
                    return;
                if (counter)
                    w.U32(counter).I64(launched);   // (after the layout: the receiver of a spell go reads them)
                uint32_t map = l->GetMapId();
                uint32_t phase = l->GetPhaseMask();
                uint64_t instance = svc.Presence().instanceKey;
                float x = l->GetPositionX(), y = l->GetPositionY();
                // the servers whose players are around, not every one on the map (a continent holds players far apart)
                svc.SayAll([=](PeerInfo const& p)
                {
                    float dx = p.presence.x - x, dy = p.presence.y - y;
                    return p.presence.map == map && p.presence.instanceKey == instance && (p.presence.phaseMask & phase) != 0 && dx * dx + dy * dy < 260.0f * 260.0f;
                }, game::MsgPacket, w.data);
            });
        }

        // the credit effects of a spell that another player cast: the caster is the mirror, the targets are the local ones; the core shares what the spell says to share
        void ShareCredit(Player* caster, SpellInfo const* info, Player* local)
        {
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            {
                if (info->Effects[i].Effect != SPELL_EFFECT_KILL_CREDIT2)
                    continue;
                int32 entry = info->Effects[i].MiscValue;
                if (!entry && info->Id == 42793)
                    entry = 24008;   // (as the core's own handler)
                if (!entry)
                    continue;
                uint32 target = info->Effects[i].TargetA.GetTarget();
                if (target == TARGET_UNIT_CASTER)
                    caster->RewardPlayerAndGroupAtEvent(entry, caster);   // (the group of the caster around it: the local player if it is in it and near)
                else if ((target == TARGET_UNIT_CASTER_AREA_PARTY || target == TARGET_UNIT_LASTTARGET_AREA_PARTY || target == TARGET_UNIT_TARGET_PARTY) &&
                    caster->GetGroup() && caster->GetGroup() == local->GetGroup() && local->IsAlive() && local->IsInMap(caster) && local->IsWithinDist(caster, 100.0f))
                    local->RewardPlayerAndGroupAtEvent(entry, local);
            }
        }

        // A heal or power that another player's caster gave to the local player's character: the caster's server says it in the log it sends, and the
        // owner applies it to the real character when the game would allow it at this moment: the caster's mirror may assist the player (not a hostile one, not at war), the
        // player lives, the spell's range from the mirror (with a margin for the delay of the positions). Through the core's own healing, so that the game shows the heal.
        void ApplyEffect(uint16_t opcode, WorldPacket const& decoded, Player* mirror, Player* local)
        {
            WorldPacket p(decoded);
            p.rpos(0);
            try
            {
                ObjectGuid victim, caster;
                uint32 spell, a, b, c = 0;
                p >> victim.ReadAsPacked() >> caster.ReadAsPacked() >> spell >> a >> b;
                if (opcode == SMSG_SPELLHEALLOG)
                    p >> c;   // (heal, overheal, absorb)
                if (victim != local->GetGUID() || caster != mirror->GetGUID())
                    return;
                SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
                char const* what = opcode == SMSG_SPELLHEALLOG ? "heal" : "power";
                float range = info ? info->GetMaxRange(true, mirror) : 0.0f;
                if (!info || !local->IsAlive() || !mirror->IsValidAssistTarget(local) || mirror->GetExactDist(local) > std::max(range, 5.0f) + 15.0f)
                {
                    LOG_INFO("module", "[P2P] Refused a {} of {} from {}: not a valid target now", what, spell, mirror->GetName());
                    return;
                }
                if (opcode == SMSG_SPELLHEALLOG)
                {
                    if (a > local->GetMaxHealth() * 2u)
                        return;
                    Unit::DealHeal(mirror, local, a);   // (a = the heal: the log's second number)
                    LOG_INFO("module", "[P2P] {} healed {} for {} (spell {})", mirror->GetName(), local->GetName(), a, spell);
                }
                else if (c <= 1000000)
                {
                    // power: the log is victim, caster, spell, power type, amount: here a = type and b = amount
                    local->ModifyPower(Powers(a), int32(b), false);
                    LOG_INFO("module", "[P2P] {} gave {} power {} (spell {})", mirror->GetName(), local->GetName(), b, spell);
                }
            }
            catch (ByteBufferException const&)
            {
            }
        }

        void OnPacket(Id128 const& peer, Bytes const& data, Player* local)
        {
            Reader r(data);
            uint16_t opcode = r.U16();
            Player* mirror = Mirrors::Get().MirrorOf(peer);
            if (!r.ok() || !mirror || !mirror->IsInWorld())
                return;
            WorldPacket out(opcode);
            if (!Decode(opcode, r, mirror->GetGUID(), local, out))
            {
                if (opcode == SMSG_SPELL_GO)
                    LOG_INFO("module", "[P2P] A spell go of {} could not be decoded", mirror->GetName());
                return;
            }
            uint32_t counter = 0;
            int64_t launched = 0;
            if (opcode == SMSG_SPELL_GO)
            {
                counter = r.U32();
                launched = r.I64();
                int64_t now = Service::Get().NetNow();
                if (!r.ok())
                    counter = 0;
                if (!counter || launched > now + 5000 || launched < now - 60000)
                    launched = now;
            }
            mirror->SendMessageToSet(&out, false);
            if (opcode == SMSG_SPELLHEALLOG || opcode == SMSG_SPELLENERGIZELOG)
                ApplyEffect(opcode, out, mirror, local);
            if (opcode == SMSG_SPELL_GO)
            {
                // the spell's id follows the two guids, the cast count and precedes the flags
                WorldPacket p(out);
                p.rpos(0);
                try
                {
                    ObjectGuid ignored;
                    uint8 count;
                    uint32 spell;
                    p >> ignored.ReadAsPacked() >> ignored.ReadAsPacked() >> count >> spell;
                    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell))
                    {
                        LOG_INFO("module", "[P2P] Spell {} (act {}) of {} arrives for the replay of its hits", spell, counter, mirror->GetName());
                        ShareCredit(mirror, info, local);
                        ReplayHits(out, mirror, info, counter, launched);
                    }
                }
                catch (ByteBufferException const&)
                {
                }
            }
        }

        // ---- creatures: the server that decides a creature forwards what the creature shows, the others show it through their copy
        thread_local static inline bool _replaying = false;

        // the hook of the core for the packets of an object that is not a player, on whatever thread; false: not delivered here
        // what a game object's sound or animation does here now (the one that decides tells, copies show nothing of their own):
        // the script of a creature that this server decides runs: shown here and told; the script of a puppet runs (the replay of another server's act): shown by the decider
        // and nothing here; no creature script (a player's act that every server replays, or the object's own behavior that every server runs): every server shows its own, nobody tells
        enum class ObjectShow { Own, Tell, Hide };
        static ObjectShow GameObjectShows()
        {
            Creature const* me = _actionMe.empty() ? nullptr : _actionMe.back();
            if (!me)
                return ObjectShow::Own;
            return me->IsPuppet() ? ObjectShow::Hide : ObjectShow::Tell;
        }

        // a game object's own sound or animation: shown by the server whose game made it (not inside the scope of a player's act, which every server replays and shows
        // itself), and told to the servers near; the object is the same spawn on every server
        static bool GameObjectHook(GameObject const* go, WorldPacket const* data)
        {
            uint16_t opcode = data->GetOpcode();
            Service& service = Service::Get();
            if (!service.Enabled() || _replaying || !go->GetSpawnId() || !CreatureOnly(opcode) || opcode == SMSG_SPELLNONMELEEDAMAGELOG)
                return true;
            ObjectShow show = GameObjectShows();
            LOG_INFO("module", "[P2P] Object spawn {} (entry {}) shows {}: {}", go->GetSpawnId(), go->GetEntry(), opcode == SMSG_GAMEOBJECT_CUSTOM_ANIM ? "an animation" : opcode == SMSG_PLAY_MUSIC ? "music" : "a sound",
                show == ObjectShow::Tell ? "told to the servers near" : show == ObjectShow::Hide ? "not shown here, the server that decides the creature shows it" : "shown by this server only");
            if (show == ObjectShow::Hide)
                return false;
            if (show == ObjectShow::Own)
                return true;
            ObjectGuid guid = go->GetGUID();
            Bytes body(data->contents(), data->contents() + data->size());
            service.Defer([opcode, body, guid]
            {
                Service& svc = Service::Get();
                Player* l = svc.Local();
                GameObject* o = l && l->IsInWorld() ? l->GetMap()->GetGameObject(guid) : nullptr;
                if (!o)
                    return;
                EntityKey key;
                key.kind = 2;
                key.map = o->GetMapId();
                key.instance = svc.Presence().instanceKey;
                key.spawn = static_cast<uint32_t>(o->GetSpawnId());
                WorldPacket packet(opcode);
                packet.append(body.data(), body.size());
                Writer w;
                key.Write(w);
                w.U16(opcode);
                if (!Encode(opcode, packet, l, w, guid))
                    return;
                SayNear(o->GetPositionX(), o->GetPositionY(), o->GetMapId(), o->GetPhaseMask(), 260.0f, w.data);
            });
            return true;
        }

        // the servers whose players are within a distance of a place on a map
        static void SayNear(float x, float y, uint32_t map, uint32_t phase, float distance, Bytes const& data)
        {
            Service& svc = Service::Get();
            uint64_t instance = svc.Presence().instanceKey;
            svc.SayAll([=](PeerInfo const& p)
            {
                float dx = p.presence.x - x, dy = p.presence.y - y;
                return p.presence.map == map && p.presence.instanceKey == instance && (p.presence.phaseMask & phase) != 0 && dx * dx + dy * dy < distance * distance;
            }, game::MsgCreaturePacket, data);
        }

        // a sound or a music for the players within a radius of an object (a creature's sound of the zone's range does not go through the creature text): the decider of a
        // creature tells the others, which play it to their own players within the radius; a copy plays nothing itself
        static bool RadiusHook(WorldObject const* object, WorldPacket const* data, float radius)
        {
            Service& service = Service::Get();
            if (!service.Enabled() || _replaying || !object->IsInWorld())
                return true;
            Creature const* creature = object->ToCreature();
            if (creature)
            {
                if (creature->IsPuppet())
                    return false;
                if (!WorldTracked(creature))
                    return true;
            }
            else if (object->IsGameObject())
            {
                ObjectShow show = GameObjectShows();
                if (show == ObjectShow::Hide)
                    return false;
                if (show == ObjectShow::Own || !object->ToGameObject()->GetSpawnId())
                    return true;
            }
            else
                return true;
            uint16_t opcode = data->GetOpcode();
            if (!LayoutOf(opcode))
                return true;
            ObjectGuid guid = object->GetGUID();
            Bytes body(data->contents(), data->contents() + data->size());
            service.Defer([opcode, body, guid, radius]
            {
                Service& svc = Service::Get();
                Player* l = svc.Local();
                WorldObject* o = nullptr;
                if (l && l->IsInWorld())
                    o = guid.IsGameObject() ? static_cast<WorldObject*>(l->GetMap()->GetGameObject(guid)) : l->GetMap()->GetCreature(guid);
                if (!o)
                    return;
                EntityKey key;
                if (Creature* c = o->ToCreature())
                {
                    if (!WorldDecides(c) || !WorldKeyOf(c, key))
                        return;
                }
                else
                {
                    key.kind = 2;
                    key.map = o->GetMapId();
                    key.instance = svc.Presence().instanceKey;
                    key.spawn = static_cast<uint32_t>(o->ToGameObject()->GetSpawnId());
                }
                WorldPacket packet(opcode);
                packet.append(body.data(), body.size());
                Writer w;
                key.Write(w);
                w.U16(uint16_t(opcode | RadiusFlag));
                w.F32(radius);
                if (!Encode(opcode, packet, l, w, guid))
                    return;
                SayNear(o->GetPositionX(), o->GetPositionY(), o->GetMapId(), o->GetPhaseMask(), radius + 100.0f, w.data);
            });
            return true;
        }

        static constexpr uint16_t RadiusFlag = 0x8000;   // (set in the opcode of a forwarded packet that goes to the players within a radius, which follows)

        static bool ObjectHook(WorldObject const* object, WorldPacket const* data)
        {
            if (object->IsGameObject())
                return GameObjectHook(object->ToGameObject(), data);
            if (!object->IsCreature())
                return true;
            uint16_t opcode = data->GetOpcode();
            if (!LayoutOf(opcode))
                return true;
            Service& service = Service::Get();
            Creature const* creature = object->ToCreature();
            if (!service.Enabled() || _replaying)
                return true;
            // a copy of a creature that another server decides shows nothing of its own: what it shows comes from there
            if (creature->IsPuppet())
                return false;
            if (!WorldTracked(creature))
                return true;
            ObjectGuid guid = creature->GetGUID();
            Bytes body(data->contents(), data->contents() + data->size());
            service.Defer([opcode, body, guid]
            {
                Service& svc = Service::Get();
                Player* l = svc.Local();
                Creature* c = l && l->IsInWorld() ? l->GetMap()->GetCreature(guid) : nullptr;
                if (!c || !WorldDecides(c))
                    return;
                EntityKey key;
                if (!WorldKeyOf(c, key))
                    return;
                WorldPacket packet(opcode);
                packet.append(body.data(), body.size());
                Writer w;
                key.Write(w);
                w.U16(opcode);
                if (!Encode(opcode, packet, l, w, guid))
                    return;
                float x = c->GetPositionX(), y = c->GetPositionY();
                uint32_t map = c->GetMapId(), phase = c->GetPhaseMask();
                uint64_t instance = svc.Presence().instanceKey;
                svc.SayAll([=](PeerInfo const& p)
                {
                    float dx = p.presence.x - x, dy = p.presence.y - y;
                    return p.presence.map == map && p.presence.instanceKey == instance && (p.presence.phaseMask & phase) != 0 && dx * dx + dy * dy < 260.0f * 260.0f;
                }, game::MsgCreaturePacket, w.data);
            });
            return true;
        }

        // what a creature says: tell the others (a line to one player only to that player's server), and a copy says nothing itself
        static bool TextHook(Creature* source, uint8 group, uint32 id, ChatMsg type, Language lang, CreatureTextRange range, TeamId team, bool gmOnly, WorldObject const* target)
        {
            Service& service = Service::Get();
            if (!service.Enabled() || _replaying)
                return true;
            if (source->IsPuppet())
                return false;
            if (!WorldTracked(source))
                return true;
            ObjectGuid guid = source->GetGUID();
            ObjectGuid targetGuid = target ? target->GetGUID() : ObjectGuid::Empty;
            service.Defer([=]
            {
                Service& svc = Service::Get();
                Player* l = svc.Local();
                Creature* c = l && l->IsInWorld() ? l->GetMap()->GetCreature(guid) : nullptr;
                if (!c || !WorldDecides(c))
                    return;
                EntityKey key;
                if (!WorldKeyOf(c, key))
                    return;
                Writer w;
                key.Write(w);
                w.U8(group).U32(id).U8(static_cast<uint8_t>(type)).U32(static_cast<uint32_t>(lang)).U8(static_cast<uint8_t>(range)).U8(static_cast<uint8_t>(team)).U8(gmOnly ? 1 : 0);
                if (!WriteToken(targetGuid, l, w, guid))
                    return;
                bool whisper = type == CHAT_MSG_MONSTER_WHISPER || type == CHAT_MSG_RAID_BOSS_WHISPER;
                // a line for one player goes to that player's server
                if (whisper && range == TEXT_RANGE_NORMAL)
                {
                    if (targetGuid.IsPlayer())
                    {
                        Id128 peer = Mirrors::Get().PeerOf(targetGuid);
                        if (!(peer == Id128{}))
                            svc.Say(peer, game::MsgCreatureText, w.data);
                    }
                    return;
                }
                float x = c->GetPositionX(), y = c->GetPositionY();
                uint32_t map = c->GetMapId(), zone = c->GetZoneId(), area = c->GetAreaId();
                uint64_t instance = svc.Presence().instanceKey;
                float reach = range == TEXT_RANGE_NORMAL ? (type == CHAT_MSG_MONSTER_YELL ? 300.0f : 100.0f) : 0.0f;
                svc.SayAll([=](PeerInfo const& p)
                {
                    if (range != TEXT_RANGE_WORLD && (p.presence.map != map || p.presence.instanceKey != instance))
                        return false;
                    switch (range)
                    {
                        case TEXT_RANGE_AREA: return p.presence.area == area;
                        case TEXT_RANGE_ZONE: return p.presence.zone == zone;
                        case TEXT_RANGE_MAP:
                        case TEXT_RANGE_WORLD: return true;
                        default:
                        {
                            float dx = p.presence.x - x, dy = p.presence.y - y;
                            return dx * dx + dy * dy < (reach + 60.0f) * (reach + 60.0f);
                        }
                    }
                }, game::MsgCreatureText, w.data);
            });
            return true;
        }

        void OnCreaturePacket(Id128 const& peer, Bytes const& data, Player* local)
        {
            Reader r(data);
            EntityKey key = EntityKey::Read(r);
            uint16_t opcode = r.U16();
            bool radial = (opcode & RadiusFlag) != 0;
            opcode &= ~RadiusFlag;
            float radius = radial ? r.F32() : 0.0f;
            if (!r.ok() || radius < 0.0f || radius > 1000.0f)
                return;
            WorldObject* source = nullptr;
            if (key.kind == 2)
            {
                // a game object: the same spawn on every server
                if (key.map == local->GetMapId())
                {
                    auto range = local->GetMap()->GetGameObjectBySpawnIdStore().equal_range(key.spawn);
                    if (range.first != range.second)
                        source = range.first->second;
                }
            }
            else if (Creature* c = WorldLookup(key, local); c && c->IsPuppet())
                source = c;
            if (!source || !source->IsInWorld())
                return;
            WorldPacket out(opcode);
            if (!Decode(opcode, r, source->GetGUID(), local, out))
                return;
            if (CreatureOnly(opcode) && opcode != SMSG_SPELLNONMELEEDAMAGELOG)
                LOG_INFO("module", "[P2P] {} of {} {} from another server is shown here{}", opcode == SMSG_GAMEOBJECT_CUSTOM_ANIM ? "animation" : opcode == SMSG_PLAY_MUSIC ? "music" : "sound",
                    key.kind == 2 ? "object spawn" : "creature", key.kind == 2 ? key.spawn : source->GetEntry(), radial ? Acore::StringFormat(" within {:.0f} yards", radius) : std::string());
            _replaying = true;
            if (radial)
                SendWithin(source, out, radius);
            else
                source->SendMessageToSet(&out, false);
            _replaying = false;
        }

        // the packet to this server's players within a radius of an object, as the core's radius sounds do
        static void SendWithin(WorldObject* source, WorldPacket const& packet, float radius)
        {
            std::vector<Player*> targets;
            Acore::AnyPlayerInObjectRangeCheck check(source, radius, false);
            Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(source, targets, check);
            Cell::VisitObjects(source, searcher, radius);
            for (Player* player : targets)
                player->SendDirectMessage(&packet);
        }

        void OnCreatureText(Id128 const& peer, Bytes const& data, Player* local)
        {
            Reader r(data);
            EntityKey key = EntityKey::Read(r);
            uint8 group = r.U8();
            uint32 id = r.U32();
            ChatMsg type = static_cast<ChatMsg>(r.U8());
            Language lang = static_cast<Language>(r.U32());
            CreatureTextRange range = static_cast<CreatureTextRange>(r.U8());
            TeamId team = static_cast<TeamId>(r.U8());
            bool gmOnly = r.U8() != 0;
            ObjectGuid target;
            Creature* c = r.ok() ? WorldLookup(key, local) : nullptr;
            if (!c || !c->IsInWorld() || !c->IsPuppet() || !ReadToken(r, c->GetGUID(), local, target))
                return;
            WorldObject* targetObject = nullptr;
            if (!target.IsEmpty())
            {
                targetObject = target.IsPlayer() ? static_cast<WorldObject*>(ObjectAccessor::FindPlayer(target)) : c->GetMap()->GetCreature(target);
                // (a line aimed at a creature or player this server does not have is said without its target, unless it is a whisper: that one is for the target only)
                bool whisper = type == CHAT_MSG_MONSTER_WHISPER || type == CHAT_MSG_RAID_BOSS_WHISPER;
                if (!targetObject && whisper)
                    return;
            }
            _replaying = true;
            CreatureTextSayExact(c, group, id, type, lang, range, team, gmOnly, targetObject);
            _replaying = false;
        }

        // ---- the reaction scope: the reaction of the game to another player's act is evaluated here, with the mirror as the actor. What an effect changes
        // that another server owns (a character, or a creature that another server decides) is ignored here: its owner runs the same reaction and sends the result.
        // The scope has a role: Replay (the act of another player, evaluated here) or Acting (the act of this server's own player). A stack of the roles entered on this
        // thread; the top is the current one. A spell made while a scope is open carries the role of that scope, and opens it again whenever its effects run (a spell
        // with a travel time, a trigger), so the effects of what a script cast are filtered like the script itself.
        enum Role : int { RoleNone = 0, RoleReplay = 1, RoleActing = 2 };
        thread_local static inline std::vector<int> _roles;
        static int CurrentRole() { return _roles.empty() ? int(RoleNone) : _roles.back(); }
        struct Scope
        {
            explicit Scope(int role) { _roles.push_back(role); }
            ~Scope() { _roles.pop_back(); }
        };

        // ---- the scope of an act: the role, the act's network time (the script cooldowns are measured in it) and the random numbers of the thread seeded by
        // the act's identity, so that every server that evaluates the act draws the same numbers
        struct ActFrame { int64_t time; bool wasOn; uint64 wasState; };
        thread_local static inline std::vector<ActFrame> _frames;
        static int64_t ActTime() { return _frames.empty() ? 0 : _frames.back().time; }
        static void SeedPush(int64_t time, uint64 seed)
        {
            _frames.push_back({time, g_seededRandomOn, g_seededRandomState});
            g_seededRandomOn = true;
            g_seededRandomState = seed;
        }
        static void SeedPop()
        {
            if (_frames.empty())
                return;
            g_seededRandomOn = _frames.back().wasOn;
            g_seededRandomState = _frames.back().wasState;
            _frames.pop_back();
        }
        struct ActScope
        {
            ActScope(int role, int64_t time, uint64 seed) : _scope(role) { SeedPush(time, seed); }
            ~ActScope() { SeedPop(); }
            Scope _scope;
        };
        static uint64 SeedOf(Id128 const& id, uint64 a, uint64 b)
        {
            uint64 h = 0xcbf29ce484222325ull;
            for (uint8_t byte : id)
                h = (h ^ byte) * 0x100000001b3ull;
            h ^= a * 0x9E3779B97F4A7C15ull;
            h = (h ^ (h >> 29)) * 0xBF58476D1CE4E5B9ull;
            h ^= b * 0xD6E8FEB86659FD93ull;
            return h ^ (h >> 32);
        }

        // The cooldown of a script event that a reaction sets off (a creature that answers a hit once every 15 seconds) is counted in the network time of the acts: it
        // fires for an act if the act's time is at least the cooldown after the time of the act it last fired for. Every server then answers acts that reach it in the
        // same order alike. Outside an act the script's own timer rules (what 0: the event may fire although its timer counts, or may not; what 3: it fired).
        static inline std::mutex _gateMutex;
        static inline std::unordered_map<void const*, int64_t> _gateUntil;
        static int EventGate(SmartScriptHolder const& e, int what)
        {
            int64_t t = ActTime();
            if (!t)
                return 0;
            std::lock_guard<std::mutex> lock(_gateMutex);
            if (what == 3)
            {
                if (e.timer)
                {
                    _gateUntil[&e] = t + e.timer;
                    if (_gateUntil.size() > 8192)
                        for (auto it = _gateUntil.begin(); it != _gateUntil.end();)
                            it = it->second < t - 600000 ? _gateUntil.erase(it) : std::next(it);
                }
                else
                    _gateUntil.erase(&e);
                return 0;
            }
            auto it = _gateUntil.find(&e);
            if (it == _gateUntil.end())
                return 0;
            return t >= it->second ? 1 : 2;
        }

        static inline std::mutex _markMutex;
        static inline std::unordered_map<Spell const*, int> _marks;
        static inline std::atomic<int> _markCount{0};
        static int MarkOf(Spell const* spell)
        {
            if (!_markCount.load(std::memory_order_relaxed))
                return RoleNone;
            std::lock_guard<std::mutex> lock(_markMutex);
            auto it = _marks.find(spell);
            return it == _marks.end() ? int(RoleNone) : it->second;
        }

        // the script actions (SMART_ACTION_*) of a creature that this server does not decide: only the credit, and what steers the script (phases, timers, counters, data)
        static bool KeptOnPuppet(uint32 type)
        {
            switch (type)
            {
                case 33: case 26: case 15:                                            // credit: CALL_KILLEDMONSTER, CALL_GROUPEVENTHAPPENS, CALL_AREAEXPLOREDOREVENTHAPPENS
                case 22: case 23: case 30: case 31: case 67: case 73: case 74: case 125:   // event phases and timed events
                case 80: case 87: case 88: case 64: case 63: case 45: case 242: case 225: // timed action lists, stored targets, counters, data
                case 223: case 76: case 77: case 78: case 211: case 100:
                    return true;
                default:
                    return false;   // (not classified is synchronized: ignored, so that an unexamined action can never act twice)
            }
        }

        // the actions that act on the character of the invoker or on its client: run for the server's own player (where it acts), ignored on a mirror (the owner runs them)
        static bool ActorOnly(uint32 type)
        {
            switch (type)
            {
                case 56: case 57: case 6: case 7: case 98: case 240: case 72: case 68: case 135: case 220:
                case 134:                                                              // INVOKER_CAST: the player's own cast, made where the player is
                    return true;
                default:
                    return false;
            }
        }

        // the actions that change the world: only the authority of the source runs them (the decider of the creature; for the script of an object, an area trigger or a
        // spell, the server of the acting player)
        static bool WorldAction(uint32 type)
        {
            switch (type)
            {
                case 12: case 107: case 219: case 221: case 228: case 50: case 241: case 226:
                case 131: case 132: case 133: case 70: case 41: case 126: case 9: case 32:
                case 99: case 118: case 202: case 104: case 105: case 106: case 34: case 35:
                case 112: case 111: case 237: case 58: case 214: case 215:
                    return true;
                default:
                    return false;
            }
        }

        // Every script action of every creature asks here (the core's ReactionAction, at its beginning and its end). The rule depends on the script's own situation, not
        // only on a scope:
        // * a creature that another server decides (a puppet) runs only the credit and the flow of its script, always: its timed action lists, its out-of-combat timers and
        //   what it sees by itself (a player coming near) included. What it says, casts, summons or sets is its decider's (a creature's casts are synchronized: a puppet casts nothing);
        // * the actions of the player's own character (items, quests, dialogs, the invoker's cast) run for this server's own player and not for a mirror;
        // * on a creature this server decides (or a script with no creature), an action whose invoker is a mirror runs in the Replay role, one whose invoker is this
        //   server's own player in the Acting role, unless a scope is open already: what the action casts or sets is then filtered by the unit it reaches.
        thread_local static inline std::vector<char> _actionPushed;
        thread_local static inline std::vector<Creature const*> _actionMe;   // the creature whose script action is running (null: a script with no creature)
        static bool ActionHook(Creature* me, Unit* invoker, uint32 type, bool begin)
        {
            if (begin)
                _actionMe.push_back(me);
            else if (!_actionMe.empty())
                _actionMe.pop_back();
            if (!begin)
            {
                if (!_actionPushed.empty())
                {
                    if (_actionPushed.back())
                        _roles.pop_back();
                    _actionPushed.pop_back();
                }
                return true;
            }
            int role = CurrentRole();
            bool pushed = false;
            bool allowed = true;
            Player* player = invoker ? invoker->ToPlayer() : nullptr;
            bool mirrorInvoker = player && player->IsMirror();
            if (ActorOnly(type))
            {
                Creature* c = invoker ? invoker->ToCreature() : nullptr;
                allowed = !mirrorInvoker && !(c && c->IsPuppet());   // (a puppet's script gives the item to this server's own player, too: nobody else does)
            }
            else if (me && me->IsPuppet())
                allowed = KeptOnPuppet(type);
            else if (!me && role == RoleReplay && WorldAction(type))
                allowed = false;         // the script of an object, an area trigger or a spell: the acting server changes the world
            else if (role == RoleNone && player)
            {
                _roles.push_back(mirrorInvoker ? int(RoleReplay) : int(RoleActing));
                pushed = true;
            }
            _actionPushed.push_back(pushed ? 1 : 0);
            return allowed;
        }

        // The effect seams of the core: an effect is not applied to a unit whose owner is another server while the reaction to an act is evaluated; the owner
        // runs the same reaction and sends the result. kind: 1 aura, 2 power, 3 health, 4 faction, 5 movement, 6 teleport, 7 summon (the summoner), 8 object spawned.
        // Outside a scope everything is applied (a fight is not a reaction: sections 2 and 6).
        static bool EffectHook(WorldObject const* object, int kind)
        {
            int role = CurrentRole();
            if (role == RoleNone)
                return true;
            if (kind == 7 || kind == 8)
            {
                // the world changes on one server: the acting server for an act of its own player (but not through a copy of a creature), and for the act of another
                // player the server that decides the creature whose script runs
                Creature const* c = object ? object->ToCreature() : nullptr;
                if (c && c->IsPuppet())
                    return false;
                if (role == RoleReplay)
                    return c != nullptr;
                return true;
            }
            if (kind == 10 || kind == 11)
            {
                // instance data and the state of a door or button: the world changes on one server, the authority of the script's creature (or the acting server)
                Creature const* me = _actionMe.empty() ? nullptr : _actionMe.back();
                if (me && me->IsPuppet())
                    return false;
                return role != RoleReplay || me != nullptr;
            }
            Unit const* unit = object ? object->ToUnit() : nullptr;
            if (!unit)
                return true;
            if (Player const* p = unit->ToPlayer(); p && p->IsMirror())
                return false;
            if (Creature const* c = unit->ToCreature(); c && c->IsPuppet())
                return false;
            return true;
        }

        // ---- the other effects of a creature's spell on a character of another server
        // The core asks before it knocks back, throws, teleports or drains a mirror; when the spell is a creature's that this server decides, the effect goes to the
        // character's own server and is not applied to the mirror. The spells in progress on this thread are noted by the core's spell hook.
        thread_local static inline std::vector<Spell*> _effectSpells;
        enum EffectKind : uint8_t { EffKnock = 1, EffJump = 2, EffTeleport = 3, EffPower = 4 };

        static bool EffectOnMirrorHook(Unit* target, int kind, float a, float b, float c, float d, float e)
        {
            if (_effectSpells.empty())
                return true;
            Player* p = target->ToPlayer();
            if (!p || !p->IsMirror())
                return true;
            Spell* spell = _effectSpells.back();
            WorldObject* caster = spell->GetCaster();
            Creature* creature = caster ? caster->ToCreature() : nullptr;
            if (!creature || creature->IsPuppet() || !WorldTracked(creature))
                return true;
            ObjectGuid victim = p->GetGUID(), source = creature->GetGUID();
            uint32 spellId = spell->GetSpellInfo()->Id;
            LOG_INFO("module", "[P2P] Effect {} of spell {} of creature {} on {} is sent to its owner's server", kind, spellId, creature->GetEntry(), p->GetName());
            Service::Get().Defer([=]
            {
                Service& svc = Service::Get();
                Player* l = svc.Local();
                Creature* cr = l && l->IsInWorld() ? l->GetMap()->GetCreature(source) : nullptr;
                EntityKey key;
                Id128 peer = Mirrors::Get().PeerOf(victim);
                if (!cr || peer == Id128{} || !WorldDecides(cr) || !WorldKeyOf(cr, key))
                    return;
                Writer w;
                key.Write(w);
                w.U8(static_cast<uint8_t>(kind)).U32(spellId).F32(a).F32(b).F32(c).F32(d).F32(e);
                svc.Say(peer, game::MsgEffect, w.data);
            });
            return false;
        }

        // a place a peer may teleport the local character to: on the ground (where the map knows its height) and in sight of where the character stands (the navigation mesh may be
        // missing, so no path is asked)
        static bool ValidPlace(Player* local, float x, float y, float z)
        {
            Map* map = local->GetMap();
            uint32 phase = local->GetPhaseMask();
            float h = map->GetHeight(phase, x, y, z + 2.0f, true);
            if (h > INVALID_HEIGHT && std::abs(h - z) > 10.0f)
                return false;
            return map->isInLineOfSight(local->GetPositionX(), local->GetPositionY(), local->GetPositionZ() + 2.0f, x, y, z + 2.0f, phase, LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing);
        }

        // the character's server: the sender decides the creature, the creature is near, the character is a valid target of the spell; then the effect is applied to the real character
        void OnEffect(Id128 const& peer, Bytes const& data, Player* local)
        {
            Reader r(data);
            EntityKey key = EntityKey::Read(r);
            uint8_t kind = r.U8();
            uint32 spellId = r.U32();
            float a = r.F32(), b = r.F32(), c = r.F32(), d = r.F32(), e = r.F32();
            SpellInfo const* info = r.ok() ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
            if (!info || !local->IsAlive())
                return;
            Creature* creature = WorldLookup(key, local);
            Id128 decider{};
            if (!creature || !WorldDeciderOf(creature, decider) || !(decider == peer) || creature->GetExactDist(local) > 80.0f)
            {
                LOG_INFO("module", "[P2P] Refused an effect of spell {}: the creature is not found here, not decided by the sender, or too far", spellId);
                return;
            }
            if (info->IsPositive() ? !creature->IsValidAssistTarget(local) : !creature->IsValidAttackTarget(local, info))
            {
                LOG_INFO("module", "[P2P] Refused an effect of spell {} of creature {}: this character is not a valid target now", spellId, creature->GetEntry());
                return;
            }
            switch (kind)
            {
                case EffKnock:
                    if (c < 0.0f || c > 100.0f || std::abs(d) > 100.0f)
                        return;
                    local->KnockbackFrom(a, b, c, d);
                    break;
                case EffJump:
                    if (d < 0.0f || d > 100.0f || std::abs(e) > 100.0f || local->GetExactDist(a, b, c) > 100.0f)
                        return;
                    local->GetMotionMaster()->MoveJump(a, b, c, d, e);
                    break;
                case EffTeleport:
                    if (local->GetExactDist(a, b, c) > 60.0f || !ValidPlace(local, a, b, c))
                    {
                        LOG_INFO("module", "[P2P] Refused a teleport of spell {} to {} {} {}: too far or no valid place", spellId, a, b, c);
                        return;
                    }
                    local->NearTeleportTo(a, b, c, d);
                    break;
                case EffPower:
                    if (a < 0.0f || a >= float(MAX_POWERS) || std::abs(b) > std::max(5000.0f, float(local->GetMaxPower(Powers(int(a))))))
                        return;
                    local->ModifyPower(Powers(int(a)), int32(b));
                    break;
                default:
                    return;
            }
            LOG_INFO("module", "[P2P] Effect {} of spell {} of creature {} applied to {}", kind, spellId, creature->GetEntry(), local->GetName());
        }

        thread_local static inline uint32 _hitSpawn = 0;      // the creature whose spell hit is being processed (the answer of its AI follows)
        thread_local static inline std::vector<char> _seeded;
        static bool SpellHitHook(Spell* spell, Unit* unit)
        {
            _hitSpawn = unit && unit->IsCreature() ? static_cast<uint32>(unit->ToCreature()->GetSpawnId()) : 0;
            if (CurrentRole() == RoleNone)
                return true;
            if (Player* p = unit->ToPlayer(); p && p->IsMirror())
            {
                // what a creature that this server decides casts on a mirror is its decider's cast: it is forwarded to the mirror's owner, not refused
                Creature const* caster = spell->GetCaster() ? spell->GetCaster()->ToCreature() : nullptr;
                return caster && !caster->IsPuppet();
            }
            if (Creature* c = unit->ToCreature(); c && c->IsPuppet())
                return false;
            return true;
        }

        // The core's spells: 0 made, 1 destroyed, 2/3 the effects on one target begin/end, 4/5 the AI of a creature answers the hit
        static void SpellHook(Spell* spell, int what)
        {
            switch (what)
            {
                case 0:
                    if (int role = CurrentRole())
                    {
                        std::lock_guard<std::mutex> lock(_markMutex);
                        _marks[spell] = role;
                        _markCount.store(int(_marks.size()), std::memory_order_relaxed);
                    }
                    break;
                case 6:
                    _goSpell = spell;
                    break;
                case 7:
                    _goSpell = nullptr;
                    break;
                case 1:
                    if (_launchCount.load(std::memory_order_relaxed))
                    {
                        std::lock_guard<std::mutex> lock(_launchMutex);
                        _launch.erase(spell);
                        _launchCount.store(int(_launch.size()), std::memory_order_relaxed);
                    }
                    if (_markCount.load(std::memory_order_relaxed))
                    {
                        std::lock_guard<std::mutex> lock(_markMutex);
                        _marks.erase(spell);
                        _markCount.store(int(_marks.size()), std::memory_order_relaxed);
                    }
                    break;
                case 2:
                case 4:
                {
                    if (what == 2)
                        _effectSpells.push_back(spell);
                    int role = MarkOf(spell);
                    // a spell of this server's own player: the answer of the creature it hit is the reaction of this server to its player's act
                    if (!role && what == 4 && !CurrentRole())
                    {
                        Player const* caster = spell->GetCaster() ? spell->GetCaster()->ToPlayer() : nullptr;
                        if (caster && !caster->IsMirror())
                            role = RoleActing;
                    }
                    _roles.push_back(role ? role : CurrentRole());   // (no mark: the current role stays, so that the end pops what the beginning pushed)
                    if (what == 4)
                    {
                        // the creature's answer to a hit of this server's own player is seeded as the replays on the other servers seed theirs
                        bool seed = _frames.empty() && role == RoleActing && spell->GetCaster() && spell->GetCaster()->IsPlayer();
                        _seeded.push_back(seed ? 1 : 0);
                        if (seed)
                        {
                            Launch l = LaunchOf(spell);
                            int64_t at = l.counter ? l.time : Service::Get().NetNow();
                            uint64 seed = SeedOf(Service::Get().Me().id, l.counter ? l.counter : spell->GetSpellInfo()->Id, _hitSpawn);
                            LOG_INFO("module", "[P2P] The answer of the creature to spell {} of this player (act {}) is seeded {} at {}", spell->GetSpellInfo()->Id, l.counter, seed, at);
                            SeedPush(at, seed);
                        }
                    }
                    break;
                }
                case 5:
                    if (!_seeded.empty())
                    {
                        if (_seeded.back())
                            SeedPop();
                        _seeded.pop_back();
                    }
                    // fall through
                case 3:
                    if (what == 3 && !_effectSpells.empty())   // (pushed at 2 only)
                        _effectSpells.pop_back();
                    if (!_roles.empty())
                        _roles.pop_back();
                    break;
            }
        }

        struct DelayedHit { ObjectGuid mirror, creature; uint32 spell; int64_t due; uint32_t counter; int64_t time; };
        std::deque<DelayedHit> _hits;

        static void RunHit(Player* mirror, Creature* c, SpellInfo const* info, uint32_t counter, int64_t time)
        {
            // the act number and the time of the launch are the caster's (a spell go of an older server has none: the spell and the arrival stand for them)
            Id128 who = Mirrors::Get().PeerOf(mirror->GetGUID());
            uint64 seed = SeedOf(who, counter ? counter : info->Id, static_cast<uint32>(c->GetSpawnId()));
            LOG_INFO("module", "[P2P] The answer of the creature to spell {} of {} (act {}) is replayed here, seeded {} at {}", info->Id, mirror->GetName(), counter, seed, time);
            ActScope scope(RoleReplay, time, seed);
            c->AI()->SpellHit(mirror, info);
        }

        void RunDelayedHits(Player* local)
        {
            if (_hits.empty())
                return;
            int64_t now = Service::Get().NetNow();
            std::vector<DelayedHit> due;
            for (auto it = _hits.begin(); it != _hits.end();)
            {
                if (it->due <= now)
                {
                    due.push_back(*it);
                    it = _hits.erase(it);
                }
                else
                    ++it;
            }
            std::stable_sort(due.begin(), due.end(), [](DelayedHit const& a, DelayedHit const& b) { return a.time < b.time; });
            for (DelayedHit const& h : due)
            {
                Player* mirror = ObjectAccessor::GetPlayer(*local, h.mirror);
                Creature* c = mirror && mirror->IsInWorld() ? mirror->GetMap()->GetCreature(h.creature) : nullptr;
                SpellInfo const* info = sSpellMgr->GetSpellInfo(h.spell);
                if (c && info && c->IsAlive() && c->AI())
                    RunHit(mirror, c, info, h.counter, h.time);
            }
        }

        // The spell of a mirror reached these creatures: each creature's script answers it as it would have on the caster's server (a line, the quest credit...), with the
        // mirror as the invoker
        void ReplayHits(WorldPacket const& out, Player* mirror, SpellInfo const* info, uint32_t counter, int64_t time)
        {
            WorldPacket p(out);
            p.rpos(0);
            try
            {
                ObjectGuid ignored;
                uint8 count;
                uint32 spell, flags, stamp;
                p >> ignored.ReadAsPacked() >> ignored.ReadAsPacked() >> count >> spell >> flags >> stamp;
                uint8 n;
                p >> n;
                for (uint8 i = 0; i < n && i < 32; ++i)
                {
                    ObjectGuid g;
                    p >> g;
                    if (!g.IsCreatureOrVehicle())
                        continue;
                    Creature* c = mirror->GetMap()->GetCreature(g);
                    if (!c || !c->IsAlive() || !c->AI() || c->GetExactDist(mirror) > 100.0f)
                        continue;
                    // the spell lands after its travel time, as it does on the caster's server (the cast packet is sent when the spell leaves)
                    int64_t delay = info->Speed > 0.0f ? static_cast<int64_t>(c->GetExactDist(mirror) / info->Speed * 1000.0f) : 0;
                    // it runs at the later of the travel time and the reorder delay after the launch, so that it falls in the order of the times with the other acts
                    _hits.push_back({mirror->GetGUID(), g, spell, time + std::max<int64_t>(std::min<int64_t>(delay, 5000), ReorderMs), counter, time});
                }
            }
            catch (ByteBufferException const&)
            {
            }
        }

        static void Install()
        {
            ::PlayerBroadcast = &Spells::Broadcast;
            ::ObjectBroadcast = &Spells::ObjectHook;
            ::ObjectRadiusBroadcast = &Spells::RadiusHook;
            ::CreatureTextOut = &Spells::TextHook;
            ::ReactionAction = &Spells::ActionHook;
            ::ReactionSpellHit = &Spells::SpellHitHook;
            ::ReactionSpell = &Spells::SpellHook;
            ::ReactionEventGate = &Spells::EventGate;
            ::ReactionEffect = &Spells::EffectHook;
            ::EffectOnMirror = &Spells::EffectOnMirrorHook;
        }

        // a Spell object for the mirror, with the target it was cast at
        Spell* Make(Player* mirror, SpellInfo const* info, Id128 const& target, Player* local)
        {
            Spell* spell = new Spell(mirror, info, TRIGGERED_FULL_MASK);
            Unit* unit = nullptr;
            if (target == Service::Get().Me().id)
                unit = local;
            else if (!(target == Id128{}))
                unit = Mirrors::Get().MirrorOf(target);
            if (unit)
                spell->m_targets.SetUnitTarget(unit);
            return spell;
        }

        void EndChannel(Player* mirror, Id128 const& peer, uint32 id)
        {
            for (auto it = _pending.begin(); it != _pending.end(); ++it)
                if (it->peer == peer && it->spellId == id && it->channel)
                {
                    if (it->spell)
                        it->spell->SendChannelUpdate(0);
                    delete it->spell;
                    _pending.erase(it);
                    return;
                }
            // (a channel whose start was not seen: the fields are cleared all the same)
            if (mirror->GetUInt32Value(UNIT_CHANNEL_SPELL) == id)
            {
                mirror->SetGuidValue(UNIT_FIELD_CHANNEL_OBJECT, ObjectGuid::Empty);
                mirror->SetUInt32Value(UNIT_CHANNEL_SPELL, 0);
            }
        }

        // the casts whose time has come are shown ending; a channel that was never told to end ends at its length
        void Flush(bool all)
        {
            int64 now = GameTime::GetGameTimeMS().count();
            for (auto it = _pending.begin(); it != _pending.end();)
            {
                if (all || it->due <= now)
                {
                    if (it->spell)
                    {
                        if (it->channel)
                            it->spell->SendChannelUpdate(0);
                        else if (!all)
                            it->spell->SendSpellGo();
                    }
                    delete it->spell;
                    it = _pending.erase(it);
                }
                else
                    ++it;
            }
        }

    private:
        struct Pending { Spell* spell; Id128 peer; uint32 spellId; int64 due; bool channel; };
        std::deque<Pending> _pending;
        uint32 _channel = 0;
        int64_t _channelEnd = 0;
    };


    // ---------------------------------------------------------------------------------------------------------------------------------------------------------------
    // The acts of a player that set scripts or quest events off: the act goes to the servers near, each of which runs the core's own reaction to it with
    // the mirror as the actor, inside a reaction scope. The acting server runs the same reaction inside its own scope (PacketHandling wraps the core's handler of the packet).
    class Acts : public Feature
    {
    public:
        enum Kind : uint8_t { AreaTriggerAct = 1, HelloAct = 2, SelectAct = 3, UseAct = 4, ClickAct = 5, AcceptAct = 6, RewardAct = 7, EmoteAct = 8 };
        enum Target : uint8_t { TargetNone = 0, TargetCreature = 1, TargetObject = 2 };
        static constexpr int64_t ReorderMs = Spells::ReorderMs;

        static Acts& Get()
        {
            static Acts acts;
            return acts;
        }

        static bool ActOpcode(uint16_t op)
        {
            switch (op)
            {
                case CMSG_AREATRIGGER: case CMSG_GOSSIP_HELLO: case CMSG_QUESTGIVER_HELLO: case CMSG_GOSSIP_SELECT_OPTION: case CMSG_GAMEOBJ_USE: case CMSG_SPELLCLICK:
                case CMSG_QUESTGIVER_ACCEPT_QUEST: case CMSG_QUESTGIVER_CHOOSE_REWARD: case CMSG_TEXT_EMOTE:
                    return true;
                default:
                    return false;
            }
        }

        // the core handles a packet of the local player: the act travels, and the reaction of this server runs in the Acting scope
        static void PacketHook(WorldSession* session, WorldPacket const& packet, bool begin)
        {
            Service& service = Service::Get();
            thread_local static std::vector<char> pushed;
            if (!begin)
            {
                if (!pushed.empty())
                {
                    if (pushed.back())
                    {
                        Spells::SeedPop();
                        Spells::_roles.pop_back();
                    }
                    pushed.pop_back();
                }
                return;
            }
            Player* local = service.Enabled() ? service.Local() : nullptr;
            if (!local || session != local->GetSession() || !ActOpcode(packet.GetOpcode()))
            {
                pushed.push_back(0);
                return;
            }
            uint32_t counter = ++Spells::_actCounter;
            int64_t now = service.NetNow();
            Bytes body(packet.contents(), packet.contents() + packet.size());
            uint16_t op = packet.GetOpcode();
            // a click: whether the clicked creature has a spell for this player is known now, before the core's handler casts it (the replay tells the AI only the result)
            uint8_t click = op == CMSG_SPELLCLICK ? ClickResult(local, packet) : 0;
            service.Defer([op, body, counter, now, click]
            {
                Get().Send(op, body, counter, now, click);
            });
            Spells::_roles.push_back(Spells::RoleActing);
            Spells::SeedPush(now, Spells::SeedOf(service.Me().id, counter, 0));
            pushed.push_back(1);
        }

        // 1 when a spell of npc_spellclick_spells fits the clicker, 0 when none does (what Unit::HandleSpellClick returns to the AI)
        static uint8_t ClickResult(Player* clicker, WorldPacket const& packet)
        {
            try
            {
                WorldPacket p(packet);
                p.rpos(0);
                ObjectGuid g;
                p >> g;
                Creature* c = clicker->GetMap()->GetCreature(g);
                if (!c)
                    return 0;
                uint32 entry = c->GetVehicleKit() ? c->GetVehicleKit()->GetCreatureEntry() : c->GetEntry();
                SpellClickInfoMapBounds bounds = sObjectMgr->GetSpellClickInfoMapBounds(entry);
                for (auto it = bounds.first; it != bounds.second; ++it)
                {
                    if (!it->second.IsFitToRequirements(clicker, c))
                        continue;
                    ConditionList conds = sConditionMgr->GetConditionsForSpellClickEvent(entry, it->second.spellId);
                    ConditionSourceInfo info(clicker, c);
                    if (sConditionMgr->IsObjectMeetToConditions(info, conds))
                        return 1;
                }
            }
            catch (ByteBufferException const&)
            {
            }
            return 0;
        }

        static bool WriteTarget(Writer& w, Player* local, ObjectGuid guid)
        {
            if (guid.IsCreatureOrVehicle())
            {
                Creature* c = local->GetMap()->GetCreature(guid);
                EntityKey key;
                if (!c || !WorldKeyOf(c, key))
                    return false;
                w.U8(TargetCreature);
                key.Write(w);
                return true;
            }
            if (guid.IsGameObject())
            {
                GameObject* go = local->GetMap()->GetGameObject(guid);
                if (!go || !go->GetSpawnId())
                    return false;
                EntityKey key;
                key.kind = 2;
                key.map = go->GetMapId();
                key.instance = Service::Get().Presence().instanceKey;
                key.spawn = static_cast<uint32_t>(go->GetSpawnId());
                w.U8(TargetObject);
                key.Write(w);
                return true;
            }
            return false;
        }

        static WorldObject* ReadTarget(Reader& r, Player* local)
        {
            uint8_t kind = r.U8();
            if (!r.ok() || kind == TargetNone)
                return nullptr;
            EntityKey key = EntityKey::Read(r);
            if (!r.ok())
                return nullptr;
            if (kind == TargetCreature)
                return WorldLookup(key, local);
            if (kind == TargetObject && key.map == local->GetMapId())
            {
                auto range = local->GetMap()->GetGameObjectBySpawnIdStore().equal_range(key.spawn);
                if (range.first != range.second)
                    return range.first->second;
            }
            return nullptr;
        }

        // world thread
        void Send(uint16_t op, Bytes const& body, uint32_t counter, int64_t time, uint8_t click)
        {
            Service& svc = Service::Get();
            Player* l = svc.Local();
            if (!l || !l->IsInWorld())
                return;
            WorldPacket p(op);
            p.append(body.data(), body.size());
            p.rpos(0);
            Writer w;
            w.U32(counter).I64(time);
            try
            {
                switch (op)
                {
                    case CMSG_AREATRIGGER:
                    {
                        uint32 id;
                        p >> id;
                        w.U8(AreaTriggerAct).U32(id);
                        break;
                    }
                    case CMSG_GOSSIP_HELLO:
                    case CMSG_QUESTGIVER_HELLO:
                    {
                        ObjectGuid g;
                        p >> g;
                        w.U8(HelloAct);
                        if (!WriteTarget(w, l, g))
                            return;
                        break;
                    }
                    case CMSG_GOSSIP_SELECT_OPTION:
                    {
                        ObjectGuid g;
                        uint32 menu, list;
                        p >> g >> menu >> list;
                        std::string code;
                        if (p.rpos() < p.size())
                            p >> code;
                        w.U8(SelectAct);
                        if (!WriteTarget(w, l, g))
                            return;
                        w.U32(menu).U32(list).Str(code);
                        w.U32(l->PlayerTalkClass->GetGossipOptionSender(list)).U32(l->PlayerTalkClass->GetGossipOptionAction(list));
                        break;
                    }
                    case CMSG_GAMEOBJ_USE:
                    {
                        ObjectGuid g;
                        p >> g;
                        GameObject* go = l->GetMap()->GetGameObject(g);
                        if (!go || go->GetGoType() == GAMEOBJECT_TYPE_CHEST)
                            return;   // (a chest is the loot's: Loot.cpp)
                        w.U8(UseAct);
                        if (!WriteTarget(w, l, g))
                            return;
                        break;
                    }
                    case CMSG_SPELLCLICK:
                    {
                        ObjectGuid g;
                        p >> g;
                        w.U8(ClickAct);
                        if (!WriteTarget(w, l, g))
                            return;
                        w.U8(click);
                        break;
                    }
                    case CMSG_QUESTGIVER_ACCEPT_QUEST:
                    {
                        ObjectGuid g;
                        uint32 quest;
                        p >> g >> quest;
                        w.U8(AcceptAct);
                        if (!WriteTarget(w, l, g))
                            return;
                        w.U32(quest);
                        break;
                    }
                    case CMSG_QUESTGIVER_CHOOSE_REWARD:
                    {
                        ObjectGuid g;
                        uint32 quest, reward;
                        p >> g >> quest >> reward;
                        w.U8(RewardAct);
                        if (!WriteTarget(w, l, g))
                            return;
                        w.U32(quest).U32(reward);
                        break;
                    }
                    case CMSG_TEXT_EMOTE:
                    {
                        uint32 text, emote;
                        ObjectGuid g;
                        p >> text >> emote >> g;
                        w.U8(EmoteAct).U32(text).U32(emote);
                        if (g.IsEmpty() || !WriteTarget(w, l, g))
                            return;   // (an emote at nobody sets nothing off)
                        break;
                    }
                    default:
                        return;
                }
            }
            catch (ByteBufferException const&)
            {
                return;
            }
            float x = l->GetPositionX(), y = l->GetPositionY();
            uint32_t map = l->GetMapId(), phase = l->GetPhaseMask();
            uint64_t instance = svc.Presence().instanceKey;
            LOG_INFO("module", "[P2P] Act {} ({}) of this player told to the servers near", counter, KindName(w.data, 12));
            svc.SayAll([=](PeerInfo const& peer)
            {
                float dx = peer.presence.x - x, dy = peer.presence.y - y;
                return peer.presence.map == map && peer.presence.instanceKey == instance && (peer.presence.phaseMask & phase) != 0 && dx * dx + dy * dy < 260.0f * 260.0f;
            }, game::MsgAct, w.data);
        }

        static char const* KindName(Bytes const& body, size_t at)
        {
            static char const* names[] = { "?", "area trigger", "dialog opened", "dialog option", "object used", "spell click", "quest accepted", "quest handed in", "emote at a creature" };
            uint8_t k = body.size() > at ? body[at] : 0;
            return names[k < 9 ? k : 0];
        }

        void OnEvent(InEvent const& e, Player* local) override
        {
            if (e.kind != InEvent::Message || e.type != game::MsgAct || !local || !local->IsInWorld())
                return;
            Reader r(e.data);
            uint32_t counter = r.U32();
            int64_t time = r.I64();
            if (!r.ok())
                return;
            // (the timestamp is the sender's, in network time: a wild one is the sender's fault, and is not trusted further than the cooldowns)
            int64_t now = Service::Get().NetNow();
            if (time > now + 5000 || time < now - 60000)
                time = now;
            _pending.push_back({e.peer, counter, time, r.Rest()});
        }

        void Update(uint32_t, Player* local) override
        {
            if (_pending.empty() || !local || !local->IsInWorld())
                return;
            int64_t now = Service::Get().NetNow();
            std::vector<Pending> due;
            for (auto it = _pending.begin(); it != _pending.end();)
            {
                if (it->time + ReorderMs <= now)
                {
                    due.push_back(std::move(*it));
                    it = _pending.erase(it);
                }
                else
                    ++it;
            }
            std::stable_sort(due.begin(), due.end(), [](Pending const& a, Pending const& b) { return a.time < b.time; });
            for (Pending const& act : due)
                Replay(act, local);
        }

        void OnLogout(Player*) override { _pending.clear(); }

    private:
        struct Pending { Id128 peer; uint32_t counter; int64_t time; Bytes body; };

        void Replay(Pending const& act, Player* local)
        {
            Player* mirror = Mirrors::Get().MirrorOf(act.peer);
            if (!mirror || !mirror->IsInWorld() || !mirror->IsAlive() || mirror->GetMap() != local->GetMap())
                return;
            Reader r(act.body);
            uint8_t kind = r.U8();
            LOG_INFO("module", "[P2P] Act {} ({}) of {} is run here with its mirror as the actor", act.counter, KindName(act.body, 0), Service::Get().NameOf(act.peer));
            Spells::ActScope scope(Spells::RoleReplay, act.time, Spells::SeedOf(act.peer, act.counter, 0));
            WorldSession* session = mirror->GetSession();
            switch (kind)
            {
                case AreaTriggerAct:
                {
                    uint32_t id = r.U32();
                    AreaTrigger const* at = r.ok() ? sObjectMgr->GetAreaTrigger(id) : nullptr;
                    if (!at || mirror->IsInFlight() || mirror->IsGameMaster() || !mirror->IsInAreaTriggerRadius(at))
                        return;
                    // the script of the trigger; its teleport and the exploration of a quest are the actor's server's (the mirror's quest log is read only)
                    sScriptMgr->OnAreaTrigger(mirror, at);
                    break;
                }
                case HelloAct:
                case SelectAct:
                case UseAct:
                case ClickAct:
                {
                    WorldObject* target = ReadTarget(r, local);
                    if (!target)
                        return;
                    // only the script hooks of the object run here; what the core's own handler does besides (a flight, a binding, a cost, a despawn, a ritual) is the acting server's
                    Creature* creature = target->ToCreature();
                    GameObject* go = target->ToGameObject();
                    if (!target->IsInWorld() || (creature && !creature->IsAlive()) || !target->IsWithinDistInMap(mirror, 15.0f))
                        return;
                    if (kind == HelloAct)
                    {
                        if (creature)
                        {
                            sScriptMgr->OnGossipHello(mirror, creature);
                            if (creature->AI())
                                creature->AI()->sGossipHello(mirror);
                        }
                        else if (go)
                            sScriptMgr->OnGossipHello(mirror, go);
                    }
                    else if (kind == SelectAct)
                    {
                        uint32_t menu = r.U32(), list = r.U32();
                        std::string code = r.Str();
                        uint32_t sender = r.U32(), action = r.U32();
                        if (!r.ok())
                            return;
                        if (creature && creature->AI())
                        {
                            if (code.empty())
                            {
                                creature->AI()->sGossipSelect(mirror, menu, list);
                                sScriptMgr->OnGossipSelect(mirror, creature, sender, action);
                            }
                            else
                            {
                                creature->AI()->sGossipSelectCode(mirror, menu, list, code.c_str());
                                sScriptMgr->OnGossipSelectCode(mirror, creature, sender, action, code.c_str());
                            }
                        }
                        else if (go && go->AI())
                        {
                            if (code.empty())
                            {
                                go->AI()->GossipSelect(mirror, menu, list);
                                sScriptMgr->OnGossipSelect(mirror, go, sender, action);
                            }
                            else
                            {
                                go->AI()->GossipSelectCode(mirror, menu, list, code.c_str());
                                sScriptMgr->OnGossipSelectCode(mirror, go, sender, action, code.c_str());
                            }
                        }
                    }
                    else if (kind == UseAct)
                    {
                        if (!go || go->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE))
                            return;
                        // the object's scripts, then the goober's event script and spell (its page, its despawn, its quest credit are the acting server's)
                        if (sScriptMgr->OnGossipHello(mirror, go) || (go->AI() && go->AI()->GossipHello(mirror, false)))
                            return;
                        if (go->GetGoType() == GAMEOBJECT_TYPE_GOOBER)
                        {
                            GameObjectTemplate const* info = go->GetGOInfo();
                            // (the goober's spell is the user's own cast, relayed as a spell go; the zone script's event is the acting server's)
                            if (info->goober.eventId)
                            {
                                go->GetMap()->ScriptsStart(sEventScripts, info->goober.eventId, mirror, go);
                                if (go->AI())
                                    go->AI()->EventInform(info->goober.eventId);
                            }
                        }
                    }
                    else
                    {
                        bool cast = r.U8() != 0;
                        if (!creature || creature->GetVehicleKit() || !r.ok())
                            return;   // (a seat in a vehicle is the rider's server's)
                        // the spells of the click are the acting player's own cast (relayed as a spell go): the creature's script only learns of the click
                        if (creature->IsAIEnabled && creature->AI())
                            creature->AI()->OnSpellClick(mirror, cast);
                        // (a click spell that the clicked creature casts itself is cast here when this server decides the creature, since the acting server, where
                        // it is a puppet, cast nothing; the target is the clicker's mirror or the creature, and the effects on the mirror reach its owner as for any cast of a creature)
                        if (cast && WorldDecides(creature))
                        {
                            uint32 entry = creature->GetEntry();
                            SpellClickInfoMapBounds bounds = sObjectMgr->GetSpellClickInfoMapBounds(entry);
                            for (auto it = bounds.first; it != bounds.second; ++it)
                            {
                                if ((it->second.castFlags & NPC_CLICK_CAST_CASTER_CLICKER) || !it->second.IsFitToRequirements(mirror, creature))
                                    continue;
                                ConditionList conds = sConditionMgr->GetConditionsForSpellClickEvent(entry, it->second.spellId);
                                ConditionSourceInfo info(mirror, creature);
                                SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(it->second.spellId);
                                if (!spellInfo || spellInfo->HasAura(SPELL_AURA_CONTROL_VEHICLE) || !sConditionMgr->IsObjectMeetToConditions(info, conds))
                                    continue;
                                Unit* target = (it->second.castFlags & NPC_CLICK_CAST_TARGET_CLICKER) ? static_cast<Unit*>(mirror) : creature;
                                ObjectGuid orig = (it->second.castFlags & NPC_CLICK_CAST_ORIG_CASTER_OWNER) ? creature->GetOwnerGUID() : mirror->GetGUID();
                                LOG_INFO("module", "[P2P] Spell click: creature {} casts spell {} (click of {})", creature->GetEntry(), it->second.spellId, mirror->GetName());
                                creature->CastSpell(target, spellInfo, TRIGGERED_NONE, nullptr, nullptr, orig);
                            }
                        }
                    }
                    break;
                }
                case AcceptAct:
                case RewardAct:
                {
                    WorldObject* giver = ReadTarget(r, local);
                    uint32_t questId = r.U32();
                    uint32_t reward = kind == RewardAct ? r.U32() : 0;
                    Quest const* quest = giver && r.ok() ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
                    if (!quest || !giver->hasQuest(questId) && !giver->hasInvolvedQuest(questId) || !mirror->CanInteractWithQuestGiver(giver))
                        return;
                    // the reaction of the giver; the mirror's quest log is its owner's, the core's own changes to it are refused
                    if (Creature* c = giver->ToCreature())
                    {
                        if (kind == AcceptAct)
                        {
                            sScriptMgr->OnQuestAccept(mirror, c, quest);
                            if (c->AI())
                                c->AI()->sQuestAccept(mirror, quest);
                        }
                        else if (!sScriptMgr->OnQuestReward(mirror, c, quest, reward) && c->AI())
                            c->AI()->sQuestReward(mirror, quest, reward);
                    }
                    else if (GameObject* go = giver->ToGameObject())
                    {
                        if (kind == AcceptAct)
                        {
                            sScriptMgr->OnQuestAccept(mirror, go, quest);
                            if (go->AI())
                                go->AI()->QuestAccept(mirror, quest);
                        }
                        else if (!sScriptMgr->OnQuestReward(mirror, go, quest, reward) && go->AI())
                            go->AI()->QuestReward(mirror, quest, reward);
                    }
                    break;
                }
                case EmoteAct:
                {
                    uint32_t text = r.U32();
                    r.U32();
                    WorldObject* target = ReadTarget(r, local);
                    Creature* c = target ? target->ToCreature() : nullptr;
                    if (c && c->IsAlive() && c->AI() && r.ok())
                        c->AI()->ReceiveEmote(mirror, text);
                    break;
                }
            }
        }

        std::vector<Pending> _pending;
    };

    void AddSpells()
    {
        Spells::Install();
        ::PacketHandling = &Acts::PacketHook;
        Service::Get().AddFeature(&Spells::Get());
        Service::Get().AddFeature(&Acts::Get());
    }
}
