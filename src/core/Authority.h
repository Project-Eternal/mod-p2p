/*
 * The authority table: for every shared entity (a creature or object of the world), whose server decides it. Authority is claimed, never
 * inferred silently: the deciding server puts its claim into the entity's state messages. Claims that meet are settled by one rule that every
 * server applies alike: the earlier claim time wins, then the lower identifier.
 *
 *   Untouched   nobody interacted with it: the server whose player is closest governs it (a new governor needs a margin of distance)
 *   Negotiated  players interact with it: the participants exchange its changes; the server that decided it keeps deciding it (the arbiter)
 *   Owned       one server decides it (a vehicle, a dead creature tapped by a player, a creature spawned for a quest)
 */
#pragma once
#include "Codec.h"
#include <algorithm>
#include <map>
#include <tuple>
#include <vector>

namespace p2p
{
    enum class Situation : uint8_t { Untouched = 0, Negotiated = 1, Owned = 2 };

    struct Claim
    {
        Id128 server{};      // the character identifier of the deciding server's player
        int64_t time = 0;    // network time of the claim, milliseconds
        bool Empty() const { return server == Id128{}; }
    };

    // true when claim a beats claim b
    inline bool Beats(Claim const& a, Claim const& b)
    {
        if (b.Empty()) return !a.Empty();
        if (a.Empty()) return false;
        if (a.time != b.time) return a.time < b.time;
        return a.server < b.server;
    }

    struct EntityKey
    {
        uint8_t kind = 0;        // 1 creature, 2 game object, 3 summon
        uint32_t map = 0;
        uint64_t instance = 0;   // 0: the open world
        uint32_t spawn = 0;      // the spawn number in the world database; for a summon the owner's counter
        Id128 owner{};           // summons: the owner's character
        bool operator<(EntityKey const& o) const
        {
            return std::tie(kind, map, instance, spawn, owner) < std::tie(o.kind, o.map, o.instance, o.spawn, o.owner);
        }
        bool operator==(EntityKey const& o) const { return !(*this < o) && !(o < *this); }
        void Write(Writer& w) const { w.U8(kind).U32(map).U64(instance).U32(spawn).Id(owner); }
        static EntityKey Read(Reader& r)
        {
            EntityKey k;
            k.kind = r.U8(); k.map = r.U32(); k.instance = r.U64(); k.spawn = r.U32(); k.owner = r.Id();
            return k;
        }
    };

    struct AuthorityEntry
    {
        Situation situation = Situation::Untouched;
        Claim claim;                       // who decides it now
        std::vector<Id128> participants;   // Negotiated: in the order of engaging; the first is the arbiter
        uint64_t generation = 0;           // changes at each respawn (made of the respawning server and the time)
        bool dead = false;
        int64_t lastHeard = 0;
    };

    class AuthorityTable
    {
    public:
        explicit AuthorityTable(Id128 const& me) : _me(me) { }

        AuthorityEntry& At(EntityKey const& key) { return _entries[key]; }
        AuthorityEntry* Find(EntityKey const& key)
        {
            auto it = _entries.find(key);
            return it == _entries.end() ? nullptr : &it->second;
        }
        void Forget(EntityKey const& key) { _entries.erase(key); }
        size_t Size() const { return _entries.size(); }
        template <class F> void ForEach(F&& f) { for (auto& [k, e] : _entries) f(k, e); }

        bool Mine(AuthorityEntry const& e) const { return e.claim.server == _me; }

        // The deciding server of an untouched entity is the one whose player is closest. 'candidate' wants it with distance 'its',
        // the current governor is at 'current'; a margin keeps the entity from flapping between two equally close players.
        static bool GovernorShouldChange(float current, float candidate, float margin) { return candidate + margin < current; }

        // A claim arrives with an entity's state. Returns true when it is accepted (it becomes the entry's claim). When our own claim is
        // beaten, 'lost' is set: the caller turns the entity into a puppet.
        bool OnClaim(AuthorityEntry& e, Claim const& incoming, Situation situation, bool& lost)
        {
            lost = false;
            if (e.claim.Empty() || e.claim.server == incoming.server || Beats(incoming, e.claim) || e.situation != situation)
            {
                // a different situation is more advanced when it is more engaged: untouched < negotiated < owned
                if (!e.claim.Empty() && e.situation != situation && situation < e.situation && e.claim.server != incoming.server)
                    return false;
                lost = e.claim.server == _me && incoming.server != _me;
                e.claim = incoming;
                e.situation = situation;
                return true;
            }
            return false;
        }

        // Negotiated: a server engages. The first to engage is the arbiter; later ones join.
        // The one who already decides the entity keeps deciding it: a player who attacks it joins the fight as a participant, it never takes the entity away.
        void Engage(AuthorityEntry& e, Id128 const& who, int64_t now)
        {
            if (std::find(e.participants.begin(), e.participants.end(), who) == e.participants.end())
                e.participants.push_back(who);
            if (e.situation == Situation::Untouched)
            {
                e.situation = Situation::Negotiated;
                if (e.claim.Empty())
                    e.claim = Claim{who, now};
            }
        }

        // A participant leaves (or its link drops): the next one in the order of engaging claims. Returns the new arbiter, or zero when
        // nobody is left (the entity returns to Untouched).
        Id128 Leave(AuthorityEntry& e, Id128 const& who, int64_t now)
        {
            e.participants.erase(std::remove(e.participants.begin(), e.participants.end(), who), e.participants.end());
            if (e.claim.server != who)
                return e.claim.server;
            if (e.participants.empty())
            {
                e.situation = Situation::Untouched;
                e.claim = Claim{};
                return Id128{};
            }
            e.claim = Claim{e.participants.front(), now};
            return e.claim.server;
        }

        // A server's authority over everything it decided ends (its link dropped): the entries it held pass on by the rule above.
        std::vector<EntityKey> ServerGone(Id128 const& who, int64_t now)
        {
            std::vector<EntityKey> changed;
            for (auto& [key, e] : _entries)
            {
                bool held = e.claim.server == who;
                bool in = std::find(e.participants.begin(), e.participants.end(), who) != e.participants.end();
                if (!held && !in)
                    continue;
                if (e.situation == Situation::Owned && held)
                {
                    e.claim = Claim{};   // (an owned entity of a vanished server has no decider; the next to need it claims it)
                    e.situation = Situation::Untouched;
                }
                else
                    Leave(e, who, now);
                changed.push_back(key);
            }
            return changed;
        }

        static uint64_t MakeGeneration(Id128 const& server, int64_t time)
        {
            Writer w;
            w.Id(server).I64(time);
            Key32 h = Hash(w.data);
            uint64_t g;
            memcpy(&g, h.data(), 8);
            // the number of a life grows with the network time of its respawn (the servers compare two lives by it: the newer one wins); the hash keeps
            // two servers that respawn a creature in the same millisecond apart
            return (static_cast<uint64_t>(time) << 12) | (g & 0xFFE) | 1;   // (never zero: zero is "the original")
        }

    private:
        static Key32 Hash(Bytes const& b);   // SHA-256, in Authority.cpp
        Id128 _me;
        std::map<EntityKey, AuthorityEntry> _entries;
    };
}
