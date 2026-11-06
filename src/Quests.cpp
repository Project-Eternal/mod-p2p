/*
 * Quests between the servers of a party: sharing. The core's sharing asks each member of the group, through the Player object it has for the member, whether the quest can
 * be taken: a mirror has no quest log. Here the checks are run
 * by the server that holds the member's quest log: the sharer's server tells every member on another server, the member's server runs the core's own checks on its player
 * (already on it, done, cannot take it, log full, busy), shows the offer as the core does, and tells the sharer the result; the member's answer (accept or decline) is told back too.
 * The quest progress of a member is its own server's, so what a member has done or handed in is what the checks see.
 */
#include "Mirrors.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <map>
#include <set>

AC_GAME_API extern thread_local bool g_mirrorQuestWrite;   // core (PlayerQuest.cpp): while set, a mirror's quest log may be written

namespace p2p
{
    namespace
    {
        enum Step : uint8_t { Share = 1, Result = 2, Answer = 3, Log = 4 };
        constexpr uint64_t KeyQuestLog = 4;   // (keyed channel: the newest quest log of the sender; 1 to 3 are in Protocol.h)

        // The quest log of a character as a message: for each quest in the log its status, timer, counts and explored flag, then the quests handed in (the
        // mirror carries its owner's quest log, read only, so that the conditions of scripts that ask about the actor's quests are answered as on the owner's server)
        Bytes EncodeLog(Player* p)
        {
            Writer w;
            w.U8(Log);
            auto const& map = p->getQuestStatusMap();
            uint16_t n = 0;
            for (auto const& [id, d] : map)
                if (d.Status != QUEST_STATUS_NONE)
                    ++n;
            w.U16(n);
            for (auto const& [id, d] : map)
            {
                if (d.Status == QUEST_STATUS_NONE)
                    continue;
                w.U32(id).U8(static_cast<uint8_t>(d.Status)).U32(d.Timer);
                for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                    w.U16(d.ItemCount[i]);
                for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                    w.U16(d.CreatureOrGOCount[i]);
                w.U16(d.PlayerCount).U8(d.Explored ? 1 : 0);
            }
            auto const& done = p->getRewardedQuests();
            uint32_t m = static_cast<uint32_t>(std::min<size_t>(done.size(), 20000));
            w.U32(m);
            uint32_t i = 0;
            for (uint32 id : done)
                if (i++ < m)
                    w.U32(id);
            return w.data;
        }

        // the mirror's log is made to be the message's: the core's own changes are refused (g_mirrorQuestWrite is the module's writing)
        void ApplyLog(Player* mirror, Reader& r)
        {
            struct Open { Open() { g_mirrorQuestWrite = true; } ~Open() { g_mirrorQuestWrite = false; } } open;
            std::map<uint32, QuestStatusData> wanted;
            uint16_t n = r.U16();
            for (uint16_t k = 0; k < n && r.ok(); ++k)
            {
                uint32 id = r.U32();
                QuestStatusData d;
                d.Status = static_cast<QuestStatus>(r.U8());
                d.Timer = r.U32();
                for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                    d.ItemCount[i] = r.U16();
                for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                    d.CreatureOrGOCount[i] = r.U16();
                d.PlayerCount = r.U16();
                d.Explored = r.U8() != 0;
                wanted[id] = d;
            }
            std::set<uint32> done;
            uint32_t m = r.U32();
            for (uint32_t k = 0; k < m && k < 20000 && r.ok(); ++k)
                done.insert(r.U32());
            if (!r.ok())
                return;
            static std::map<uint64_t, uint32_t> told;   // (a line only when the number of quests changes)
            uint32_t count = static_cast<uint32_t>(wanted.size());
            if (auto [it, fresh] = told.emplace(mirror->GetGUID().GetRawValue(), count); fresh || it->second != count)
            {
                it->second = count;
                LOG_INFO("module", "[P2P] The quest log of {} is carried here: {} quests, {} handed in", mirror->GetName(), count, done.size());
            }
            // quests the owner no longer has
            std::vector<uint32> gone;
            for (auto const& [id, d] : mirror->getQuestStatusMap())
                if (d.Status != QUEST_STATUS_NONE && !wanted.count(id))
                    gone.push_back(id);
            for (uint32 id : gone)
                mirror->RemoveActiveQuest(id, false);
            for (auto const& [id, d] : wanted)
            {
                Quest const* quest = sObjectMgr->GetQuestTemplate(id);
                if (!quest)
                    continue;
                if (mirror->FindQuestSlot(id) >= MAX_QUEST_LOG_SIZE)
                    mirror->AddQuest(quest, nullptr);   // (a slot; the status below is the owner's)
                mirror->getQuestStatusMap()[id] = d;
            }
            for (uint32 id : done)
                if (!mirror->IsQuestRewarded(id))
                    mirror->SetRewardedQuest(id);
        }
    }

