/*
 * What the servers say to each other above the node's own messages (core/Node.h: types below MsgFirstApplication are the node's).
 * Everything is encoded with core/Codec.h; a decoder that sees ok() false drops the message.
 */
#pragma once
#include "core/Codec.h"
#include "core/Node.h"

namespace p2p::game
{
    enum Msg : uint16_t
    {
        MsgSnapshot = 16,    // a character's appearance and state, when a link opens and when it changes much
        MsgMove = 17,        // a movement packet of the character (the client's own, forwarded)
        MsgChat = 18,        // say, yell, emote, channel, whisper
        MsgEntity = 19,      // state of a shared creature or object (position/path, health, authority claim)
        MsgEvent = 20,       // an outcome: damage, healing, aura, death, spawn, despawn, respawn
        MsgParty = 21,       // party and raid membership
        MsgFriend = 22,      // a friend list change told to the friend
        MsgTransfer = 23,    // items, gold and mail: offer, accept, receipt
        MsgInstance = 24,    // the state of an instance (the leader to the members)
        MsgSpell = 25,       // the start of a spell cast by the character
        MsgTrade = 26,       // trade window
        MsgQuest = 29,       // sharing a quest with the members of the party on other servers: the offer, the checks' result, the answer
        MsgDuel = 28,        // a duel between the characters of two servers: request, accept, cancel, end
        MsgAura = 27,        // an aura that one server's caster applied to the other's character: the owner applies it to the real character
        MsgWorldChange = 31, // the changes game masters make to the world of a realm, passed between the servers of the realm (WorldShare.cpp)
        MsgPacket = 32,      // a packet the sender's game shows of its character to those around, with its identities made network ones (Spells.cpp): the others show it as it happens
        MsgCreatures = 34,   // the creatures a server decides near the receiver: key, life, state (World.cpp), every 10 seconds
        MsgCreaturePacket = 35, // a packet the server that decides a creature shows of it to those around: sound, emote, cast, visual
        MsgAct = 37,            // an act of a player that sets scripts or quest events off (area trigger, dialog, object, spell click, quest, emote at a creature): replayed with the mirror as actor
        MsgCreatureText = 36,   // a line a creature of the sender says (the text group and id, the range): the receivers say it through their copy
        MsgEffect = 39,         // an effect of a creature's spell on another server's character that is not damage or an aura: knockback, jump or pull, near teleport, power (Spells.cpp)
        MsgSummon = 38,         // the state of a creature the sender summoned (a script's, a pet, a guardian, a totem): key, entry, summoner, life, place, look (World.cpp)
        MsgLoot = 30,        // the loot of a creature killed by a party: a member opens it, takes, rolls (to the loot owner); the packets of the loot come back
    };

    // What a server announces about its character in the rooms and over every link: enough for the other side to decide whether it wants to see
    // this character, without anything private. Position is for the choice of the governor of untouched creatures too.
    struct Presence
    {
        uint32_t map = 0;
        uint64_t instanceKey = 0;   // 0: the open world; else the instance (the party's, or the character's own)
        uint32_t phaseMask = 1;
        uint32_t zone = 0, area = 0;
        float x = 0, y = 0, z = 0;
        Id128 party{};              // zero: none
        uint8_t level = 1;
        uint8_t status = 0;         // bit 0: away (AFK), bit 1: busy (DND): shown in the friends window of the others

        // The format version of the announced presence is its first byte. Version 4 adds the keyed channel, the link proof and away/busy in the presence to version 3 (reliable channel, routes, records): a server
        // links only with servers of its own version, and the status command lists the others as "different version".
        static constexpr uint8_t Version = 4;
        static uint8_t VersionOf(Bytes const& b) { return b.empty() ? 0 : b[0]; }

        Bytes Encode() const
        {
            Writer w;
            w.U8(Version).U32(map).U64(instanceKey).U32(phaseMask).U32(zone).U32(area).F32(x).F32(y).F32(z).Id(party).U8(level).U8(status);
            return w.data;
        }
        static bool Decode(Bytes const& b, Presence& p)
        {
            Reader r(b);
            if (r.U8() != Version)
                return false;
            p.map = r.U32();
            p.instanceKey = r.U64();
            p.phaseMask = r.U32();
            p.zone = r.U32();
            p.area = r.U32();
            p.x = r.F32();
            p.y = r.F32();
            p.z = r.F32();
            p.party = r.Id();
            p.level = r.U8();
            p.status = r.U8();
            return r.ok();
        }
    };
}

namespace p2p::game
{
    constexpr int EquipmentSlots = 19;

    // An aura that shows on a character, as the others see it. 'expiry' is a network time (a record is not rewritten as the time passes);
    // 0: no end.
    struct AuraRecord
    {
        uint32_t spell = 0;
        int64_t expiry = 0;
        int32_t maxDuration = 0;
        uint8_t stacks = 1;
        uint8_t flags = 0;       // bit 0: beneficial
        Id128 caster{};          // the character that cast it, zero when not known to the other side (or the character itself)
    };

    // Keys of the keyed channel (one stream per link, so a key names a kind of state of the sender)
    constexpr uint64_t KeyMovement = 1, KeySnapshot = 2, KeyMemberState = 3;
    constexpr uint64_t KeyEntityBase = 0x4000000000000000ull;   // | a hash of the creature's key
    constexpr uint64_t KeyCreatureList = 0x3FFF000000000001ull;

