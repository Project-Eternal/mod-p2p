/*
 * The rules of every realm. They live in this module because they serve the shared world's purpose (a world nobody hosts has no use for the limits
 * of one that is), but they are registered whether the shared world is on or off, and on every realm. None of them changes the core:
 *
 *   - mail never expires, whoever sent it (the one-week limit of a transfer to another server's player is the transfer's own, see Transfers);
 *   - a creature killed by a party gives every member who would get experience the full amount, as if the member had killed it alone;
 *   - items sold to a vendor stay in the list of items to buy back, at the price they were sold for, until bought back or pushed out by newer sales:
 *     they do not disappear with time or at logout (the list is kept in a table of the module and restored at login).
 */
#include "Characters.h"
#include "DatabaseEnv.h"
#include "Formulas.h"
#include "GameTime.h"
#include "Items.h"
#include "Log.h"
#include "Mail.h"
#include "Player.h"
#include "QueryResult.h"
#include "ScriptMgr.h"
#include "World.h"
#include "WorldSession.h"
#include <map>
#include <mutex>

namespace p2p
{
    namespace
    {
        constexpr uint32 NeverExpiresDays = 3650;                   // ten years: the core's own expiry is in days
        constexpr uint32 BuybackOutOfReach = 0x20000000;            // added to the time stamp of a sold item: seventeen years more than the client's 30 hours

        struct Buyback
        {
            struct Slot { uint64 item = 0; uint32 seq = 0; };
            std::map<uint32, Slot> slots;   // buyback slot -> the item there and its row
            uint32 nextSeq = 1;
            uint32 timer = 0, sinceSave = 15000;
            bool restored = false, dirty = false;
        };
        std::mutex g_mutex;
        std::map<uint64, Buyback> g_buyback;   // by player guid (the hooks of a player run on its map's thread)

        bool Real(Player* player) { return player && !player->IsMirror() && player->GetSession() && !player->GetSession()->IsHeadless(); }
    }

    class RulesMail : public MailScript
    {
    public:
        RulesMail() : MailScript("P2PRulesMail", { MAILHOOK_ON_MAIL_EXPIRY_DELAY }) { }

        // The core's rules (cash on delivery 3 days, a game master's mail 90, an auction's and a battle master's) come first and this replaces their result, so no
        // mail has a limit. (The one-week limit of a mail on its way to another server's character is the transfer's own, kept in its record: Transfer.cpp.)
        void OnMailExpiryDelay(MailDraft*, MailSender const&, uint32& expireDelay) override
        {
            expireDelay = NeverExpiresDays * DAY;
        }
    };

    class RulesPlayer : public PlayerScript
    {
    public:
        RulesPlayer() : PlayerScript("P2PRulesPlayer", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_BEFORE_LOGOUT, PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_GIVE_EXP, PLAYERHOOK_ON_STORE_NEW_ITEM,
            PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST, PLAYERHOOK_ON_QUEST_REWARD_ITEM, PLAYERHOOK_ON_LOOT_ITEM, PLAYERHOOK_ON_CREATE_ITEM,
            PLAYERHOOK_ON_REPUTATION_CHANGE, PLAYERHOOK_ON_BEFORE_ACHI_COMPLETE, PLAYERHOOK_ON_BEFORE_CRITERIA_PROGRESS, PLAYERHOOK_ON_BEFORE_UPDATE_SKILL, PLAYERHOOK_ON_CAN_GIVE_LEVEL }) { }

        // A creature killed by a party gives each member who gets experience at all the amount the member would get alone: the core cut it by the size
        // of the party (and in half for members of low level next to a high one).
        void OnPlayerGiveXP(Player* player, uint32& amount, Unit* victim, uint8 xpSource) override
        {
            if (player->IsMirror())
            {
                amount = 0;   // (a mirror shows a character of another server: its experience and level are what that server reports)
                return;
            }
            if (xpSource != PlayerXPSource::XPSOURCE_KILL || !victim || !player->GetGroup() || !amount)
                return;
            uint32 alone = Acore::XP::Gain(player, victim, false);
            alone = uint32(alone * player->GetTotalAuraMultiplier(SPELL_AURA_MOD_XP_PCT));
            if (alone > amount)
                amount = alone;
        }

