/*
 * Duels between players of two servers. The duel is the core's own on both servers: the challenger and the target are Player objects on each (one real, one
 * a mirror), and each server keeps the duel state of its copy. What carries it across:
 *   - the request: the server of the challenger tells the target's server, which has the challenger's mirror cast the duel spell on the local player, so that
 *     the core makes the arbiter flag and asks the local player as it would for a challenger on the same server;
 *   - the acceptance and the cancellation of the target are told to the challenger's server, which replays them through the mirror's session;
 *   - the hits travel as player hits already do (World.cpp): the victim's server applies them, and so decides when the duel ends - the end is told by the
 *     loser's server (it owns the loser) and shown on the winner's.
 */
#include "Mirrors.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace p2p
{
    namespace
    {
        enum Step : uint8_t { Request = 1, Accept = 2, Cancel = 3, End = 4 };
        constexpr uint32 DuelSpell = 7266;
    }

    class Duels : public Feature
    {
    public:
        static Duels& Get()
        {
            static Duels duels;
            return duels;
        }

        // the local player challenged a mirror: its server is asked to put the request to its player (the hook runs on a map thread)
        void LocalRequest(Player* target, Player* challenger)
        {
            ObjectGuid targetGuid = target->GetGUID();
            Service::Get().Defer([this, targetGuid]
            {
                Id128 peer = Mirrors::Get().PeerOf(targetGuid);
                if (peer == Id128{})
                    return;
                Writer w;
                w.U8(Request);
                Service::Get().Say(peer, game::MsgDuel, w.data);
            });
        }

        void LocalEnd(Player* winner, Player* loser, DuelCompleteType type)
        {
            Service& service = Service::Get();
            Player* local = service.Local();
            if (!local || loser != local)
                return;   // (the loser's server tells: it owns the loser)
            ObjectGuid winnerGuid = winner->GetGUID();
            service.Defer([this, winnerGuid, type]
            {
                Id128 peer = Mirrors::Get().PeerOf(winnerGuid);
                if (peer == Id128{})
                    return;
                Writer w;
                w.U8(End).U8(static_cast<uint8>(type));
                Service::Get().Say(peer, game::MsgDuel, w.data);
            });
        }

        // the local player answers a duel (accepts, or cancels/forfeits): the challenger's server replays it
        void LocalAnswer(WorldSession* session, WorldPacket const& packet)
        {
            Service& service = Service::Get();
            Player* local = service.Local();
            if (!service.Enabled() || !local || session != local->GetSession() || !local->duel)
                return;
            Player* opponent = local->duel->Opponent;
            Id128 peer = opponent && opponent->IsMirror() ? Mirrors::Get().PeerOf(opponent->GetGUID()) : Id128{};
            if (peer == Id128{})
                return;
            Writer w;
            w.U8(packet.GetOpcode() == CMSG_DUEL_ACCEPTED ? Accept : Cancel);
            service.Say(peer, game::MsgDuel, w.data);
        }

        void OnEvent(InEvent const& e, Player* local) override
        {
            if (e.kind != InEvent::Message || e.type != game::MsgDuel || !local || !local->IsInWorld())
                return;
            Player* mirror = Mirrors::Get().MirrorOf(e.peer);
            if (!mirror)
                return;
            Reader r(e.data);
            Step step = static_cast<Step>(r.U8());
            switch (step)
            {
                case Request:
                    // as if the challenger had cast the duel spell at the local player
                    if (!local->duel && !mirror->duel && mirror->GetExactDist2d(local) < 40.0f)
                        mirror->CastSpell(local, DuelSpell, true);
                    break;
                case Accept:
                case Cancel:
                    if (mirror->duel && mirror->duel->Opponent == local)
                    {
                        // the mirror (the challenged player on this side) answers: the arbiter flag is the one the challenger made
                        WorldPacket data(step == Accept ? CMSG_DUEL_ACCEPTED : CMSG_DUEL_CANCELLED, 8);
                        data << local->GetGuidValue(PLAYER_DUEL_ARBITER);
                        data.rpos(0);
                        if (step == Accept)
                            mirror->GetSession()->HandleDuelAcceptedOpcode(data);
                        else
                            mirror->GetSession()->HandleDuelCancelledOpcode(data);
                    }
                    break;
                case End:
                {
                    uint8 type = r.U8();
                    if (r.ok() && mirror->duel && type <= DUEL_FLED)
                        mirror->DuelComplete(DuelCompleteType(type));
                    break;
                }
            }
        }
    };

    class DuelHooks : public PlayerScript
    {
    public:
        DuelHooks() : PlayerScript("P2PDuels", { PLAYERHOOK_ON_DUEL_REQUEST, PLAYERHOOK_ON_DUEL_END }) { }

        void OnPlayerDuelRequest(Player* target, Player* challenger) override
        {
            Service& s = Service::Get();
            if (s.Enabled() && target->IsMirror() && challenger == s.Local())
                Duels::Get().LocalRequest(target, challenger);
        }

        void OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType type) override
        {
            if (Service::Get().Enabled() && winner->IsMirror())
                Duels::Get().LocalEnd(winner, loser, type);
        }
    };

    class DuelPackets : public ServerScript
    {
    public:
        DuelPackets() : ServerScript("P2PDuelPackets", { SERVERHOOK_CAN_PACKET_RECEIVE }) { }
        bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
        {
            if (packet.GetOpcode() == CMSG_DUEL_ACCEPTED || packet.GetOpcode() == CMSG_DUEL_CANCELLED)
                Duels::Get().LocalAnswer(session, packet);
            return true;
        }
    };

    void AddDuels()
    {
        Service::Get().AddFeature(&Duels::Get());
        new DuelHooks();
        new DuelPackets();
    }
}
