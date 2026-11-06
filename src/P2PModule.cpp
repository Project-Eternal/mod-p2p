/*
 * mod-p2p: servers of different players meet through a relay node and keep their worlds in agreement.
 * The portable part (identity, cryptography, rooms, links, the node) is src/core; this is the part that knows the game.
 * Configuration: conf/mod_p2p.conf.dist.
 */
#include "Chat.h"
#include "Creature.h"
#include "GameObject.h"
#include "ChatCommand.h"
#include "Config.h"
#include "GameTime.h"
#include "Mirrors.h"
#include "ObjectAccessor.h"
#include "P2PService.h"
#include "Player.h"
#include "ScriptMgr.h"

using namespace Acore::ChatCommands;
using namespace p2p;

namespace p2p
{
    void AddMirrors();   // Mirrors.cpp
    void AddChat();      // Chat.cpp
    void AddWorld();     // World.cpp
    std::string DescribeWorld();
    std::string DescribeCreature(Creature* c);   // World.cpp
    std::string TestWorld(Player* local, bool hit, uint32 damage);
    std::string TestHitPlayer(Player* local, uint32 damage);   // World.cpp
    std::string TestCreatureCast(Player* local, uint32 spell);   // World.cpp
    std::string TestDoubleCast(Player* local, uint32 spell);     // World.cpp
    std::string TestAuraBench(Player* local, uint32 count);      // Mirrors.cpp
    std::string TestRadiusSound(Player* local, uint32 sound);    // World.cpp
    std::string TestSummon(Player* local, uint32 entry);       // World.cpp
    std::string TestHitSummon(Player* local, uint32 damage);   // World.cpp
    std::string TestCreatureHitSummon(Player* local, uint32 damage);   // World.cpp
    std::string TestClaim(Player* local, bool earlier);        // World.cpp
    void TestEntry(uint32 entry);
    void TestSpawn(uint32 spawn);   // World.cpp
    std::string TestLootRules(Player* local, uint32 method, uint32 threshold);
    std::string TestMail(Player* local, std::string const& name, uint32 money);
    void AddSocial();    // Social.cpp
    void AddParty();     // Party.cpp
    void AddMail();      // Mail.cpp
    void AddTrade();     // Trade.cpp
    void AddInstances(); // Instances.cpp
    void AddSpells();    // Spells.cpp
    void AddRules();     // Rules.cpp
    void AddTransfers(); // Transfer.cpp
    void AddDuels();     // Duels.cpp
    void AddQuests();    // Quests.cpp
    void AddLoot();      // Loot.cpp
    void AddWorldChanges();   // WorldChanges.cpp
    void AddWorldShare();     // WorldShare.cpp
}

namespace
{
    class P2PWorld : public WorldScript
    {
    public:
        P2PWorld() : WorldScript("P2PWorld", { WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN, WORLDHOOK_ON_BEFORE_REALM_UNBIND, WORLDHOOK_ON_AFTER_REALM_BIND }) { }
        void OnStartup() override { Service::Get().Startup(); }
        void OnBeforeRealmUnbind(uint32 realmId) override { Service::Get().OnRealmUnbind(realmId); }
        void OnAfterRealmBind(uint32 realmId) override { Service::Get().OnRealmBind(realmId); }
        void OnUpdate(uint32 diff) override { Service::Get().Update(diff); }
        void OnShutdown() override { Service::Get().Shutdown(); }
    };

