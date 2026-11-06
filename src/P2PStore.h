/*
 * What mod-p2p keeps in the characters database (the one database that is saved): the account key and the network identity of the
 * characters. Everything is stored as hex text so that it does not depend on how a database driver treats binary values.
 */
#pragma once
#include "core/Identity.h"
#include <optional>
#include <string>

namespace p2p
{
    class Store
    {
    public:
        static void EnsureTables();
        // the account's key pair: made at the first start, kept for good
        static Account LoadOrCreateAccount();
        // the identity of one of this server's own characters: made at its first login
        static Certificate OwnCharacter(Account const& account, uint32_t guidLow, std::string const& name);
    };
}
