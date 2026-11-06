/*
 * Friends, followers and the ignore list.
 *
 * The lists are the core's own (account wide: Social.AccountWide), their entries are the rows of characters met. A friend is looked for in the
 * pair room of the two characters, so that the friend's coming online is seen from anywhere. Friends are one sided as in the game: when a
 * player adds a character, that character's server hears of it over the link and remembers a follower (a row of the other character that
 * has one of ours as friend), tells its player, and joins the pair room too. An ignored character is removed from this player's world: no
 * link, no mirror, no message.
 */
#include "Mirrors.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "QueryResult.h"
#include "ScriptMgr.h"
#include "SocialMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <cstring>
#include <map>
#include <mutex>
#include <set>

AC_GAME_API extern bool (*SocialRemoteFriendInfo)(ObjectGuid const&, FriendInfo&);

namespace p2p
{
    class Social : public Feature
    {
    public:
        static Social& Get()
        {
            static Social social;
            return social;
        }

        void OnLogin(Player* player) override
        {
            _friends.clear();
            _ignored.clear();
            _followers.clear();
            _online.clear();
            _timer = 0;
            _first = true;
            // who has one of my characters as a friend: the rows of other characters
            if (QueryResult r = CharacterDatabase.Query("SELECT s.guid FROM character_social s JOIN p2p_character p ON p.guid = s.guid WHERE p.own = 0 "
                "AND (s.flags & 1) = 1 AND s.friend IN (SELECT guid FROM characters WHERE account = {})", player->GetSession()->GetAccountId()))
                do
                    _followers.insert(r->Fetch()[0].Get<uint32>());
                while (r->NextRow());
        }

        void OnLogout(Player* player) override { _online.clear(); _status.clear(); _shown.clear(); }

        // whether this player is busy or away is part of the presence: a change reaches the friends' windows of the servers that hear it
        void Presence(Player* local, game::Presence& presence) override
        {
            presence.status = (local->isAFK() ? 1 : 0) | (local->isDND() ? 2 : 0);
        }

        // Each change of "online" and "offline" is shown once: whatever sends the packet of a friend's status (this feature, the core for a mirror that
        // comes and goes), one that repeats the state last shown for that character is not delivered. Called for every packet to the local player's game.
        bool CanSend(WorldPacket const& packet)
        {
            if (packet.GetOpcode() != SMSG_FRIEND_STATUS || packet.size() < 9)
                return true;
            Player* local = Service::Get().Local();
            if (!local)
                return true;
            uint8 what = packet.contents()[0];
            uint64 guid;
            memcpy(&guid, packet.contents() + 1, 8);
            uint32 state = what;
            if (what == FRIEND_ONLINE && packet.size() >= 10)
                state = (what << 8) | packet.contents()[9];   // (online with another status, away or busy, is news)
            std::lock_guard<std::mutex> lock(_shownMutex);
            auto it = _shown.find(guid);
            if ((what == FRIEND_ONLINE || what == FRIEND_OFFLINE) && it != _shown.end() && it->second == state)
                return false;
            if (what == FRIEND_ONLINE || what == FRIEND_OFFLINE)
                _shown[guid] = state;
            return true;
        }

        void Rooms(Player* local, std::vector<Room>& rooms) override
        {
            Service& service = Service::Get();
            auto add = [&](uint32_t guid)
            {
                KnownCharacter* k = Characters::Get().FindByGuid(guid);
                Bytes secret;
                if (k && PairSecret(service.MyAccount(), service.Me(), k->cert, secret))
                    rooms.push_back(PrivateRoom(service.RoomScope(), "pair", secret));
            };
            for (uint32_t g : _friends) add(g);
            for (uint32_t g : _followers) add(g);
        }

        void Update(uint32_t diff, Player* local) override
        {
            _timer += diff;
            if (_timer < 2000 && !_first)
                return;
            _timer = 0;
            Service& service = Service::Get();
            PlayerSocial* social = local->GetSocial();
            if (!social)
                return;
            std::set<uint32_t> friends, ignored;
            Characters::Get().ForEach([&](KnownCharacter& k)
            {
                ObjectGuid g = ObjectGuid::Create<HighGuid::Player>(k.guid);
                if (social->HasFriend(g)) friends.insert(k.guid);
                if (social->HasIgnore(g)) ignored.insert(k.guid);
            });
            bool changed = _first;
            for (uint32_t g : friends)
                if (!_friends.count(g))
                {
                    changed = true;
                    NotifyFriend(g, true);
                }
            for (uint32_t g : _friends)
                if (!friends.count(g))
                {
                    changed = true;
                    NotifyFriend(g, false);
                }
            if (ignored != _ignored)
            {
                changed = true;
                _ignored = ignored;
                std::set<Id128> blocked;
                for (uint32_t g : ignored)
                    if (KnownCharacter* k = Characters::Get().FindByGuid(g))
                        blocked.insert(k->cert.id);
                service.SetBlocked(blocked);
                // an ignored follower does not follow
                for (uint32_t g : ignored)
                    if (_followers.erase(g))
                        CharacterDatabase.Execute("DELETE FROM character_social WHERE guid = {} AND flags = 1", g);
            }
            _friends = friends;
            _first = false;
            if (changed)
                service.WantRefresh();
            SendPending();
            UpdateOnline(local);
        }

        // I added (or removed) a character as a friend: it hears it when a link exists
        void NotifyFriend(uint32_t guid, bool added)
        {
            KnownCharacter* k = Characters::Get().FindByGuid(guid);
            if (!k)
                return;
            _pending[k->cert.id] = added;
            Service::Get().Need(k->cert.id, true);
        }