    class P2PPlayer : public PlayerScript
    {
    public:
        P2PPlayer() : PlayerScript("P2PPlayer", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_BEFORE_LOGOUT }) { }
        void OnPlayerLogin(Player* player) override { Service::Get().OnLogin(player); }
        void OnPlayerBeforeLogout(Player* player) override { Service::Get().OnLogout(player); }
    };

    // .save keeps the character at once: the core saves it (when it does), then the whole chain of saving runs (Service::KeepNow), at most every 3 seconds
    class P2PSaveCommand : public AllCommandScript
    {
    public:
        P2PSaveCommand() : AllCommandScript("P2PSaveCommand", { ALLCOMMANDHOOK_ON_TRY_EXECUTE_COMMAND }) { }

        bool OnTryExecuteCommand(ChatHandler& handler, std::string_view cmdStr) override
        {
            std::string word(cmdStr.substr(0, cmdStr.find(' ')));
            std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) { return std::tolower(c); });
            if (word == "help")
                return !AnswerHelp(handler, cmdStr);
            WorldSession* session = handler.GetSession();
            if (word != "save" || !session || !session->GetPlayer() || !Service::Get().Enabled())
                return true;
            ObjectGuid guid = session->GetPlayer()->GetGUID();
            Service::Get().Defer([this, guid]
            {
                // (deferred: after the command ran, on the world thread)
                int64_t now = GameTime::GetGameTimeMS().count();
                Player* player = ObjectAccessor::FindPlayer(guid);
                if (!player)
                    return;
                if (now - _lastKept >= 3000)
                {
                    _lastKept = now;
                    Service::Get().KeepNow(player);
                }
                // the core says nothing to a player who saves with .save: this does, as on a normal server (a game master who has the core's own answer is not told twice)
                if (!player->GetSession()->HasPermission(rbac::RBAC_PERM_COMMANDS_SAVE_WITHOUT_DELAY))
                    ChatHandler(player->GetSession()).SendSysMessage(LANG_PLAYER_SAVED);
            });
            return true;
        }

        // ".help p2p" and ".help p2p COMMAND": the usage of the shared world's commands. The core takes the help of a command from the world database's `command`
        // table, which has no rows for a module's commands (it would suggest a report to the makers of the server software); the module answers itself.
        static bool AnswerHelp(ChatHandler& handler, std::string_view cmdStr)
        {
            std::string text(cmdStr);
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
            std::vector<std::string> words;
            for (size_t at = 0; at < text.size();)
            {
                size_t end = text.find(' ', at);
                if (end == std::string::npos)
                    end = text.size();
                if (end > at)
                    words.push_back(text.substr(at, end - at));
                at = end + 1;
            }
            if (words.size() < 2 || words[1] != "p2p")
                return false;
            static std::vector<std::pair<std::string, std::string>> const usage =
            {
                { "status",  ".p2p status: how many relay nodes are reached and how many players you are in touch with and how. With another player or a creature targeted: the connection to that player, or which server decides the creature." },
                { "players", ".p2p players: the players you are in touch with, each with how it is reached (directly or through a relay node) and where it is." },
                { "details", ".p2p details: everything of the two above, with the identifiers, the rooms, the figures of every route and the creatures this server looks after, for a report." },
                { "relays",  ".p2p relays: the relay nodes of this session, each with its number, whether it is reached and since when, or why it is not." },
                { "relay",   ".p2p relay add ADDRESS | remove NUMBER | try ADDRESS: change the relay nodes for this session only (nothing is saved; the page's settings stay as they were). ADDRESS is ws:// or wss://. add: a node at the end of the list; remove: leave that node (the numbers of the others do not change); try: add it and say at once whether it is reached." },
                { "save",    ".save: keep the character at once (also on this page's storage), and say so." },
            };
            if (words.size() == 2)
            {
                handler.SendSysMessage("The shared world's commands (.help p2p COMMAND for one of them):");
                for (auto const& [name, line] : usage)
                    handler.SendSysMessage("  " + line.substr(0, line.find(':')));
                return true;
            }
            for (auto const& [name, line] : usage)
                if (name == words[2])
                {
                    handler.SendSysMessage(line);
                    return true;
                }
            handler.SendSysMessage("There is no shared world command \"" + words[2] + "\": .help p2p lists them.");
            return true;
        }

    private:
        int64_t _lastKept = -10000;
    };

    class P2PCommands : public CommandScript
    {
    public:
        P2PCommands() : CommandScript("P2PCommands") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable testTable =
            {
                { "near", HandleTestNear, SEC_GAMEMASTER, Console::No },
                { "hit",  HandleTestHit,  SEC_GAMEMASTER, Console::No },
                { "hitplayer", HandleTestHitPlayer, SEC_GAMEMASTER, Console::No },
                { "creaturecast", HandleTestCreatureCast, SEC_GAMEMASTER, Console::No },
                { "doublecast", HandleTestDoubleCast, SEC_GAMEMASTER, Console::No },
                { "aurabench", HandleTestAuraBench, SEC_GAMEMASTER, Console::No },
                { "radiussound", HandleTestRadiusSound, SEC_GAMEMASTER, Console::No },
                { "summon", HandleTestSummon, SEC_GAMEMASTER, Console::No },
                { "hitsummon", HandleTestHitSummon, SEC_GAMEMASTER, Console::No },
                { "creaturehitsummon", HandleTestCreatureHitSummon, SEC_GAMEMASTER, Console::No },
                { "claim", HandleTestClaim, SEC_GAMEMASTER, Console::No },
                { "entry", HandleTestEntry, SEC_GAMEMASTER, Console::No },
                { "spawn", HandleTestSpawn, SEC_GAMEMASTER, Console::No },
                { "lootrules", HandleTestLootRules, SEC_GAMEMASTER, Console::No },
                { "mail", HandleTestMail, SEC_GAMEMASTER, Console::No },
                { "items", HandleTestItems, SEC_GAMEMASTER, Console::No },
                { "object", HandleTestObject, SEC_GAMEMASTER, Console::No },
                { "net", HandleTestNet, SEC_GAMEMASTER, Console::No }
            };
            static ChatCommandTable relayTable =
            {
                { "add",    HandleRelayAdd,    SEC_PLAYER, Console::No },
                { "remove", HandleRelayRemove, SEC_PLAYER, Console::No },
                { "try",    HandleRelayTry,    SEC_PLAYER, Console::No }
            };
            static ChatCommandTable p2pTable =
            {
                { "relays",  HandleRelays,  SEC_PLAYER, Console::No },
                { "relay",   relayTable },
                { "status",  HandleStatus,  SEC_PLAYER, Console::No },
                { "players", HandlePlayers, SEC_PLAYER, Console::No },
                { "details", HandleDetails, SEC_PLAYER, Console::No },
                { "test",    testTable },
                { "",        HandleStatus,  SEC_PLAYER, Console::No }
            };
            static ChatCommandTable commandTable = { { "p2p", p2pTable } };
            return commandTable;
        }

        // test commands (game masters only: the Normal realm has none)
        static bool HandleTestNear(ChatHandler* handler)
        {
            handler->SendSysMessage(p2p::TestWorld(handler->GetSession()->GetPlayer(), false, 0));
            return true;
        }

        static bool HandleTestCreatureCast(ChatHandler* handler, uint32 spell)
        {
            handler->SendSysMessage(p2p::TestCreatureCast(handler->GetSession()->GetPlayer(), spell));
            return true;
        }

        static bool HandleTestDoubleCast(ChatHandler* handler, uint32 spell)
        {
            handler->SendSysMessage(p2p::TestDoubleCast(handler->GetSession()->GetPlayer(), spell));
            return true;
        }

        static bool HandleTestAuraBench(ChatHandler* handler, uint32 count)
        {
            handler->SendSysMessage(p2p::TestAuraBench(handler->GetSession()->GetPlayer(), count));
            return true;
        }

        static bool HandleTestRadiusSound(ChatHandler* handler, uint32 sound)
        {
            handler->SendSysMessage(p2p::TestRadiusSound(handler->GetSession()->GetPlayer(), sound));
            return true;
        }

        static bool HandleTestSummon(ChatHandler* handler, uint32 entry)
        {
            handler->SendSysMessage(p2p::TestSummon(handler->GetSession()->GetPlayer(), entry));
            return true;
        }

        static bool HandleTestCreatureHitSummon(ChatHandler* handler, uint32 damage)
        {
            handler->SendSysMessage(p2p::TestCreatureHitSummon(handler->GetSession()->GetPlayer(), damage));
            return true;
        }

        static bool HandleTestHitSummon(ChatHandler* handler, uint32 damage)
        {
            handler->SendSysMessage(p2p::TestHitSummon(handler->GetSession()->GetPlayer(), damage));
            return true;
        }

        // the creatures of this entry only are looked at by the test commands near and hit (0: any)
        static bool HandleTestEntry(ChatHandler* handler, uint32 entry)
        {
            p2p::TestEntry(entry);
            handler->SendSysMessage("test entry set");
            return true;
        }

        // the creature of this spawn number is the one the test commands look at (the same creature on every server)
        static bool HandleTestSpawn(ChatHandler* handler, uint32 spawn)
        {
            p2p::TestSpawn(spawn);
            handler->SendSysMessage("test spawn set");
            return true;
        }

        // the loot method (0 free for all, 1 round robin, 2 master loot, 3 group loot, 4 need before greed) and the quality from which items are rolled for
        static bool HandleTestLootRules(ChatHandler* handler, uint32 method, uint32 threshold)
        {
            handler->SendSysMessage(p2p::TestLootRules(handler->GetSession()->GetPlayer(), method, threshold));
            return true;
        }

        // the items in the backpack and the bags, with their guids (the protocol bots need them to attach items to a mail): "item ENTRY xCOUNT at BAG SLOT guid HEX"
        static bool HandleTestItems(ChatHandler* handler)
        {
            Player* player = handler->GetSession()->GetPlayer();
            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    handler->PSendSysMessage("item {} x{} at {} {} guid {:x}", item->GetEntry(), item->GetCount(), uint32(INVENTORY_SLOT_BAG_0), uint32(slot), item->GetGUID().GetRawValue());
            for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
                if (Bag* b = player->GetBagByPos(bag))
                    for (uint32 slot = 0; slot < b->GetBagSize(); ++slot)
                        if (Item* item = b->GetItemByPos(slot))
                            handler->PSendSysMessage("item {} x{} at {} {} guid {:x}", item->GetEntry(), item->GetCount(), uint32(bag), slot, item->GetGUID().GetRawValue());
            handler->SendSysMessage("items listed");
            return true;
        }

        // the nearest game object of an entry on the player's map, for the protocol bots (they cannot read the world's objects): "object ENTRY spawn S N yards: ready|looted|..., guid HEX, items N"
        static bool HandleTestObject(ChatHandler* handler, uint32 entry)
        {
            Player* player = handler->GetSession()->GetPlayer();
            GameObject* best = nullptr;
            for (auto const& [spawn, go] : player->GetMap()->GetGameObjectBySpawnIdStore())
                if (go->GetEntry() == entry && (!best || player->GetExactDist2d(go) < player->GetExactDist2d(best)))
                    best = go;
            if (!best)
            {
                handler->PSendSysMessage("no object {} near", entry);
                return true;
            }
            char const* state[] = { "not ready", "ready", "opened", "emptied" };
            handler->PSendSysMessage("object {} spawn {} {:.0f} yards: {}{}, guid {:x}, items {} quest {} gold {}", entry, best->GetSpawnId(), player->GetExactDist2d(best),
                state[std::min<uint32>(best->getLootState(), 3)], best->isSpawned() ? "" : ", despawned", best->GetGUID().GetRawValue(), best->loot.items.size(), best->loot.quest_items.size(), best->loot.gold);
            return true;
        }

        // a network event as the page's host would report it (kind "direct" or "route", text with an optional tab and detail): the chat lines can be checked without a browser
        static bool HandleTestNet(ChatHandler* handler, Acore::ChatCommands::Tail text)
        {
            Service::Get().OnNetwork("direct", nullptr, std::string(text));
            handler->SendSysMessage("network event sent");
            return true;
        }

        static bool HandleTestHit(ChatHandler* handler, uint32 damage)
        {
            handler->SendSysMessage(p2p::TestWorld(handler->GetSession()->GetPlayer(), true, damage));
            return true;
        }

        // a hit on the nearest other player's character as the player's own (the owner's server refuses what the game would not allow)
        static bool HandleTestHitPlayer(ChatHandler* handler, uint32 damage)
        {
            handler->SendSysMessage(p2p::TestHitPlayer(handler->GetSession()->GetPlayer(), damage));
            return true;
        }

        // a forced claim of the nearest creature (a double claim): "earlier" gives it a claim older than the other server's
        static bool HandleTestClaim(ChatHandler* handler, std::string how)
        {
            handler->SendSysMessage(p2p::TestClaim(handler->GetSession()->GetPlayer(), how == "earlier"));
            return true;
        }

        static bool HandleTestMail(ChatHandler* handler, std::string name, uint32 money)
        {
            handler->SendSysMessage(p2p::TestMail(handler->GetSession()->GetPlayer(), name, money));
            return true;
        }

        // at every level, a target adds a section: another player's character (the connection to that player), or a creature (which server decides it)
        static void Target(ChatHandler* handler)
        {
            Unit* target = handler->getSelectedUnit();
            if (!target)
                return;
            if (Player* player = target->ToPlayer())
            {
                Id128 id = Mirrors::Get().PeerOf(player->GetGUID());
                LinkFigures f;
                if (id == Id128{} || !Service::Get().Figures(id, f))
                    return;
                std::string route = !f.linked ? "no connection (known through a room)" : f.direct ? "direct" : "through " + f.routeLabel;
                handler->PSendSysMessage("Target {}: {}", Service::Get().NameOf(id), route);
                if (f.linked)
                {
                    handler->PSendSysMessage("  delay {} ms there and back, {}% of the messages lost, connected for {} s", f.rtt, int(f.loss * 100), f.upForMs / 1000);
                    if (!f.direct)
                        handler->PSendSysMessage("  no direct connection: {}", DirectReasonName(f.directReason));
                }
            }
            else if (Creature* creature = target->ToCreature())
                handler->SendSysMessage(p2p::DescribeCreature(creature));
        }

        static bool HandleRelays(ChatHandler* handler)
        {
            for (std::string const& line : Service::Get().DescribeRelays())
                handler->SendSysMessage(line);
            return true;
        }

        static bool Address(ChatHandler* handler, std::string const& address)
        {
            if (address.compare(0, 5, "ws://") != 0 && address.compare(0, 6, "wss://") != 0)
            {
                handler->SendSysMessage("A relay node's address starts with ws:// or wss:// (.help p2p relay).");
                return false;
            }
            return true;
        }

        static bool HandleRelayAdd(ChatHandler* handler, std::string address)
        {
            if (Address(handler, address))
            {
                Service::Get().RelayAdd(address, false);
                handler->SendSysMessage("The relay node was added for this session (.p2p relays shows the list).");
            }
            return true;
        }

        static bool HandleRelayTry(ChatHandler* handler, std::string address)
        {
            if (Address(handler, address))
            {
                Service::Get().RelayAdd(address, true);
                handler->SendSysMessage("Trying the relay node for this session: it is reported as soon as it is reached or refused.");
            }
            return true;
        }

        static bool HandleRelayRemove(ChatHandler* handler, uint32 number)
        {
            std::vector<NodeFigures> nodes = Service::Get().NodeList();
            if (number < 1 || number > nodes.size() || nodes[number - 1].removed)
            {
                handler->SendSysMessage("There is no such relay node: .p2p relays shows the list.");
                return true;
            }
            Service::Get().RelayRemove(number - 1);
            handler->PSendSysMessage("Relay #{} was removed for this session.", number);
            return true;
        }

        static bool HandleStatus(ChatHandler* handler)
        {
            handler->SendSysMessage(Service::Get().Describe());
            Target(handler);
            return true;
        }

        static bool HandlePlayers(ChatHandler* handler)
        {
            handler->SendSysMessage(Service::Get().Describe());
            for (std::string const& line : Service::Get().DescribePlayers())
                handler->SendSysMessage("  " + line);
            Target(handler);
            return true;
        }

        // for a report: everything above, with the identifiers, the rooms, the figures of every route and the creatures this server looks after
        static bool HandleDetails(ChatHandler* handler)
        {
            for (std::string const& line : Service::Get().DescribeDetails())
                handler->SendSysMessage(line);
            for (std::string const& line : Mirrors::Get().Describe())
                handler->SendSysMessage("  " + line);
            handler->SendSysMessage("  " + p2p::DescribeWorld());
            Target(handler);
            return true;
        }
    };
}

void AddP2PScripts()
{
    new P2PWorld();
    new P2PPlayer();
    new P2PSaveCommand();
    new P2PCommands();
    p2p::AddMirrors();
    p2p::AddChat();
    p2p::AddWorld();
    p2p::AddSocial();
    p2p::AddParty();
    p2p::AddMail();
    p2p::AddTrade();
    p2p::AddInstances();
    p2p::AddSpells();
    p2p::AddRules();
    p2p::AddTransfers();
    p2p::AddDuels();
    p2p::AddQuests();
    p2p::AddLoot();
    p2p::AddWorldChanges();
    p2p::AddWorldShare();
}