        // The rest of a mirror's progress is refused too: reputation, achievements and their criteria, skill, levels. Each player's progress is made only on that
        // player's own server, from what that server sees.
        bool OnPlayerReputationChange(Player* player, uint32, int32&, bool) override { return !player->IsMirror(); }
        bool OnPlayerBeforeAchievementComplete(Player* player, AchievementEntry const*) override { return !player->IsMirror(); }
        bool OnPlayerBeforeCriteriaProgress(Player* player, AchievementCriteriaEntry const*) override { return !player->IsMirror(); }
        bool OnPlayerCanGiveLevel(Player* player, uint8) override { return !player->IsMirror(); }
        void OnPlayerBeforeUpdateSkill(Player* player, uint32, uint32& value, uint32 max, uint32) override
        {
            if (player->IsMirror())
                value = max;   // (the core raises a skill only when the roll is below the current value: a value at the maximum leaves it as it is)
        }

        // Important events ask for a save of the character (at most once every 15 seconds): an item received, a quest done, a level gained. With the core's own
        // save every two minutes, a reload of the page loses very little.
        void OnPlayerStoreNewItem(Player* player, Item*, uint32) override { Important(player); }
        void OnPlayerLevelChanged(Player* player, uint8) override { Important(player); }
        void OnPlayerCompleteQuest(Player* player, Quest const*) override { Important(player); }
        void OnPlayerQuestRewardItem(Player* player, Item*, uint32) override { Important(player); }
        void OnPlayerLootItem(Player* player, Item*, uint32, ObjectGuid) override { Important(player); }
        void OnPlayerCreateItem(Player* player, Item*, uint32) override { Important(player); }