        void SendPending()
        {
            for (auto it = _pending.begin(); it != _pending.end();)
            {
                PeerInfo* p = Service::Get().FindPeer(it->first);
                if (p && p->linked)
                {
                    Writer w;
                    w.U8(it->second ? 1 : 0).U32(0);
                    Service::Get().Say(it->first, game::MsgFriend, w.data);
                    it = _pending.erase(it);
                }
                else
                    ++it;
            }
        }

        void OnEvent(InEvent const& e, Player* local) override
        {
            if (e.kind == InEvent::LinkUp)
                SendPending();
            if (e.kind != InEvent::Message || e.type != game::MsgFriend || !local)
                return;
            Reader r(e.data);
            bool added = r.U8() != 0;
            if (!r.ok())
                return;
            KnownCharacter* k = Characters::Get().Meet(e.peer);
            if (!k || _ignored.count(k->guid))
                return;
            if (added && !_followers.count(k->guid))
            {
                // a remote action that makes the local database grow: the player is told, and the list has a limit
                if (_followers.size() >= 50)
                {
                    uint32_t oldest = *_followers.begin();
                    _followers.erase(_followers.begin());
                    CharacterDatabase.Execute("DELETE FROM character_social WHERE guid = {} AND flags = 1", oldest);
                }
                _followers.insert(k->guid);
                CharacterDatabase.Execute("REPLACE INTO character_social (guid, friend, flags, note) VALUES ({}, {}, 1, '')", k->guid, local->GetGUID().GetCounter());
                Service::Get().Tell(local, k->localName + " has added you as a friend.");
                Service::Get().WantRefresh();
            }
            else if (!added && _followers.erase(k->guid))
            {
                CharacterDatabase.Execute("DELETE FROM character_social WHERE guid = {} AND flags = 1", k->guid);
                Service::Get().WantRefresh();
            }
        }

        // friends who are on another server are online for the friends list, wherever they are
        void UpdateOnline(Player* local)
        {
            std::set<uint32_t> now;
            for (uint32_t g : _friends)
            {
                KnownCharacter* k = Characters::Get().FindByGuid(g);
                PeerInfo* p = k ? Service::Get().FindPeer(k->cert.id) : nullptr;
                if (p && p->inTouch)   // (one meaning of "online": the register of who is in touch)
                    now.insert(g);
            }
            for (uint32_t g : now)
            {
                KnownCharacter* k = Characters::Get().FindByGuid(g);
                PeerInfo* p = k ? Service::Get().FindPeer(k->cert.id) : nullptr;
                uint8 status = p ? p->presence.status : 0;
                if (!_online.count(g) || _status[g] != status)
                    Status(local, g, true);
                _status[g] = status;
            }
            for (uint32_t g : _online)
                if (!now.count(g))
                    Status(local, g, false);
            _online = now;
        }

        static bool Info(ObjectGuid const& guid, FriendInfo& info)
        {
            KnownCharacter* k = Characters::Get().FindByGuid(static_cast<uint32_t>(guid.GetCounter()));
            if (!k)
                return false;
            // a friend of this server's list that is not in touch is offline, explicitly: the list shows every friend, online or not
            PeerInfo* p = Service::Get().FindPeer(k->cert.id);
            if (!p || !p->inTouch)
            {
                info.Status = FRIEND_STATUS_OFFLINE;
                info.Area = 0;
                info.Level = k->level;   // (the client shows the last level it knows)
                info.Class = k->cls;
                return true;
            }
            info.Status = (p->presence.status & 2) ? FRIEND_STATUS_DND : (p->presence.status & 1) ? FRIEND_STATUS_AFK : FRIEND_STATUS_ONLINE;
            info.Area = p->presence.zone;
            info.Level = k->level;
            info.Class = k->cls;
            return true;
        }

        void Status(Player* local, uint32_t guid, bool online)
        {
            KnownCharacter* k = Characters::Get().FindByGuid(guid);
            if (!k)
                return;
            ObjectGuid g = ObjectGuid::Create<HighGuid::Player>(guid);
            WorldPacket data(SMSG_FRIEND_STATUS, 20);
            data << uint8(online ? FRIEND_ONLINE : FRIEND_OFFLINE) << g;
            if (online)
            {
                PeerInfo* p = Service::Get().FindPeer(k->cert.id);
                uint8 status = p && (p->presence.status & 2) ? FRIEND_STATUS_DND : p && (p->presence.status & 1) ? FRIEND_STATUS_AFK : FRIEND_STATUS_ONLINE;
                data << uint8(status) << uint32(p ? p->presence.zone : 0) << uint32(k->level) << uint32(k->cls);
            }
            local->SendDirectMessage(&data);
        }

    private:
        std::set<uint32_t> _friends, _ignored, _followers, _online;
        std::map<uint32_t, uint8> _status;
        std::mutex _shownMutex;
        std::map<uint64, uint32> _shown;   // by guid: the last online/offline state shown
        std::map<Id128, bool> _pending;
        uint32_t _timer = 0;
        bool _first = true;
    };

    namespace
    {
        class SocialPackets : public ServerScript
        {
        public:
            SocialPackets() : ServerScript("P2PSocialPackets", { SERVERHOOK_CAN_PACKET_SEND }) { }
            bool CanPacketSend(WorldSession* session, WorldPacket const& packet) override
            {
                Player* local = Service::Get().Local();
                if (!Service::Get().Enabled() || !local || session != local->GetSession())
                    return true;
                return Social::Get().CanSend(packet);
            }
        };
    }

    void AddSocial()
    {
        new SocialPackets();
        Service::Get().AddFeature(&Social::Get());
        ::SocialRemoteFriendInfo = &Social::Info;
    }
}
