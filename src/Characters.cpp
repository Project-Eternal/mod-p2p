#include "Characters.h"
#include "CharacterCache.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "P2PService.h"
#include "Player.h"
#include "WorldSession.h"
#include "Util.h"
#include <algorithm>
#include <vector>

namespace p2p
{
    namespace
    {
        std::string Lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s;
        }

        std::string Escaped(std::string s)
        {
            CharacterDatabase.EscapeString(s);
            return s;
        }
    }

    Characters& Characters::Get()
    {
        static Characters characters;
        return characters;
    }

    bool Characters::ValidName(std::string const& name)
    {
        if (name.size() < 2 || name.size() > 12)
            return false;
        return std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalpha(c) != 0; });
    }

    static std::string Normalized(std::string name)
    {
        name = Lower(name);
        if (!name.empty())
            name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        return name;
    }

    std::string Characters::UniqueName(std::string const& wanted, uint32_t exceptGuid)
    {
        std::string base = ValidName(wanted) ? Normalized(wanted) : "Unknown";
        std::string name = base;
        for (int n = 2; _taken.count(Lower(name)); ++n)
        {
            std::string number = std::to_string(n);
            name = base.substr(0, 12 - number.size()) + number;
        }
        return name;
    }

    std::string Characters::Placeholder(std::string const& real, uint32_t guid)
    {
        std::string base = ValidName(real) ? Normalized(real) : "Unknown";
        return base + "#" + std::to_string(guid);
    }

    void Characters::Load(Account const& mine)
    {
        _account = sConfigMgr->GetOption<uint32>("P2P.MirrorAccountId", 2147483000u);
        _known.clear();
        _byGuid.clear();
        _taken.clear();

        // the names of the own characters are taken first
        if (QueryResult own = CharacterDatabase.Query("SELECT name FROM characters WHERE account <> {} AND account <> 0", _account))
            do
                _taken.insert(Lower(own->Fetch()[0].Get<std::string>()));
            while (own->NextRow());

        // what the core's own tables refer to
        std::set<uint32_t> referenced, friends;
        auto collect = [&](char const* sql, std::set<uint32_t>& into)
        {
            if (QueryResult r = CharacterDatabase.Query(sql))
                do
                    into.insert(r->Fetch()[0].Get<uint32>());
                while (r->NextRow());
        };
        collect("SELECT friend FROM character_social", friends);
        referenced = friends;
        {
            // followers: rows of the characters met that have one of ours as a friend
            if (QueryResult f = CharacterDatabase.Query("SELECT guid FROM character_social WHERE guid IN (SELECT guid FROM characters WHERE account = {})", _account))
                do
                    referenced.insert(f->Fetch()[0].Get<uint32>());
                while (f->NextRow());
        }
        collect("SELECT receiver FROM mail", referenced);
        collect("SELECT sender FROM mail WHERE messageType = 0", referenced);
        // the characters a transfer of goods refers to (a mail on its way, a payment, a trade, a cash on delivery mail not yet paid)
        collect("SELECT g.guid FROM p2p_character g JOIN p2p_xfer x ON x.other = g.net_id WHERE g.own = 0", referenced);
        collect("SELECT memberGuid FROM group_member", referenced);
        collect("SELECT leaderGuid FROM `groups`", referenced);

        struct Row { uint32_t guid; Bytes cert; std::string real; uint8_t race, cls, gender, level; };
        std::vector<Row> rows;
        if (QueryResult r = CharacterDatabase.Query("SELECT p.guid, p.certificate, p.real_name, c.race, c.class, c.gender, c.level FROM p2p_character p "
            "JOIN characters c ON c.guid = p.guid WHERE p.own = 0 ORDER BY p.guid"))
            do
            {
                Field* f = r->Fetch();
                Row row;
                row.guid = f[0].Get<uint32>();
                FromHex(f[1].Get<std::string>(), row.cert);
                row.real = f[2].Get<std::string>();
                row.race = f[3].Get<uint8>();
                row.cls = f[4].Get<uint8>();
                row.gender = f[5].Get<uint8>();
                row.level = f[6].Get<uint8>();
                rows.push_back(std::move(row));
            }
            while (r->NextRow());

        // rows nothing refers to are forgotten: the characters met are kept only while they matter
        uint32_t dropped = 0;
        std::vector<Row> kept;
        for (Row& row : rows)
        {
            if (!referenced.count(row.guid))
            {
                CharacterDatabase.DirectExecute("DELETE FROM characters WHERE guid = {}", row.guid);
                CharacterDatabase.DirectExecute("DELETE FROM p2p_character WHERE guid = {}", row.guid);
                if (sCharacterCache->GetCharacterCacheByGuid(ObjectGuid::Create<HighGuid::Player>(row.guid)))
                    sCharacterCache->DeleteCharacterCacheEntry(ObjectGuid::Create<HighGuid::Player>(row.guid), "");
                ++dropped;
            }
            else
                kept.push_back(std::move(row));
        }
        // the names are given afresh for this session: the own characters were taken above, then the friends of the account's list in the
        // order of their identifiers (the table keeps no time of adding; the identifiers are given in the order the characters were met, so the order holds from one session to the next); a character kept from an earlier session holds no name until it is met again (a placeholder that no player can type stands in)
        std::vector<uint32_t> addedOrder;
        if (QueryResult r = CharacterDatabase.Query("SELECT friend FROM character_social WHERE (flags & 1) = 1 AND guid IN (SELECT guid FROM characters WHERE account <> {} AND account <> 0) ORDER BY friend", _account))
            do
                addedOrder.push_back(r->Fetch()[0].Get<uint32>());
            while (r->NextRow());
        auto rank = [&](uint32_t guid)
        {
            auto it = std::find(addedOrder.begin(), addedOrder.end(), guid);
            return it == addedOrder.end() ? addedOrder.size() : static_cast<size_t>(it - addedOrder.begin());
        };
        std::stable_sort(kept.begin(), kept.end(), [&](Row const& a, Row const& b) { return rank(a.guid) < rank(b.guid); });
        for (Row& row : kept)
        {
            KnownCharacter known;
            known.guid = row.guid;
            known.realName = row.real;
            known.race = row.race;
            known.cls = row.cls;
            known.gender = row.gender;
            known.level = row.level;
            if (!Certificate::Decode(row.cert, known.cert) || !known.cert.Verify() || known.cert.accountKey == mine.signing.pub)
                continue;
            known.named = rank(row.guid) < addedOrder.size();
            known.localName = known.named ? UniqueName(known.realName, known.guid) : Placeholder(known.realName, known.guid);
            if (known.named)
                _taken.insert(Lower(known.localName));
            CharacterDatabase.DirectExecute("UPDATE characters SET name = '{}' WHERE guid = {}", Escaped(known.localName), known.guid);
            ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(known.guid);
            if (sCharacterCache->GetCharacterCacheByGuid(guid))
                sCharacterCache->UpdateCharacterData(guid, known.localName, known.gender, known.race);
            else
                sCharacterCache->AddCharacterCacheEntry(guid, _account, known.localName, known.gender, known.race, known.cls, known.level);
            _byGuid[known.guid] = known.cert.id;
            _known[known.cert.id] = std::move(known);
        }
        LOG_INFO("module", "[P2P] {} characters met kept, {} forgotten", _known.size(), dropped);
    }

    // A character kept from an earlier session is named where the game shows it to the player: a chat line, a mail, a party record, a trade,
    // a friend's presence, the character's mirror. A name query answer tells the game, whose name cache may hold the placeholder already
    void Characters::Name(KnownCharacter& known)
    {
        if (known.named)
            return;
        known.localName = UniqueName(known.realName, known.guid);
        known.named = true;
        _taken.insert(Lower(known.localName));
        CharacterDatabase.DirectExecute("UPDATE characters SET name = '{}' WHERE guid = {}", Escaped(known.localName), known.guid);
        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(known.guid);
        sCharacterCache->UpdateCharacterData(guid, known.localName, known.gender, known.race);
        if (Player* local = Service::Get().Local())
            local->GetSession()->SendNameQueryOpcode(guid);
    }

    // a lookup and nothing else: it neither names the character nor writes (a link coming up, a log line and the peer list of a tester are no meeting)
    KnownCharacter* Characters::Find(Id128 const& id)
    {
        auto it = _known.find(id);
        return it == _known.end() ? nullptr : &it->second;
    }

    KnownCharacter* Characters::Meet(Id128 const& id)
    {
        KnownCharacter* known = Find(id);
        if (known)
            Name(*known);
        return known;
    }

    KnownCharacter* Characters::FindByGuid(uint32_t guid)
    {
        auto it = _byGuid.find(guid);
        return it == _byGuid.end() ? nullptr : Find(it->second);
    }

    void Characters::Insert(KnownCharacter& known)
    {
        known.guid = static_cast<uint32_t>(sObjectMgr->GetGenerator<HighGuid::Player>().Generate());
        CharacterDatabase.DirectExecute("INSERT INTO characters (guid, account, name, race, class, gender, level, taximask, innTriggerId) VALUES ({}, {}, '{}', {}, {}, {}, {}, '', 0)",
            known.guid, _account, Escaped(known.localName), known.race, known.cls, known.gender, known.level);
        CharacterDatabase.DirectExecute("REPLACE INTO p2p_character (guid, net_id, certificate, own, real_name) VALUES ({}, '{}', '{}', 0, '{}')",
            known.guid, Hex(known.cert.id), Hex(known.cert.Encode()), Escaped(known.realName));
        sCharacterCache->AddCharacterCacheEntry(ObjectGuid::Create<HighGuid::Player>(known.guid), _account, known.localName, known.gender, known.race, known.cls, known.level);
        _byGuid[known.guid] = known.cert.id;
    }

    KnownCharacter& Characters::Learn(Certificate const& cert, std::string const& realName, uint8_t race, uint8_t cls, uint8_t gender, uint8_t level)
    {
        if (KnownCharacter* existing = Find(cert.id))
        {
            Name(*existing);   // met again in this session: it gets its name now, with the number if the name is taken by then
            existing->race = race;
            existing->cls = cls;
            existing->gender = gender;
            if (existing->level != level)
                UpdateLevel(*existing, level);
            return *existing;
        }
        KnownCharacter known;
        known.cert = cert;
        known.realName = ValidName(realName) ? Normalized(realName) : "Unknown";
        known.race = race;
        known.cls = cls;
        known.gender = gender;
        known.level = level;
        known.localName = UniqueName(known.realName, 0);
        known.named = true;
        _taken.insert(Lower(known.localName));
        Insert(known);
        return _known[cert.id] = known;
    }

    void Characters::UpdateLevel(KnownCharacter& known, uint8_t level)
    {
        known.level = level;
        CharacterDatabase.Execute("UPDATE characters SET level = {} WHERE guid = {}", level, known.guid);
        sCharacterCache->UpdateCharacterLevel(ObjectGuid::Create<HighGuid::Player>(known.guid), level);
    }
}
