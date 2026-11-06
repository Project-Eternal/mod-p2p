/*
 * Rooms: the names under which the relay node groups servers, and the signed envelope of what is said in one.
 *
 * Every name starts with the realm ("/realm/<id>"). Public rooms (the world's tiles, an instance, a zone channel) are readable names. A private
 * room (party, a pair of characters, a custom channel) is named by HKDF from a secret, and every envelope in it carries a MAC with a second key
 * derived from the same secret with another label, so knowing a room's name gives no power over it.
 */
#pragma once
#include "Identity.h"

namespace p2p
{
    // Where the names of a server live: the realm, and for a private shard a prefix derived from the shard key, which is also
    // in the context of every private room and of the link proof. A plain realm number converts to the public scope (an empty shard).
    struct Scope
    {
        uint32_t realm = 1;
        bool shard = false;
        Key32 key{};            // the shard key (only when 'shard')
        std::string prefix;     // "/realm/<id>" or "/realm/<id>/shard/<hash>"
        Key32 proofKey{};       // the MAC key of the hellos (only when 'shard')

        Scope(uint32_t r = 1);
        // the shard key from the identifier: PBKDF2 with 'iterations' rounds (the slow part), salted with the realm
        static Key32 DeriveShardKey(std::string const& identifier, uint32_t realm, int iterations = DefaultIterations);
        static Scope Shard(uint32_t realm, Key32 const& shardKey);
        static constexpr int DefaultIterations = 300000;
    };

    struct Room
    {
        std::string name;
        bool priv = false;
        Key32 proofKey{};   // the MAC key of a private room
    };

    std::string RealmPrefix(Scope const& scope);
    std::string TileRoom(Scope const& scope, uint32_t map, int tileX, int tileY);
    std::string InstanceRoom(Scope const& scope, uint32_t map, Id128 const& instance);
    std::string ZoneChannelRoom(Scope const& scope, uint32_t map, uint32_t zone, std::string const& channel);
    std::string InboxTopic(Scope const& scope, std::string const& inboxHex);
    std::string GoneTopic(Scope const& scope, std::string const& inboxHex);

    // kind: "party", "pair", "chan"; the realm prefix is the HKDF context
    Room PrivateRoom(Scope const& scope, std::string const& kind, Bytes const& secret);
    Bytes PartySecret(Id128 const& partyId);
    Bytes ChannelSecret(std::string const& lowerCaseName);
    // the agreement of the two accounts plus the two character identifiers, sorted: the same on both sides
    bool PairSecret(Account const& mine, Certificate const& myCharacter, Certificate const& other, Bytes& secret);

    // A message said in a room (or sent to an inbox as an introduction): signed by the account key, with a MAC in private rooms
    struct Envelope
    {
        std::string room;
        int64_t time = 0;       // network time, milliseconds
        Certificate sender;
        Bytes payload;
        Bytes signature;
        Key32 mac{};

        Bytes Encode() const;
        static bool Decode(Bytes const& wire, Envelope& out);   // does not verify
        // 'room' is the room the message arrived in (or the inbox it was sent to)
        static Bytes Make(Account const& account, Certificate const& character, Room const& room, int64_t time, Bytes const& payload);
        // true when the signature (and the MAC, for a private room whose key is given) are right; the certificate is checked too
        bool Verify(Room const* privateRoom, bool certificateChecked = false) const;
    };
}
