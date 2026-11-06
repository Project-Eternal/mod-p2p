#include "P2PStore.h"
#include "DatabaseEnv.h"
#include "QueryResult.h"
#include "Log.h"

namespace p2p
{
    void Store::EnsureTables()
    {
        CharacterDatabase.DirectExecute("CREATE TABLE IF NOT EXISTS p2p_account (id INTEGER NOT NULL PRIMARY KEY, signing_private TEXT NOT NULL)");
        CharacterDatabase.DirectExecute("CREATE TABLE IF NOT EXISTS p2p_character (guid INTEGER NOT NULL PRIMARY KEY, net_id TEXT NOT NULL, certificate TEXT NOT NULL, "
            "own INTEGER NOT NULL DEFAULT 0, real_name TEXT NOT NULL DEFAULT '')");
        // the records of transfers of goods (mail, payments, trades): see Transfer.cpp
        CharacterDatabase.DirectExecute("CREATE TABLE IF NOT EXISTS p2p_xfer (id TEXT NOT NULL, role INTEGER NOT NULL, kind INTEGER NOT NULL, other TEXT NOT NULL, state INTEGER NOT NULL, "
            "sent_at INTEGER NOT NULL, deadline INTEGER NOT NULL, giver INTEGER NOT NULL, mail_id INTEGER NOT NULL, offer TEXT NOT NULL, mine TEXT NOT NULL, theirs TEXT NOT NULL, "
            "flags INTEGER NOT NULL, PRIMARY KEY (id, role))");
    }

    Account Store::LoadOrCreateAccount()
    {
        if (QueryResult result = CharacterDatabase.Query("SELECT signing_private FROM p2p_account WHERE id = 1"))
        {
            Bytes raw;
            if (FromHex(result->Fetch()[0].Get<std::string>(), raw) && raw.size() == 32)
            {
                Key32 priv{};
                memcpy(priv.data(), raw.data(), 32);
                return Account::FromPrivate(priv);
            }
            LOG_ERROR("module", "[P2P] The stored account key is damaged");
        }
        Account account = Account::Generate();
        CharacterDatabase.DirectExecute("REPLACE INTO p2p_account (id, signing_private) VALUES (1, '{}')", Hex(account.signing.priv));
        LOG_INFO("module", "[P2P] Made the account key {}", Hex(account.signing.pub).substr(0, 16));
        return account;
    }

    Certificate Store::OwnCharacter(Account const& account, uint32_t guid, std::string const& name)
    {
        if (QueryResult result = CharacterDatabase.Query("SELECT certificate FROM p2p_character WHERE guid = {} AND own = 1", guid))
        {
            Bytes raw;
            Certificate cert;
            if (FromHex(result->Fetch()[0].Get<std::string>(), raw) && Certificate::Decode(raw, cert) && cert.Verify() && cert.accountKey == account.signing.pub)
                return cert;
        }
        Certificate cert = Certificate::Make(account, crypto::RandomId());
        std::string escaped = name;
        CharacterDatabase.EscapeString(escaped);
        CharacterDatabase.DirectExecute("REPLACE INTO p2p_character (guid, net_id, certificate, own, real_name) VALUES ({}, '{}', '{}', 1, '{}')",
            guid, Hex(cert.id), Hex(cert.Encode()), escaped);
        return cert;
    }
}
