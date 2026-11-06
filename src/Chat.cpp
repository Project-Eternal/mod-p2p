/*
 * Chat between servers. Say, yell and emotes reach the players that are within range on the receiving server (through the mirror of the speaker
 * when there is one); channels reach the members of the same channel; a whisper reaches a character this server has met and that is online.
 * Zone channels (General, Trade, ...) have a public room per zone, custom channels a private room named from the channel's name.
 *
 * The members of a channel: each server tells the peers it is linked to the list of channels its player is in, as a state (sent when it changes and again
 * from time to time); the core's list of a channel's members shows them too (a seam of Channel::List), and a player joining or leaving a named channel is
 * announced in it as the game does. An emote aimed at a character names its target by network identity (certificate and real name), so that each server shows
 * the target under the name that player knows it by.
 */
#include "Mirrors.h"
#include "CharacterCache.h"
#include "Channel.h"
#include "Chat.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SocialMgr.h"
#include "WorldPacket.h"
#include "World.h"
#include "WorldSession.h"
#include <algorithm>
#include <deque>

AC_GAME_API extern bool (*WhisperToRemote)(Player*, std::string const&, uint32, std::string const&);
AC_GAME_API extern void (*ChannelRemoteMembers)(Channel const*, std::vector<std::pair<ObjectGuid, uint8>>&);

namespace p2p
{
    namespace
    {
        enum Kind : uint8_t { Say = 1, Yell = 2, TextEmote = 3, Whisper = 4, Channel_ = 5, EmoteAnim = 6, Party = 7, PresetEmote = 8, Channels = 9, Roll = 10 };

        std::string Lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s;
        }

        // "General - Elwynn Forest" -> "general"
        std::string BaseName(std::string const& name)
        {
            size_t at = name.find(" - ");
            return Lower(at == std::string::npos ? name : name.substr(0, at));
        }

        // the key two servers compare: a zone channel is the same in the same zone, a custom one by its name
        std::string ChannelKey(::Channel const* channel, uint32_t zone)
        {
            if (channel->IsConstant())
                return "z" + std::to_string(zone) + ":" + BaseName(channel->GetName());
            return "c:" + Lower(channel->GetName());
        }

        struct PendingWhisper { Id128 to; std::string text; uint32_t lang; int64_t expires; };
        std::deque<PendingWhisper> g_pending;

        float Distance(PeerInfo const& peer, Player* local)
        {
            float dx = peer.presence.x - local->GetPositionX(), dy = peer.presence.y - local->GetPositionY();
            return std::sqrt(dx * dx + dy * dy);
        }

        bool InSight(PeerInfo const& peer, Player* local, float range)
        {
            return peer.presence.map == local->GetMapId() && peer.presence.instanceKey == Service::Get().Presence().instanceKey &&
                (peer.presence.phaseMask & local->GetPhaseMask()) != 0 && Distance(peer, local) <= range;
        }