    class Quests : public Feature
    {
        uint32_t _since = 0, _idle = 0;
        uint64_t _digest = 0;
        bool _resend = true;

    public:
        static Quests& Get()
        {
            static Quests quests;
            return quests;
        }

        // the local player shares a quest: the members on other servers are asked. True: the packet is handled here (the core would ask the mirrors, which have no quest log)
        bool LocalShare(WorldSession* session, WorldPacket const& packet)
        {
            Service& service = Service::Get();
            Player* local = service.Local();
            if (!service.Enabled() || !local || session != local->GetSession() || !local->GetGroup())
                return false;
            WorldPacket p(packet);
            p.rpos(0);
            uint32 questId;
            p >> questId;
            if (!local->CanShareQuest(questId))
                return false;   // (the core says why)
            bool any = false;
            for (auto const& slot : local->GetGroup()->GetMemberSlots())
            {
                if (slot.guid == local->GetGUID())
                    continue;
                KnownCharacter* known = Characters::Get().FindByGuid(static_cast<uint32_t>(slot.guid.GetCounter()));
                PeerInfo* info = known ? service.FindPeer(known->cert.id) : nullptr;
                if (!info || !info->inTouch)
                    continue;
                Writer w;
                w.U8(Share).U32(questId);
                service.Say(known->cert.id, game::MsgQuest, w.data);
                any = true;
            }
            return any;
        }

