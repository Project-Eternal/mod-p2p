/*
 * The characters of other players that this server has met: a row in the characters table each (under the mirror account, which no one logs in
 * with), so that the core's name cache, whispers, friends, mail and groups find them. Names are local: the first character met with a name
 * keeps it, later ones with the same name are numbered (Arthas, Arthas2, ...); the own characters and the friends are counted first at every
 * start. Two players may see a third one under different names; only identifiers travel on the network.
 */
#pragma once
#include "core/Identity.h"
#include <map>
#include <set>
#include <string>

class Player;

namespace p2p
{
    struct KnownCharacter
    {
        uint32_t guid = 0;
        Certificate cert;
        std::string realName, localName;
        uint8_t race = 0, cls = 0, gender = 0, level = 1;
        bool named = true;   // false: kept from an earlier session and not met yet in this one (a placeholder stands in)
    };

    class Characters
    {
    public:
        static Characters& Get();

        uint32_t MirrorAccount() const { return _account; }
        // at the start of the server: reads the rows, deletes the ones nothing refers to, numbers the names again
        void Load(Account const& mine);
        // the server leaves its realm: the rows belong to its database
        void Clear() { _known.clear(); _byGuid.clear(); _taken.clear(); }

        KnownCharacter* Find(Id128 const& id);   // a pure lookup: never names a character, never writes (so it is safe wherever the identity is only compared or logged)
        // the game shows this character to the player (chat, whisper, emote, mail, party record, trade, a friend's presence, its mirror): a character kept from an earlier
        // session gets its name for this session now (world thread)
        KnownCharacter* Meet(Id128 const& id);
        KnownCharacter* FindByGuid(uint32_t guid);
        // the first time a character is met: makes its row (and the cache entry); later calls update what changed
        KnownCharacter& Learn(Certificate const& cert, std::string const& realName, uint8_t race, uint8_t cls, uint8_t gender, uint8_t level);
        void UpdateLevel(KnownCharacter& known, uint8_t level);
        bool IsRemote(uint32_t guid) const { return _byGuid.count(guid) > 0; }
        template <class F> void ForEach(F&& f) { for (auto& [id, k] : _known) f(k); }
        size_t Count() const { return _known.size(); }
        static bool ValidName(std::string const& name);

    private:
        std::string UniqueName(std::string const& wanted, uint32_t exceptGuid);
        void Insert(KnownCharacter& known);
        void Name(KnownCharacter& known);
        static std::string Placeholder(std::string const& real, uint32_t guid);

        uint32_t _account = 2147483000u;
        std::map<Id128, KnownCharacter> _known;
        std::map<uint32_t, Id128> _byGuid;
        std::set<std::string> _taken;   // names in use, lower case
    };
}