        void Send(Kind kind, uint32_t lang, std::string const& text, std::string const& channel, Id128 const& to, uint32_t number,
            std::function<bool(PeerInfo const&)> const& filter)
        {
            Writer w;
            w.U8(kind).U32(lang).Str(text).Str(channel).Id(to).U32(number);
            Service::Get().SayAll(filter, game::MsgChat, w.data);
        }
    }

    class Chat : public Feature
    {
    public:
        static Chat& Get()
        {
            static Chat chat;
            return chat;
        }

        // the keys of the channels the local player is in, as two servers compare them
        std::set<std::string> MyChannels(Player* local)
        {
            std::set<std::string> keys;
            for (::Channel* channel : local->GetJoinedChannels())
                if (channel && channel->GetPassword().empty() && !channel->IsLFG())
                    keys.insert(ChannelKey(channel, local->GetZoneId()));
            return keys;
        }

        void OnLogin(Player* player) override
        {
            _remote.clear();
            _sent.clear();
            _timer = 0;
            _sentAt = 0;
        }

        void Update(uint32_t diff, Player* local) override
        {
            _timer += diff;
            if (_timer < 1000)
                return;
            _timer = 0;
            Service& service = Service::Get();
            std::set<std::string> keys = MyChannels(local);
            int64_t now = service.NetNow();
            if (keys == _sent && now - _sentAt < 20000)
                return;
            _sent = keys;
            _sentAt = now;
            std::string text;
            for (std::string const& k : keys)
                text += (text.empty() ? "" : "\n") + k;
            Send(Channels, 0, text, "", Id128{}, 0, nullptr);
        }

        // the members of a channel on other servers: the peers in touch whose last list holds the channel
        static void RemoteMembers(::Channel const* channel, std::vector<std::pair<ObjectGuid, uint8>>& out)
        {
            Service& service = Service::Get();
            Player* local = service.Local();
            if (!service.Enabled() || !local)
                return;
            std::string key = ChannelKey(channel, local->GetZoneId());
            for (auto const& [peer, keys] : Get()._remote)
            {
                PeerInfo* info = service.FindPeer(peer);
                KnownCharacter* known = Characters::Get().Meet(peer);
                if (known && info && info->inTouch && keys.count(key))
                    out.emplace_back(ObjectGuid::Create<HighGuid::Player>(known->guid), uint8(0));
            }
        }

        // a player joined or left a named channel: the channel says so, as it does for its own members
        void Announce(Player* local, Id128 const& peer, std::string const& key, bool joined)
        {
            if (key.compare(0, 2, "c:") != 0)
                return;   // (zone channels announce nothing in the game)
            KnownCharacter* known = Characters::Get().Meet(peer);
            if (!known)
                return;
            for (::Channel* channel : local->GetJoinedChannels())
                if (channel && ChannelKey(channel, local->GetZoneId()) == key)
                {
                    WorldPacket data(SMSG_CHANNEL_NOTIFY, 1 + channel->GetName().size() + 1 + 8);
                    data << uint8(joined ? 0x00 : 0x01) << channel->GetName() << ObjectGuid::Create<HighGuid::Player>(known->guid);
                    local->SendDirectMessage(&data);
                    return;
                }
        }

        void Rooms(Player* local, std::vector<Room>& rooms) override
        {
            // the channels the character is in: a room each (channels with a password are not part of this)
            std::set<std::string> seen;
            for (::Channel* channel : local->GetJoinedChannels())
            {
                if (!channel || !channel->GetPassword().empty() || channel->IsLFG())
                    continue;
                std::string key = ChannelKey(channel, local->GetZoneId());
                if (!seen.insert(key).second)
                    continue;
                if (channel->IsConstant())
                {
                    Room room;
                    room.name = ZoneChannelRoom(Service::Get().RoomScope(), local->GetMapId(), local->GetZoneId(), BaseName(channel->GetName()));
                    rooms.push_back(room);
                }
                else
                    rooms.push_back(PrivateRoom(Service::Get().RoomScope(), "chan", ChannelSecret(Lower(channel->GetName()))));
            }
        }

        void OnEvent(InEvent const& e, Player* local) override
        {
            if (e.kind == InEvent::InTouch && local && !e.flag)
            {
                // a player who is no longer in touch leaves the channels it was in
                auto it = _remote.find(e.peer);
                if (it != _remote.end())
                {
                    for (std::string const& key : it->second)
                        Announce(local, e.peer, key, false);
                    _remote.erase(it);
                }
                return;
            }
            if (e.kind == InEvent::Gone)
            {
                _remote.erase(e.peer);
                return;
            }
            if (e.kind == InEvent::LinkUp)
            {
                _sentAt = 0;   // (the new link gets the list of channels at the next update)
                int64_t now = Service::Get().NetNow();
                for (auto it = g_pending.begin(); it != g_pending.end();)
                {
                    if (it->to == e.peer)
                    {
                        Send(Whisper, it->lang, it->text, "", it->to, 0, [&](PeerInfo const& p) { return p.cert.id == e.peer; });
                        it = g_pending.erase(it);
                    }
                    else if (it->expires < now)
                        it = g_pending.erase(it);
                    else
                        ++it;
                }
                return;
            }
            if (e.kind != InEvent::Message || e.type != game::MsgChat || !local || !local->IsInWorld())
                return;
            Reader r(e.data);
            Kind kind = static_cast<Kind>(r.U8());
            uint32_t lang = r.U32();
            std::string text = r.Str(kind == Channels ? 2000 : 300), channel = r.Str(200);
            Id128 to = r.Id();
            uint32_t number = r.U32();
            if (!r.ok())
                return;
            if (kind == Channels)
            {
                // the channels that player is in now: whoever came or went is announced
                std::set<std::string> now;
                size_t at = 0;
                while (at < text.size() && now.size() < 40)
                {
                    size_t nl = text.find('\n', at);
                    now.insert(text.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
                    if (nl == std::string::npos)
                        break;
                    at = nl + 1;
                }
                std::set<std::string> before = _remote[e.peer];
                PeerInfo* info = Service::Get().FindPeer(e.peer);
                if (info && info->inTouch)
                {
                    for (std::string const& k : now)
                        if (!before.count(k))
                            Announce(local, e.peer, k, true);
                    for (std::string const& k : before)
                        if (!now.count(k))
                            Announce(local, e.peer, k, false);
                }
                _remote[e.peer] = now;
                return;
            }
            PeerInfo* peer = Service::Get().FindPeer(e.peer);
            KnownCharacter* known = Characters::Get().Meet(e.peer);
            if (!peer || !known)
                return;
            Player* mirror = Mirrors::Get().MirrorOf(e.peer);
            ObjectGuid sender = ObjectGuid::Create<HighGuid::Player>(known->guid);
            switch (kind)
            {
                case Roll:
                {
                    // the roller's server drew the number: everyone shows that result (as the mirror's roll), never one of their own. In the party, or in range.
                    uint32_t minimum = lang, maximum = number, result = static_cast<uint32_t>(strtoul(text.c_str(), nullptr, 10));
                    bool inParty = local->GetGroup() && local->GetGroup()->IsMember(sender);
                    if (result < minimum || result > maximum || (!inParty && !InSight(*peer, local, 40.0f)))
                        break;
                    WorldPacket data(MSG_RANDOM_ROLL, 20);
                    data << uint32(minimum) << uint32(maximum) << uint32(result) << sender;
                    local->SendDirectMessage(&data);
                    break;
                }
                case Say:
                    if (mirror) mirror->Say(text, Language(lang));
                    break;
                case TextEmote:
                    if (mirror) mirror->TextEmote(text);
                    break;
                case Yell:
                    if (mirror)
                        mirror->Yell(text, Language(lang));
                    else if (InSight(*peer, local, 300.0f))
                        Deliver(local, CHAT_MSG_YELL, lang, sender, ObjectGuid::Empty, text, "");
                    break;
                case EmoteAnim:
                    if (mirror) mirror->HandleEmoteCommand(number);
                    break;
                case PresetEmote:
                    if (mirror)
                    {
                        // The target is named by its network identity when it is a character: it is shown under the name this player knows it by (met by
                        // this very emote when unknown, never under another player's number), and as nobody when this player ignores it.
                        if (r.remaining() > 0)
                        {
                            Bytes certBytes = r.Blob(1024);
                            std::string realName = r.Str(32);
                            Certificate cert;
                            if (r.ok() && Certificate::Decode(certBytes, cert) && cert.Verify())
                                text = TargetName(cert, realName, local);
                        }
                        WorldPacket data(SMSG_TEXT_EMOTE, 20 + text.size());
                        data << mirror->GetGUID() << uint32(number) << uint32(lang) << uint32(text.size() + 1) << text;
                        mirror->SendMessageToSetInRange(&data, 40.0f, true);
                        if (CreatureEmoteAnimation(number))
                            mirror->HandleEmoteCommand(CreatureEmoteAnimation(number));
                    }
                    break;
                case Whisper:
                    if (to == Service::Get().Me().id || to == Id128{})
                    {
                        Deliver(local, CHAT_MSG_WHISPER, lang, sender, local->GetGUID(), text, "");
                    }
                    break;
                case Channel_:
                    for (::Channel* c : local->GetJoinedChannels())
                        if (c && ChannelKey(c, local->GetZoneId()) == channel)
                        {
                            Deliver(local, CHAT_MSG_CHANNEL, lang, sender, sender, text, c->GetName());
                            break;
                        }
                    break;
                case Party:
                    if (Group* group = local->GetGroup())
                        if (group->IsMember(sender))
                            Deliver(local, ChatMsg(number), lang, sender, ObjectGuid::Empty, text, "");
                    break;
            }
        }

        std::string TargetName(Certificate const& cert, std::string const& realName, Player* local)
        {
            Service& service = Service::Get();
            if (cert.id == service.Me().id)
                return local->GetName();
            if (local->GetSocial())
                if (KnownCharacter* known = Characters::Get().Find(cert.id))
                    if (local->GetSocial()->HasIgnore(ObjectGuid::Create<HighGuid::Player>(known->guid)))
                        return std::string();   // (it does not exist for this player: the emote is aimed at nobody)
            KnownCharacter* known = Characters::Get().Meet(cert.id);
            if (!known)
            {
                // an unknown character: it is met by seeing the emote, with a local name by the usual rule (a number if another character has the name)
                known = &Characters::Get().Learn(cert, realName, 1, 1, 0, 1);
            }
            service.Meet(cert.id);
            return known->localName;
        }

        static void Deliver(Player* local, ChatMsg type, uint32_t lang, ObjectGuid sender, ObjectGuid receiver, std::string const& text, std::string const& channel)
        {
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, type, Language(lang), sender, receiver, text, 0, "", "", 0, false, channel);
            local->SendDirectMessage(&data);
        }

        // the animation that goes with a preset text emote (the text emote table of the client's data)
        static uint32_t CreatureEmoteAnimation(uint32_t textEmote)
        {
            if (EmotesTextEntry const* entry = sEmotesTextStore.LookupEntry(textEmote))
                return entry->textid;
            return 0;
        }
    private:
        std::map<Id128, std::set<std::string>> _remote;   // the channels each peer told it is in
        std::set<std::string> _sent;
        uint32_t _timer = 0;
        int64_t _sentAt = 0;
    };

    // ---------------------------------------------------------------- the local player's words

    class ChatHooks : public PlayerScript
    {
    public:
        ChatHooks() : PlayerScript("P2PChat", { PLAYERHOOK_ON_BEFORE_SEND_CHAT_MESSAGE, PLAYERHOOK_CAN_PLAYER_USE_CHANNEL_CHAT,
            PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT, PLAYERHOOK_ON_EMOTE, PLAYERHOOK_ON_TEXT_EMOTE }) { }

        static Player* Mine(Player* player)
        {
            Service& s = Service::Get();
            return s.Enabled() && !player->IsMirror() && s.Local() == player ? player : nullptr;
        }

        void OnPlayerBeforeSendChatMessage(Player* player, uint32& type, uint32& lang, std::string& msg) override
        {
            if (!Mine(player) || msg.empty())
                return;
            if (type == CHAT_MSG_SAY)
                Send(Say, lang, msg, "", Id128{}, 0, [player](PeerInfo const& p) { return InSight(p, player, 60.0f); });
            else if (type == CHAT_MSG_YELL)
                Send(Yell, lang, msg, "", Id128{}, 0, [player](PeerInfo const& p) { return InSight(p, player, 350.0f); });
            else if (type == CHAT_MSG_EMOTE)
                Send(TextEmote, lang, msg, "", Id128{}, 0, [player](PeerInfo const& p) { return InSight(p, player, 60.0f); });
        }

        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, ::Channel* channel) override
        {
            if (Mine(player) && channel && channel->GetPassword().empty())
                Send(Channel_, lang, msg, ChannelKey(channel, player->GetZoneId()), Id128{}, 0, nullptr);
            return true;
        }

        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Group* group) override
        {
            if (Mine(player) && group)
                Send(Party, lang, msg, "", Id128{}, type, [group](PeerInfo const& p) {
                    KnownCharacter* k = Characters::Get().Find(p.cert.id);
                    return k && group->IsMember(ObjectGuid::Create<HighGuid::Player>(k->guid));
                });
            return true;
        }

        void OnPlayerEmote(Player* player, uint32 emote) override
        {
            if (Mine(player))
                Send(EmoteAnim, 0, "", "", Id128{}, emote, [player](PeerInfo const& p) { return InSight(p, player, 60.0f); });
        }

        void OnPlayerTextEmote(Player* player, uint32 textEmote, uint32 emoteNum, ObjectGuid guid) override
        {
            if (!Mine(player))
                return;
            std::string target;
            Writer identity;
            if (Unit* unit = ObjectAccessor::GetUnit(*player, guid))
            {
                target = unit->GetName();
                // a character is named by its network identity (the receiver shows it under the name it knows it by)
                Player* targetPlayer = unit->ToPlayer();
                Id128 id = targetPlayer == player ? Service::Get().Me().id : (targetPlayer && targetPlayer->IsMirror() ? Mirrors::Get().PeerOf(targetPlayer->GetGUID()) : Id128{});
                if (!(id == Id128{}))
                {
                    if (id == Service::Get().Me().id)
                        identity.Blob(Service::Get().MyCertificate().Encode()).Str(player->GetName());
                    else if (KnownCharacter* known = Characters::Get().Find(id))
                        identity.Blob(known->cert.Encode()).Str(known->realName);
                }
            }
            Writer w;
            w.U8(PresetEmote).U32(emoteNum).Str(target).Str("").Id(Id128{}).U32(textEmote).Raw(identity.data);
            std::function<bool(PeerInfo const&)> filter = [player](PeerInfo const& p) { return InSight(p, player, 60.0f); };
            Service::Get().SayAll(filter, game::MsgChat, w.data);
        }
    };

    // The local player rolls: this server draws the number, shows it to its player and the party members on this server, and tells the others' servers the result
    class RollPackets : public ServerScript
    {
    public:
        RollPackets() : ServerScript("P2PRollPackets", { SERVERHOOK_CAN_PACKET_RECEIVE }) { }
        bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
        {
            Service& service = Service::Get();
            Player* local = service.Local();
            if (packet.GetOpcode() != MSG_RANDOM_ROLL || !service.Enabled() || !local || session != local->GetSession() || !local->IsInWorld())
                return true;
            WorldPacket p(packet);
            p.rpos(0);
            uint32 minimum, maximum;
            p >> minimum >> maximum;
            if (minimum > maximum || maximum > sWorld->getIntConfig(CONFIG_RANDOM_ROLL_MAXIMUM))
                return false;
            uint32 roll = urand(minimum, maximum);
            WorldPacket data(MSG_RANDOM_ROLL, 20);
            data << minimum << maximum << roll << local->GetGUID();
            if (Group* group = local->GetGroup())
                group->BroadcastPacket(&data, false);
            else
                local->SendDirectMessage(&data);
            Group* group = local->GetGroup();
            Send(Roll, minimum, std::to_string(roll), "", Id128{}, maximum, [local, group](PeerInfo const& peer)
            {
                KnownCharacter* k = Characters::Get().Find(peer.cert.id);
                return InSight(peer, local, 40.0f) || (group && k && group->IsMember(ObjectGuid::Create<HighGuid::Player>(k->guid)));
            });
            return false;   // (the core's own roll would draw another number)
        }
    };

    // a whisper to a name the core does not find among its players: a character met and online elsewhere
    static bool RemoteWhisper(Player* sender, std::string const& to, uint32 lang, std::string const& msg)
    {
        Service& service = Service::Get();
        if (!service.Enabled() || sender != service.Local())
            return false;
        ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(to);
        if (!guid)
            return false;
        KnownCharacter* known = Characters::Get().FindByGuid(static_cast<uint32_t>(guid.GetCounter()));
        PeerInfo* peer = known ? service.FindPeer(known->cert.id) : nullptr;
        if (!peer)
            return false;   // (not met now: as if no such player existed)
        if (peer->linked)
            Send(Whisper, lang, msg, "", known->cert.id, 0, [&](PeerInfo const& p) { return p.cert.id == known->cert.id; });
        else
        {
            g_pending.push_back({known->cert.id, msg, lang, service.NetNow() + 10000});
            service.Need(known->cert.id, true);
        }
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER_INFORM, Language(lang), guid, guid, msg, 0);
        sender->SendDirectMessage(&data);
        return true;
    }

    void AddChat()
    {
        Service::Get().AddFeature(&Chat::Get());
        new ChatHooks();
        new RollPackets();
        ::WhisperToRemote = RemoteWhisper;
        ::ChannelRemoteMembers = &Chat::RemoteMembers;
    }
}