        // The local player accepts a quest that a member on another server shared. The core's handler refuses it: it asks the sharer, who is a mirror here and has no quest log,
        // whether the quest can be shared ("CanShareQuest"). The accept is done here, with the checks of the core on the player whose quest log this is.
        // True: the packet is handled and must not reach the core.
        bool LocalAccept(WorldSession* session, WorldPacket const& packet)
        {
            Service& service = Service::Get();
            Player* local = service.Local();
            if (!service.Enabled() || !local || session != local->GetSession() || !local->GetDivider())
                return false;
            WorldPacket p(packet);
            p.rpos(0);
            ObjectGuid giver;
            uint32 questId;
            p >> giver >> questId;
            Player* sharer = ObjectAccessor::GetPlayer(*local, local->GetDivider());
            LOG_INFO("module", "[P2P] Accept of quest {} (status {}): divider {}, giver {}, sharer {} mirror {}", questId, int(local->GetQuestStatus(questId)), local->GetDivider().ToString(), giver.ToString(), sharer ? sharer->GetName() : "none", sharer && sharer->IsMirror());
            if (!sharer || !sharer->IsMirror() || (giver != sharer->GetGUID() && giver != local->GetGUID()))
                return false;
            Id128 peer = Mirrors::Get().PeerOf(sharer->GetGUID());
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (peer == Id128{} || !quest || !local->CanInteractWithQuestGiver(sharer))
            {
                LOG_INFO("module", "[P2P] Accept of quest {} refused: peer {} quest {} interact {}", questId, peer == Id128{} ? "none" : "known", quest != nullptr, quest && local->CanInteractWithQuestGiver(sharer));
                local->PlayerTalkClass->SendCloseGossip();
                local->SetDivider();
                return true;
            }
            // (the core's exploit fix: quests that give items to be sold are not shared)
            if (uint32 itemId = quest->GetSrcItemId())
                if (ItemTemplate const* srcItem = sObjectMgr->GetItemTemplate(itemId))
                    if (srcItem->SellPrice > 0)
                        return true;
            if (!local->CanTakeQuest(quest, true))
            {
                LOG_INFO("module", "[P2P] Accept of quest {} refused: cannot take it (status {} level {} race {} class {} prev {} exclusive {} day {} conditions {})", questId, int(local->GetQuestStatus(questId)),
                    local->SatisfyQuestLevel(quest, false), local->SatisfyQuestRace(quest, false), local->SatisfyQuestClass(quest, false), local->SatisfyQuestPreviousQuest(quest, false),
                    local->SatisfyQuestExclusiveGroup(quest, false), local->SatisfyQuestDay(quest, false), local->SatisfyQuestConditions(quest, false));
                local->PlayerTalkClass->SendCloseGossip();
                local->SetDivider();
                return true;
            }
            Writer w;
            w.U8(Answer).U8(static_cast<uint8>(QUEST_PARTY_MSG_ACCEPT_QUEST));
            service.Say(peer, game::MsgQuest, w.data);
            LOG_INFO("module", "[P2P] Quest {} accepted, the sharer's server is told", questId);
            local->SetDivider();
            if (local->CanAddQuest(quest, true))
            {
                local->AddQuestAndCheckCompletion(quest, sharer);
                local->PlayerTalkClass->SendCloseGossip();
                if (quest->GetSrcSpell() > 0)
                    local->CastSpell(local, quest->GetSrcSpell(), true);
                return true;
            }
            local->PlayerTalkClass->SendCloseGossip();
            return true;
        }

        // the local player's answer to an offered quest: told to the sharer's server (the core would send it to the mirror)
        void LocalAnswer(WorldSession* session, WorldPacket const& packet)
        {
            Service& service = Service::Get();
            Player* local = service.Local();
            if (!service.Enabled() || !local || session != local->GetSession() || !local->GetDivider())
                return;
            Player* sharer = ObjectAccessor::GetPlayer(*local, local->GetDivider());
            Id128 peer = sharer && sharer->IsMirror() ? Mirrors::Get().PeerOf(sharer->GetGUID()) : Id128{};
            if (peer == Id128{})
                return;
            WorldPacket p(packet);
            p.rpos(0);
            ObjectGuid ignored;
            uint8 message;
            p >> ignored >> message;
            Writer w;
            w.U8(Answer).U8(message);
            service.Say(peer, game::MsgQuest, w.data);
        }

        // the quest log of the local player changes: the others, who carry it on its mirror, get the new one (looked at about twice a second)
        void Update(uint32_t diff, Player* local) override
        {
            if (!local || !local->IsInWorld() || !Service::Get().Enabled())
                return;
            _since += diff;
            if (_since < 500)
                return;
            _since = 0;
            Bytes log = EncodeLog(local);
            uint64_t digest = 1469598103934665603ull;
            for (uint8_t b : log)
                digest = (digest ^ b) * 1099511628211ull;
            _idle += 500;
            if (digest == _digest && !_resend && _idle < 20000)
                return;   // (unchanged; but every 20 s again, for the servers that met the player since)
            _idle = 0;
            _digest = digest;
            _resend = false;
            Service::Get().SayAllKeyed(nullptr, KeyQuestLog, game::MsgQuest, log);
        }

        void OnLogin(Player*) override { _digest = 0; _resend = true; }