    // How a character looks and how it is: the complete record of what other servers show of it, with a version number. It is sent on the reliable
    // channel when anything in it changes and again every 5 seconds; the receiver sets its mirror to it, so that a message that was lost or late is
    // repaired by the next one (auras that are missing are added, extra ones go; a mirror that moves while the record says it stands is stopped).
    struct Snapshot
    {
        static constexpr uint8_t Version = 2;
        uint32_t seq = 0;
        std::string name;
        uint8_t race = 0, cls = 0, gender = 0, level = 1;
        uint32_t bytes = 0, bytes2 = 0, bytes3 = 0;
        uint32_t displayId = 0, nativeDisplayId = 0, mountDisplayId = 0;
        uint32_t faction = 0, unitFlags = 0, playerFlags = 0, unitBytes2 = 0;
        float scale = 1.0f;
        uint32_t health = 0, maxHealth = 0, power = 0, maxPower = 0;
        uint8_t powerType = 0, standState = 0, shapeshift = 0;
        uint32_t equipment[EquipmentSlots][3] = {};   // visible item entry, permanent enchantment, temporary enchantment
        std::vector<AuraRecord> auras;                // the auras that show (buffs and debuffs)
        uint32_t title = 0;
        float x = 0, y = 0, z = 0, o = 0;
        uint32_t map = 0;
        uint64_t instanceKey = 0;
        uint32_t phaseMask = 1;
        uint32_t zone = 0, area = 0;   // what the sender's game says about the place, to be compared with what ours says
        float ground = 0;              // the height of the ground under the character
        uint32_t moveFlags = 0;        // the movement state: the direction and turning flags, the swimming and flying flags
        float runSpeed = 0;
        uint8_t flags = 0;             // bit 0: dead, bit 1: in combat
        uint32_t castSpell = 0, channelSpell = 0;   // the spell being cast and the spell being channelled (0: none)
        int64_t castEnd = 0, channelEnd = 0;        // network time
        Id128 channelTarget{};

        Bytes Encode() const
        {
            Writer w;
            w.U8(Version).U32(seq).Str(name).U8(race).U8(cls).U8(gender).U8(level).U32(bytes).U32(bytes2).U32(bytes3);
            w.U32(displayId).U32(nativeDisplayId).U32(mountDisplayId).U32(faction).U32(unitFlags).U32(playerFlags).U32(unitBytes2).F32(scale);
            w.U32(health).U32(maxHealth).U32(power).U32(maxPower).U8(powerType).U8(standState).U8(shapeshift);
            for (auto const& e : equipment)
                w.U32(e[0]).U32(e[1]).U32(e[2]);
            w.U32(static_cast<uint32_t>(auras.size()));
            for (AuraRecord const& a : auras)
                w.U32(a.spell).I64(a.expiry).U32(static_cast<uint32_t>(a.maxDuration)).U8(a.stacks).U8(a.flags).Id(a.caster);
            w.U32(title).F32(x).F32(y).F32(z).F32(o).U32(map).U64(instanceKey).U32(phaseMask).U32(zone).U32(area).F32(ground);
            w.U32(moveFlags).F32(runSpeed).U8(flags).U32(castSpell).I64(castEnd).U32(channelSpell).I64(channelEnd).Id(channelTarget);
            return w.data;
        }
        static bool Decode(Bytes const& b, Snapshot& s)
        {
            Reader r(b);
            if (r.U8() != Version)
                return false;
            s.seq = r.U32();
            s.name = r.Str(32);
            s.race = r.U8(); s.cls = r.U8(); s.gender = r.U8(); s.level = r.U8();
            s.bytes = r.U32(); s.bytes2 = r.U32(); s.bytes3 = r.U32();
            s.displayId = r.U32(); s.nativeDisplayId = r.U32(); s.mountDisplayId = r.U32();
            s.faction = r.U32(); s.unitFlags = r.U32(); s.playerFlags = r.U32(); s.unitBytes2 = r.U32(); s.scale = r.F32();
            s.health = r.U32(); s.maxHealth = r.U32(); s.power = r.U32(); s.maxPower = r.U32();
            s.powerType = r.U8(); s.standState = r.U8(); s.shapeshift = r.U8();
            for (auto& e : s.equipment)
            {
                e[0] = r.U32(); e[1] = r.U32(); e[2] = r.U32();
            }
            uint32_t n = r.U32();
            if (n > 80)
                return false;
            s.auras.clear();
            for (uint32_t i = 0; i < n; ++i)
            {
                AuraRecord a;
                a.spell = r.U32();
                a.expiry = r.I64();
                a.maxDuration = static_cast<int32_t>(r.U32());
                a.stacks = r.U8();
                a.flags = r.U8();
                a.caster = r.Id();
                s.auras.push_back(a);
            }
            s.title = r.U32();
            s.x = r.F32(); s.y = r.F32(); s.z = r.F32(); s.o = r.F32();
            s.map = r.U32();
            s.instanceKey = r.U64();
            s.phaseMask = r.U32();
            s.zone = r.U32(); s.area = r.U32(); s.ground = r.F32();
            s.moveFlags = r.U32(); s.runSpeed = r.F32(); s.flags = r.U8();
            s.castSpell = r.U32(); s.castEnd = r.I64(); s.channelSpell = r.U32(); s.channelEnd = r.I64(); s.channelTarget = r.Id();
            return r.ok() && r.end();
        }
    };
}