        void OnPlayerLogin(Player* player) override
        {
            if (!Real(player))
                return;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_buyback[player->GetGUID().GetRawValue()] = Buyback();
            }
            Restore(player);
            MailsNeverExpire(player);
        }

        void OnPlayerBeforeLogout(Player* player) override
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_buyback.erase(player->GetGUID().GetRawValue());
        }

        void OnPlayerUpdate(Player* player, uint32 diff) override
        {
            if (!Real(player))
                return;
            Buyback* state;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                auto it = g_buyback.find(player->GetGUID().GetRawValue());
                if (it == g_buyback.end())
                    return;
                state = &it->second;
            }
            state->timer += diff;
            state->sinceSave += diff;
            if (state->timer < 500)
                return;
            state->timer = 0;
            if (state->dirty && state->sinceSave >= 15000)
            {
                state->dirty = false;
                state->sinceSave = 0;
                player->SaveToDB(false, false);
                LOG_INFO("module", "[P2P] {} saved after an important event", player->GetName());
            }
            Sync(player, *state);
        }

    private:
        static void Important(Player* player)
        {
            if (!Real(player))
                return;
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_buyback.find(player->GetGUID().GetRawValue());
            if (it != g_buyback.end())
                it->second.dirty = true;
        }

        // The mails stored before this rule existed (or by a server without it) still have their limit; they are lifted once, at the login of their receiver.
        // New mail needs nothing: OnMailExpiryDelay gives it none.
        static void MailsNeverExpire(Player* player)
        {
            time_t now = GameTime::GetGameTime().count();
            time_t limited = now + (NeverExpiresDays - 100) * DAY, never = now + NeverExpiresDays * DAY;
            for (Mail* mail : player->GetMails())
                if (mail->expire_time < limited)
                    mail->expire_time = never;
            CharacterDatabase.Execute("UPDATE mail SET expire_time = {} WHERE receiver = {} AND expire_time < {}", uint32(never), player->GetGUID().GetCounter(), uint32(limited));
        }

        // the items sold since the last look are written down; those bought back or pushed out are struck out
        static void Sync(Player* player, Buyback& state)
        {
            uint32 guid = player->GetGUID().GetCounter();
            for (uint32 slot = BUYBACK_SLOT_START; slot < BUYBACK_SLOT_END; ++slot)
            {
                Item* item = player->GetItemFromBuyBackSlot(slot);
                auto known = state.slots.find(slot);
                uint32 eslot = slot - BUYBACK_SLOT_START;
                if (known != state.slots.end() && (!item || item->GetGUID().GetRawValue() != known->second.item))
                {
                    CharacterDatabase.Execute("DELETE FROM p2p_buyback WHERE guid = {} AND seq = {}", guid, known->second.seq);
                    state.slots.erase(known);
                    known = state.slots.end();
                }
                if (!item)
                    continue;
                if (known == state.slots.end())
                {
                    ItemData d = Describe(item);
                    uint32 seq = state.nextSeq++;
                    std::string charges = d.charges, enchantments = d.enchantments;
                    CharacterDatabase.EscapeString(charges);
                    CharacterDatabase.EscapeString(enchantments);
                    CharacterDatabase.Execute("REPLACE INTO p2p_buyback (guid, seq, entry, count, durability, flags, random, charges, enchantments, price) VALUES ({}, {}, {}, {}, {}, {}, {}, '{}', '{}', {})",
                        guid, seq, d.entry, d.count, d.durability, d.flags, d.randomProperty, charges, enchantments, player->GetUInt32Value(PLAYER_FIELD_BUYBACK_PRICE_1 + eslot));
                    state.slots[slot] = { item->GetGUID().GetRawValue(), seq };
                }
                // the time stamp: out of reach, so that the item is never dropped for its age (the order of the stamps still tells the oldest)
                uint32 stamp = player->GetUInt32Value(PLAYER_FIELD_BUYBACK_TIMESTAMP_1 + eslot);
                if (stamp < BuybackOutOfReach)
                    player->SetUInt32Value(PLAYER_FIELD_BUYBACK_TIMESTAMP_1 + eslot, stamp + BuybackOutOfReach);
            }
        }

        // at login the list of the last session is made again from the table, oldest first
        static void Restore(Player* player)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            Buyback& state = g_buyback[player->GetGUID().GetRawValue()];
            if (state.restored)
                return;
            state.restored = true;
            uint32 guid = player->GetGUID().GetCounter();
            QueryResult r = CharacterDatabase.Query("SELECT seq, entry, count, durability, flags, random, charges, enchantments, price FROM p2p_buyback WHERE guid = {} ORDER BY seq", guid);
            if (!r)
                return;
            uint32 restored = 0;
            do
            {
                Field* f = r->Fetch();
                ItemData d;
                uint32 seq = f[0].Get<uint32>();
                d.entry = f[1].Get<uint32>(); d.count = f[2].Get<uint32>(); d.durability = f[3].Get<uint32>(); d.flags = f[4].Get<uint32>();
                d.randomProperty = f[5].Get<int32>(); d.charges = f[6].Get<std::string>(); d.enchantments = f[7].Get<std::string>();
                uint32 price = f[8].Get<uint32>();
                state.nextSeq = std::max(state.nextSeq, seq + 1);
                if (Item* item = Create(player, d))
                {
                    player->AddItemToBuyBackSlot(item, price);
                    ++restored;
                }
                else
                    CharacterDatabase.Execute("DELETE FROM p2p_buyback WHERE guid = {} AND seq = {}", guid, seq);
            } while (r->NextRow());
            if (restored)
                LOG_INFO("module", "[P2P] {} items to buy back restored for {}", restored, player->GetName());
            // (the rows are matched to the slots by the next look, which finds the items in the slots but not in its list: so the table is made again from them)
            CharacterDatabase.Execute("DELETE FROM p2p_buyback WHERE guid = {}", guid);
        }
    };

    class RulesWorld : public WorldScript
    {
    public:
        RulesWorld() : WorldScript("P2PRulesWorld", { WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_AFTER_REALM_BIND }) { }
        void OnStartup() override { EnsureTable(); }
        void OnAfterRealmBind(uint32) override { EnsureTable(); }   // (the characters database of another realm)

        static void EnsureTable()
        {
            CharacterDatabase.DirectExecute("CREATE TABLE IF NOT EXISTS p2p_buyback (guid INTEGER NOT NULL, seq INTEGER NOT NULL, entry INTEGER NOT NULL, count INTEGER NOT NULL, "
                "durability INTEGER NOT NULL, flags INTEGER NOT NULL, random INTEGER NOT NULL, charges TEXT NOT NULL, enchantments TEXT NOT NULL, price INTEGER NOT NULL, "
                "PRIMARY KEY (guid, seq))");
        }
    };

    void AddRules()
    {
        new RulesWorld();
        new RulesMail();
        new RulesPlayer();
    }
}
