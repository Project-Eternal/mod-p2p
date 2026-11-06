#include "Rooms.h"
#include <algorithm>

namespace p2p
{
    Scope::Scope(uint32_t r) : realm(r), prefix("/realm/" + std::to_string(r)) { }

    Key32 Scope::DeriveShardKey(std::string const& identifier, uint32_t realm, int iterations)
    {
        return crypto::Pbkdf2(identifier, ToBytes("p2p shard " + std::to_string(realm)), iterations);
    }

    Scope Scope::Shard(uint32_t realm, Key32 const& shardKey)
    {
        Scope s(realm);
        s.shard = true;
        s.key = shardKey;
        Bytes secret(shardKey.begin(), shardKey.end());
        Bytes salt = ToBytes(s.prefix);
        s.prefix += "/shard/" + Hex(crypto::Hkdf(secret.data(), secret.size(), salt, "p2p shard room", 16));
        s.proofKey = crypto::Hkdf32(secret, salt, "p2p shard link proof");
        return s;
    }

    std::string RealmPrefix(Scope const& scope) { return scope.prefix; }

    std::string TileRoom(Scope const& scope, uint32_t map, int tileX, int tileY)
    {
        return RealmPrefix(scope) + "/world/" + std::to_string(map) + "/" + std::to_string(tileX) + "_" + std::to_string(tileY);
    }

    std::string InstanceRoom(Scope const& scope, uint32_t map, Id128 const& instance)
    {
        return RealmPrefix(scope) + "/world/" + std::to_string(map) + "/i" + Hex(instance);
    }

    std::string ZoneChannelRoom(Scope const& scope, uint32_t map, uint32_t zone, std::string const& channel)
    {
        std::string name = channel;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
        for (char& c : name)
            if (c == '/' || c == '#' || c == '+' || c == ' ')
                c = '_';
        return RealmPrefix(scope) + "/zone/" + std::to_string(map) + "/" + std::to_string(zone) + "/" + name;
    }

    std::string InboxTopic(Scope const& scope, std::string const& inboxHex) { return RealmPrefix(scope) + "/inbox/" + inboxHex; }
    std::string GoneTopic(Scope const& scope, std::string const& inboxHex) { return RealmPrefix(scope) + "/gone/" + inboxHex; }

    Room PrivateRoom(Scope const& scope, std::string const& kind, Bytes const& secret)
    {
        std::string prefix = RealmPrefix(scope);
        Bytes salt = ToBytes(prefix);
        if (scope.shard)
            salt.insert(salt.end(), scope.key.begin(), scope.key.end());   // (the prefix alone is public to the node)
        Bytes name = crypto::Hkdf(secret.data(), secret.size(), salt, "p2p room name " + kind, 16);
        Bytes proof = crypto::Hkdf(secret.data(), secret.size(), salt, "p2p room proof " + kind, 32);
        Room room;
        room.priv = true;
        room.name = prefix + "/" + kind + "/" + Hex(name);
        memcpy(room.proofKey.data(), proof.data(), 32);
        return room;
    }

    Bytes PartySecret(Id128 const& partyId) { return Bytes(partyId.begin(), partyId.end()); }
    Bytes ChannelSecret(std::string const& name) { return ToBytes("channel:" + name); }

    bool PairSecret(Account const& mine, Certificate const& myCharacter, Certificate const& other, Bytes& secret)
    {
        Key32 agreed{};
        if (!crypto::Agree(mine.agreement.priv, other.agreeKey, agreed))
            return false;
        Id128 a = myCharacter.id, b = other.id;
        if (b < a)
            std::swap(a, b);
        Writer w;
        w.Key(agreed).Id(a).Id(b);
        secret = w.data;
        return true;
    }

    static Bytes SignedPart(std::string const& room, int64_t time, Id128 const& sender, Bytes const& payload)
    {
        Writer w;
        w.Raw("p2p envelope", 12).Str(room).I64(time).Id(sender).Blob(payload);
        return w.data;
    }

    Bytes Envelope::Encode() const
    {
        Writer w;
        w.U8(1).Str(room).I64(time);
        sender.Write(w);
        w.Blob(payload).Blob(signature).Key(mac);
        return w.data;
    }

    bool Envelope::Decode(Bytes const& wire, Envelope& out)
    {
        Reader r(wire);
        if (r.U8() != 1)
            return false;
        out.room = r.Str(512);
        out.time = r.I64();
        if (!Certificate::Read(r, out.sender))
            return false;
        out.payload = r.Blob(1 << 16);
        out.signature = r.Blob(64);
        out.mac = r.Key();
        return r.ok() && r.end();
    }

    Bytes Envelope::Make(Account const& account, Certificate const& character, Room const& room, int64_t time, Bytes const& payload)
    {
        Envelope e;
        e.room = room.name;
        e.time = time;
        e.sender = character;
        e.payload = payload;
        Bytes signedPart = SignedPart(e.room, time, character.id, payload);
        e.signature = crypto::Sign(account.signing.priv, signedPart);
        if (room.priv)
            e.mac = crypto::Hmac(room.proofKey, signedPart);
        return e.Encode();
    }

    bool Envelope::Verify(Room const* privateRoom, bool certificateChecked) const
    {
        if (!certificateChecked && !sender.Verify())
            return false;
        Bytes signedPart = SignedPart(room, time, sender.id, payload);
        if (!crypto::Verify(sender.accountKey, signedPart, signature))
            return false;
        if (privateRoom)
        {
            Key32 expected = crypto::Hmac(privateRoom->proofKey, signedPart);
            return crypto::ConstantTimeEqual(expected.data(), mac.data(), 32);
        }
        return true;
    }
}