        void OnEvent(InEvent const& e, Player* local) override
        {
            if (e.kind != InEvent::Message || e.type != game::MsgQuest || !local || !local->IsInWorld())
                return;
            Player* other = Mirrors::Get().MirrorOf(e.peer);
            if (!other)
            {
                _resend = true;   // (the mirror comes a moment later: the log is sent again then)
                return;
            }
            Reader r(e.data);
            Step step = static_cast<Step>(r.U8());
            switch (step)
            {
                case Log:
                    ApplyLog(other, r);
                    break;
                case Share:
                {
                    uint32 questId = r.U32();
                    Quest const* quest = r.ok() ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
                    Group* group = local->GetGroup();
                    if (!quest || !group || !group->IsMember(other->GetGUID()) || !local->IsInMap(other))
                        return;
                    // the core's own checks, on the player whose quest log this is
                    int code = QUEST_PARTY_MSG_SHARING_QUEST;
                    if (!local->SatisfyQuestStatus(quest, false))
                        code = QUEST_PARTY_MSG_HAVE_QUEST;
                    else if (local->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
                        code = QUEST_PARTY_MSG_FINISH_QUEST;
                    else if (!local->CanTakeQuest(quest, false))
                        code = QUEST_PARTY_MSG_CANT_TAKE_QUEST;
                    else if (!local->SatisfyQuestLog(false))
                        code = QUEST_PARTY_MSG_LOG_FULL;
                    else if (local->GetDivider())
                        code = QUEST_PARTY_MSG_BUSY;
                    LOG_INFO("module", "[P2P] Quest {} shared by {}: code {} status {} autoaccept {} method {}", questId, other->GetName(), code, int(local->GetQuestStatus(questId)), quest->IsAutoAccept(), quest->GetQuestMethod());
                    Writer w;
                    w.U8(Result).U8(static_cast<uint8>(code));
                    Service::Get().Say(e.peer, game::MsgQuest, w.data);
                    if (code != QUEST_PARTY_MSG_SHARING_QUEST)
                        return;
                    if (quest->IsAutoAccept() && local->CanAddQuest(quest, true) && local->CanTakeQuest(quest, true))
                        local->AddQuestAndCheckCompletion(quest, other);
                    if (quest->IsAutoComplete() || !quest->GetQuestMethod())
                        local->PlayerTalkClass->SendQuestGiverRequestItems(quest, other->GetGUID(), local->CanCompleteRepeatableQuest(quest), true);
                    else
                    {
                        local->SetDivider(other->GetGUID());
                        local->PlayerTalkClass->SendQuestGiverQuestDetails(quest, local->GetGUID(), true);
                    }
                    break;
                }
                case Result:
                case Answer:
                {
                    // the member's server tells how the offer fared (the checks), or what the member answered: the sharer sees it as it would from a player on its server
                    uint8 message = r.U8();
                    if (!r.ok())
                        return;
                    LOG_INFO("module", "[P2P] Quest {} from {}: {}", step == Result ? "check" : "answer", other->GetName(), message);
                    if (step == Result)
                        local->SendPushToPartyResponse(other, QuestShareMessages(message));
                    else
                    {
                        WorldPacket data(MSG_QUEST_PUSH_RESULT, 9);
                        data << other->GetGUID() << uint8(message);
                        local->SendDirectMessage(&data);
                    }
                    break;
                }
            }
        }
    };

    class QuestPackets : public ServerScript
    {
    public:
        QuestPackets() : ServerScript("P2PQuestPackets", { SERVERHOOK_CAN_PACKET_RECEIVE }) { }
        bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
        {
            if (packet.GetOpcode() == CMSG_PUSHQUESTTOPARTY)
                return !Quests::Get().LocalShare(session, packet);
            if (packet.GetOpcode() == CMSG_QUESTGIVER_ACCEPT_QUEST)
                return !Quests::Get().LocalAccept(session, packet);
            if (packet.GetOpcode() == MSG_QUEST_PUSH_RESULT)
                Quests::Get().LocalAnswer(session, packet);
            return true;
        }
    };

    void AddQuests()
    {
        Service::Get().AddFeature(&Quests::Get());
        new QuestPackets();
    }
}
