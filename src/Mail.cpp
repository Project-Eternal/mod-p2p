/*
 * Mail between servers is carried by the transfers of Transfer.cpp (offer, receipt, deadline). What stays here is a test aid.
 */
#include "Mirrors.h"
#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Mail.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "World.h"

namespace p2p
{
    // test aid (.p2p test mail NAME MONEY): a mail with money from the local player to a character met, as the mailbox would make it
    std::string TestMail(Player* local, std::string const& name, uint32 money)
    {
        ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(name);
        if (!guid || !Characters::Get().IsRemote(static_cast<uint32_t>(guid.GetCounter())))
            return "no such character met";
        MailDraft draft("test mail", "sent by .p2p test mail");
        draft.AddMoney(money);
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        draft.SendMailTo(trans, MailReceiver(guid.GetCounter()), MailSender(MAIL_NORMAL, local->GetGUID().GetCounter()), MAIL_CHECK_MASK_NONE, 0);
        CharacterDatabase.CommitTransaction(trans);
        return Acore::StringFormat("mail with {} copper stored for {}", money, name);
    }

    void AddMail() { }
}
