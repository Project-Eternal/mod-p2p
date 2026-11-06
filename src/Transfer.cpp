/*
 * Transfers of goods between two servers (see Transfer.h for the rules). The records are rows of the table p2p_xfer; the goods they hold are
 * described in them (ItemData), so that returning or delivering means making an equal item from the description.
 *
 * A one way transfer (a mail, the payment of a cash on delivery mail, a gift in a trade):
 *
 *   giver                                                  receiver
 *   the goods are taken: a record holds them
 *                      -- Offer(id, deadline, sent at, goods) -->
 *                                                         delivers them (bags or mailbox) and keeps the id: a repeat is
 *                                                         answered with Received again, never delivered again
 *                      <------------- Received(id) -----------
 *   the record is deleted
 *   or, at the deadline without Received (or on TooLate): the goods are returned to the giver's own mailbox
 *
 * One case remains in which both players have the goods: the receiver delivered just before the deadline and the receipt arrived after it, or never.
 *
 * An exchange (both give): each server holds its own record of the trade (what its player gives and what it receives) and sends it; each
 * acknowledges that it stored the other's record. A server completes when it holds the other's record, which mirrors its own, and the other's
 * acknowledgement of its record. At the deadline: no record of the other's => it returns its own goods (the other cannot have completed: that
 * needs this server's acknowledgement, which was never given); the other's record but no acknowledgement => it cannot know, and its player gets
 * both: its own goods back and what the other gives. If the records do not mirror each other, both servers see it and each returns its own.
 */
#include "Transfer.h"
#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Log.h"
#include "Mail.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QueryResult.h"
#include "ScriptMgr.h"
#include "SocialMgr.h"
#include "World.h"
#include "WorldSession.h"
#include "Mirrors.h"
#include <algorithm>

namespace p2p
{
    namespace
    {
        enum Step : uint8_t { Offer = 1, Received = 2, TooLate = 3, Record = 4, Ack = 5, Result = 6 };
        constexpr int64_t Hour = 3600 * 1000LL, Week = 7 * 24 * Hour;
        constexpr uint32 NeverExpiresDays = 3650;
        constexpr uint32 MaxMoney = 2000000000u;

        std::string Hexed(Bytes const& b) { return Hex(b); }
        Bytes Unhexed(std::string const& s)
        {
            Bytes out;
            FromHex(s, out);
            return out;
        }
        CharacterDatabaseTransaction g_batch;   // (the world thread only)
        int g_batchDepth = 0;
        std::vector<std::pair<Id128, uint8_t>> g_batchSends;   // the offers and records to send once the batch is committed

        // a write of the records: into the open batch, else at once
        template <typename... Args>
        void Write(std::string_view sql, Args&&... args)
        {
            if (g_batch)
                g_batch->Append(sql, std::forward<Args>(args)...);
            else
                CharacterDatabase.DirectExecute(sql, std::forward<Args>(args)...);
        }
        std::string Escaped(std::string s)
        {
            CharacterDatabase.EscapeString(s);
            return s;
        }
        Id128 IdFromHex(std::string const& s)
        {
            Id128 id{};
            Bytes raw = Unhexed(s);
            if (raw.size() == 16)
                memcpy(id.data(), raw.data(), 16);
            return id;
        }
    }

    // ---------------------------------------------------------------- the wire forms

    Bytes OfferContent::Encode() const
    {
        Writer w;
        w.Str(subject).Str(body).U32(money).U32(cod).U8(stationery).U8(checked).Id(trade).U32(static_cast<uint32_t>(items.size()));
        for (ItemData const& i : items)
            Write(w, i);
        return w.data;
    }

    bool OfferContent::Decode(Bytes const& b, OfferContent& out)
    {
        Reader r(b);
        out.subject = r.Str(200);
        out.body = r.Str(2000);
        out.money = r.U32();
        out.cod = r.U32();
        out.stationery = r.U8();
        out.checked = r.U8();
        out.trade = r.Id();
        uint32_t n = r.U32();
        if (!r.ok() || n > 16)
            return false;
        out.items.clear();
        for (uint32_t i = 0; i < n; ++i)
            out.items.push_back(ReadItem(r));
        return r.ok() && r.end();
    }

    Bytes TradeRecord::Encode() const
    {
        Writer w;
        w.Id(trade).U32(pays).U32(receives).Bool(enchant).Bool(iEnchant).U32(spell);
        Write(w, enchantedItem);
        w.U32(static_cast<uint32_t>(gives.size()));
        for (ItemData const& i : gives)
            Write(w, i);
        w.U32(static_cast<uint32_t>(gets.size()));
        for (ItemData const& i : gets)
            Write(w, i);
        w.U32(static_cast<uint32_t>(reagents.size()));
        for (ItemData const& i : reagents)
            Write(w, i);
        return w.data;
    }

    bool TradeRecord::Decode(Bytes const& b, TradeRecord& out)
    {
        Reader r(b);
        out.trade = r.Id();
        out.pays = r.U32();
        out.receives = r.U32();
        out.enchant = r.Bool();
        out.iEnchant = r.Bool();
        out.spell = r.U32();
        out.enchantedItem = ReadItem(r);
        for (auto* list : {&out.gives, &out.gets, &out.reagents})
        {
            uint32_t n = r.U32();
            if (!r.ok() || n > 16)
                return false;
            list->clear();
            for (uint32_t i = 0; i < n; ++i)
                list->push_back(ReadItem(r));
        }
        return r.ok() && r.end();
    }

    bool TradeRecord::Mirrors(TradeRecord const& o) const
    {
        auto same = [](std::vector<ItemData> const& a, std::vector<ItemData> const& b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (!SameItem(a[i], b[i]))
                    return false;
            return true;
        };
        if (!(trade == o.trade) || !same(gives, o.gets) || !same(gets, o.gives) || pays != o.receives || receives != o.pays || enchant != o.enchant)
            return false;
        if (enchant && (iEnchant == o.iEnchant || spell != o.spell || !SameItem(enchantedItem, o.enchantedItem)))
            return false;
        return true;
    }

    // ---------------------------------------------------------------- the records

    Transfers& Transfers::Get()
    {
        static Transfers transfers;
        return transfers;
    }

    std::string Transfers::NameOf(Id128 const& peer) const { return Service::Get().NameOf(peer); }

    void Transfers::Save(Xfer const& x)
    {
        uint32 flags = (x.haveTheirs ? 1 : 0) | (x.theirAck ? 2 : 0) | (x.myAck ? 4 : 0) | (x.codPending ? 8 : 0);
        Write("REPLACE INTO p2p_xfer (id, role, kind, other, state, sent_at, deadline, giver, mail_id, offer, mine, theirs, flags) "
            "VALUES ('{}', {}, {}, '{}', {}, {}, {}, {}, {}, '{}', '{}', '{}', {})", Hex(x.id), x.role, uint32(x.kind), Hex(x.other), x.state, x.sentAt, x.deadline, x.giver,
            x.mailId, Hexed(x.offer), Hexed(x.mine), Hexed(x.theirs), flags);
    }

    void Transfers::Erase(Xfer const& x)
    {
        Write("DELETE FROM p2p_xfer WHERE id = '{}' AND role = {}", Hex(x.id), x.role);
        if (x.mailId)
            _mails.erase(x.mailId);
        _x.erase({x.id, x.role});
    }

    void Transfers::Load()
    {
        _x.clear();
        _mails.clear();
        if (QueryResult r = CharacterDatabase.Query("SELECT id, role, kind, other, state, sent_at, deadline, giver, mail_id, offer, mine, theirs, flags FROM p2p_xfer"))
            do
            {
                Field* f = r->Fetch();
                Xfer x;
                x.id = IdFromHex(f[0].Get<std::string>());
                x.role = f[1].Get<uint8>();
                x.kind = static_cast<Kind>(f[2].Get<uint8>());
                x.other = IdFromHex(f[3].Get<std::string>());
                x.state = f[4].Get<uint8>();
                x.sentAt = f[5].Get<int64>();
                x.deadline = f[6].Get<int64>();
                x.giver = f[7].Get<uint32>();
                x.mailId = f[8].Get<uint32>();
                x.offer = Unhexed(f[9].Get<std::string>());
                x.mine = Unhexed(f[10].Get<std::string>());
                x.theirs = Unhexed(f[11].Get<std::string>());
                uint32 flags = f[12].Get<uint32>();
                x.haveTheirs = flags & 1;
                x.theirAck = flags & 2;
                x.myAck = flags & 4;
                x.codPending = flags & 8;
                if (x.mailId)
                    _mails[x.mailId] = x.id;
                _x[{x.id, x.role}] = std::move(x);
            }
            while (r->NextRow());
        LOG_INFO("module", "[P2P] {} transfer records", _x.size());
    }

    void Transfers::ReferencedByTransfers(std::vector<std::string>& netIds)
    {
        if (QueryResult r = CharacterDatabase.Query("SELECT DISTINCT other FROM p2p_xfer"))
            do
                netIds.push_back(r->Fetch()[0].Get<std::string>());
            while (r->NextRow());
    }

    void Transfers::OnLogin(Player*)
    {
        if (!_loaded)
        {
            _loaded = true;
            Load();
        }
        _timer = 0;
        _scanTimer = 0;
        _early.clear();
    }

    void Transfers::OnLogout(Player*) { _early.clear(); }

    // a transfer to a peer that is not linked is asked for: the link is made through the pair room (its characters are met and kept)
    void Transfers::Rooms(Player* local, std::vector<Room>& rooms)
    {
        Service& service = Service::Get();
        std::set<Id128> seen;
        for (auto const& [key, x] : _x)
            if (x.role == 0 && (x.state == 0 || x.codPending) && x.giver == local->GetGUID().GetCounter() && seen.insert(x.other).second)
            {
                KnownCharacter* k = Characters::Get().Find(x.other);
                Bytes secret;
                if (k && PairSecret(service.MyAccount(), service.Me(), k->cert, secret))
                    rooms.push_back(PrivateRoom(service.RoomScope(), "pair", secret));
            }
    }

    // ---------------------------------------------------------------- giving

    void Transfers::Give(Kind kind, Id128 const& other, OfferContent const& offer, uint32_t giverGuid, int64_t deadlineMs, Id128 id, int64_t sentAt, uint32_t mailId)
    {
        Xfer x;
        x.id = id == Id128{} ? crypto::RandomId() : id;
        x.role = 0;
        x.kind = kind;
        x.other = other;
        x.giver = giverGuid;
        x.mailId = mailId;
        x.sentAt = sentAt ? sentAt : Service::Get().NetNow();
        x.deadline = deadlineMs;
        x.offer = offer.Encode();
        Save(x);
        if (mailId)
            _mails[mailId] = x.id;
        Service::Get().Need(other, true);   // (the link is asked for; the pair room keeps it findable)
        Service::Get().WantRefresh();
        Xfer& stored = _x[{x.id, 0}] = std::move(x);
        Send(stored, true);
    }

    void Transfers::BeginExchange(Id128 const& other, TradeRecord const& mine, OfferContent const& escrow, uint32_t giverGuid, int64_t deadlineMs)
    {
        Xfer x;
        x.id = mine.trade;
        x.role = 0;
        x.kind = Kind::Exchange;
        x.other = other;
        x.giver = giverGuid;
        x.sentAt = Service::Get().NetNow();
        x.deadline = deadlineMs;
        x.offer = escrow.Encode();
        x.mine = mine.Encode();
        auto early = _early.find(mine.trade);
        Xfer& stored = _x[{x.id, 0}] = std::move(x);
        Save(stored);
        Service::Get().Need(other, true);
        Service::Get().WantRefresh();
        if (early != _early.end())
        {
            // the other side's record came first: it is handled as if it arrived now
            Bytes record = early->second;
            _early.erase(early);
            AcceptRecord(stored, record, Service::Get().Local());
        }
        Send(stored, true);
    }

    Xfer* Transfers::FindOpenExchange(Id128 const& tradeId)
    {
        auto it = _x.find({tradeId, 0});
        return it != _x.end() && it->second.kind == Kind::Exchange ? &it->second : nullptr;
    }

    void Transfers::Reply(Id128 const& to, uint8_t step, Id128 const& id, Bytes const& extra)
    {
        Writer w;
        w.U8(step).Id(id).Raw(extra);
        Service::Get().Say(to, game::MsgTransfer, w.data);
    }

    void Transfers::Send(Xfer& x, bool force)
    {
        if (x.role != 0)
            return;
        if (g_batch)
        {
            // (the other side hears of it only when what it will rely on is kept)
            g_batchSends.push_back({x.id, x.role});
            return;
        }
        Service& service = Service::Get();
        PeerInfo* p = service.FindPeer(x.other);
        if (!p || !p->linked)
            return;
        int64_t now = service.NetNow();
        if (!force && now - x.lastSend < 10000)
            return;
        if (x.state != 0)
            return;   // (after its outcome a server acknowledges nothing more; late messages are answered with the outcome)
        x.lastSend = now;
        if (x.kind == Kind::Exchange)
        {
            if (now > x.deadline)
                return;
            Writer w;
            w.Blob(x.mine);
            Reply(x.other, Record, x.id, w.data);
            if (x.haveTheirs)
                Reply(x.other, Ack, x.id);
            return;
        }
        Writer w;
        w.U8(uint8_t(x.kind)).I64(x.sentAt).I64(x.deadline).Blob(x.offer);
        Reply(x.other, Offer, x.id, w.data);
    }

    // ---------------------------------------------------------------- mail to a character of this server

    void Transfers::SendMailTo(uint32_t guid, uint32_t fromGuid, OfferContent const& c, int64_t delayMs, uint8_t checked)
    {
        Player* local = Service::Get().Local();
        MailDraft draft(c.subject, c.body);
        draft.AddMoney(c.money);
        if (c.cod)
            draft.AddCOD(c.cod);
        CharacterDatabaseTransaction trans = g_batch ? g_batch : CharacterDatabase.BeginTransaction();
        for (ItemData const& d : c.items)
            if (Item* item = Create(local, d))
            {
                item->SaveToDB(trans);
                draft.AddItem(item);
            }
        draft.SendMailTo(trans, MailReceiver(guid), MailSender(MAIL_NORMAL, fromGuid, MailStationery(c.stationery)), MailCheckMask(checked), uint32(std::max<int64_t>(0, delayMs) / 1000),
            NeverExpiresDays);
        if (!g_batch)
            CharacterDatabase.CommitTransaction(trans);
    }

    void Transfers::BeginBatch()
    {
        if (g_batchDepth++ == 0)
            g_batch = CharacterDatabase.BeginTransaction();
    }

    void Transfers::EndBatch(Player* local)
    {
        if (g_batchDepth == 0 || --g_batchDepth > 0)
            return;
        CharacterDatabaseTransaction trans = std::move(g_batch);
        g_batch = nullptr;
        Service::Get().KeepNow(local, trans);
        std::vector<std::pair<Id128, uint8_t>> sends;
        sends.swap(g_batchSends);
        for (auto const& key : sends)
        {
            auto it = Get()._x.find(key);
            if (it != Get()._x.end())
                Get().Send(it->second, true);
        }
    }

    void Transfers::ReturnEscrow(Xfer& x, char const* subject, char const* body, OfferContent const* extra)
    {
        OfferContent c;
        if (!OfferContent::Decode(x.offer, c))
            return;
        KnownCharacter* other = Characters::Get().Meet(x.other);
        if (x.kind == Kind::Mail || x.kind == Kind::Payment)
        {
            // a mail comes back as that mail (flagged as returned); the rest has its own words
            c.checked |= MAIL_CHECK_MASK_RETURNED;
            c.cod = 0;
        }
        else
        {
            c.subject = subject;
            c.body = body;
            c.checked = MAIL_CHECK_MASK_RETURNED;
        }
        if (extra)
        {
            // what the other side gave comes in the same mail (a trade that could not be confirmed)
            c.items.insert(c.items.end(), extra->items.begin(), extra->items.end());
            c.money += extra->money;
        }
        if (x.mailId)
        {
            CharacterDatabase.Execute("DELETE FROM item_instance WHERE guid IN (SELECT item_guid FROM mail_items WHERE mail_id = {})", x.mailId);
            CharacterDatabase.Execute("DELETE FROM mail_items WHERE mail_id = {}", x.mailId);
            CharacterDatabase.Execute("DELETE FROM mail WHERE id = {}", x.mailId);
            if (Player* l = Service::Get().Local())
                l->RemoveMail(x.mailId);
            Characters::Get().ForEach([&](KnownCharacter& k)
            {
                if (Player* m = Mirrors::Get().MirrorOf(k.cert.id))
                    m->RemoveMail(x.mailId);
            });
        }
        SendMailTo(x.giver, other ? other->guid : 0, c, 0, c.checked);
    }

    // ---------------------------------------------------------------- the events

    void Transfers::OnEvent(InEvent const& e, Player* local)
    {
        if (e.kind == InEvent::LinkUp)
        {
            for (auto& [key, x] : _x)
                if (x.role == 0 && x.other == e.peer && local && x.giver == local->GetGUID().GetCounter())
                    Send(x, true);
            return;
        }
        if (e.kind != InEvent::Message || e.type != game::MsgTransfer || !local)
            return;
        Reader r(e.data);
        uint8_t step = r.U8();
        Id128 id = r.Id();
        if (!r.ok())
            return;
        switch (step)
        {
            case Offer:
                OnOffer(e.peer, id, r, local);
                break;
            case Received:
            {
                auto it = _x.find({id, 0});
                if (it == _x.end() || it->second.other != e.peer || it->second.kind == Kind::Exchange)
                    break;
                Xfer& x = it->second;
                if (x.state != 0)
                    break;
                if (x.mailId)
                {
                    CharacterDatabase.Execute("DELETE FROM item_instance WHERE guid IN (SELECT item_guid FROM mail_items WHERE mail_id = {})", x.mailId);
                    CharacterDatabase.Execute("DELETE FROM mail_items WHERE mail_id = {}", x.mailId);
                    CharacterDatabase.Execute("DELETE FROM mail WHERE id = {}", x.mailId);
                    Characters::Get().ForEach([&](KnownCharacter& k)
                    {
                        if (Player* m = Mirrors::Get().MirrorOf(k.cert.id))
                            m->RemoveMail(x.mailId);
                    });
                    _mails.erase(x.mailId);
                    x.mailId = 0;
                }
                OfferContent c;
                bool cod = OfferContent::Decode(x.offer, c) && c.cod > 0 && x.kind == Kind::Mail && !(c.checked & MAIL_CHECK_MASK_RETURNED);
                if (cod)
                {
                    // the record stays until the payment (or the mail returned unpaid) has arrived, and the receiver is looked for at every login
                    x.state = 1;
                    x.codPending = true;
                    Save(x);
                }
                else
                    Erase(x);
                break;
            }
            case TooLate:
            {
                auto it = _x.find({id, 0});
                if (it == _x.end() || it->second.other != e.peer || it->second.state != 0 || it->second.kind == Kind::Exchange)
                    break;
                Xfer& x = it->second;
                ReturnEscrow(x, "Trade cancelled", "The goods could not be delivered in time and are returned to you.");
                x.state = 2;
                Erase(x);
                break;
            }
            case Record:
            {
                Bytes record = r.Blob(1 << 16);
                if (!r.ok())
                    break;
                auto it = _x.find({id, 0});
                if (it == _x.end())
                {
                    if (_early.size() < 16)
                        _early[id] = record;   // (this side has not begun its own yet: the record is kept for it)
                    break;
                }
                if (it->second.other == e.peer && it->second.kind == Kind::Exchange)
                    AcceptRecord(it->second, record, local);
                break;
            }
            case Ack:
            {
                auto it = _x.find({id, 0});
                if (it == _x.end() || it->second.other != e.peer || it->second.kind != Kind::Exchange || it->second.state != 0)
                    break;
                it->second.theirAck = true;
                Save(it->second);
                TryComplete(it->second, local);
                break;
            }
            default:
                break;
        }
    }

    // an offer of a one way transfer
    void Transfers::OnOffer(Id128 const& from, Id128 const& id, Reader& r, Player* local)
    {
        uint8_t kindByte = r.U8();
        int64_t sentAt = r.I64(), deadline = r.I64();
        Bytes content = r.Blob(1 << 16);
        if (!r.ok() || kindByte < 1 || kindByte > 5 || kindByte == uint8_t(Kind::Exchange))
            return;
        Kind kind = static_cast<Kind>(kindByte);
        Service& service = Service::Get();
        KnownCharacter* sender = Characters::Get().Meet(from);
        if (!sender || !local->IsInWorld())
            return;
        int64_t now = service.NetNow();
        auto known = _x.find({id, 1});
        if (known != _x.end())
        {
            Reply(from, Received, id);   // (a repeat: the receipt may have been lost; it is never delivered twice)
            return;
        }
        if (now > deadline)
        {
            Reply(from, TooLate, id);
            return;
        }
        OfferContent c;
        if (!OfferContent::Decode(content, c) || c.money > MaxMoney || c.cod > MaxMoney || c.items.size() > 12)
            return;
        for (ItemData const& i : c.items)
            if (!Plausible(i))
                return;
        if (local->GetSocial() && local->GetSocial()->HasIgnore(ObjectGuid::Create<HighGuid::Player>(sender->guid)))
        {
            Reply(from, TooLate, id);   // (the node would not have linked us: a refusal that returns the goods)
            return;
        }
        Xfer rec;
        rec.id = id;
        rec.role = 1;
        rec.kind = kind;
        rec.other = from;
        rec.state = 1;
        rec.sentAt = sentAt;
        rec.deadline = deadline;
        bool intoBags = kind == Kind::Gift && onGift && onGift(from, c, local);
        if (kind == Kind::Loot && onLoot)
        {
            onLoot(from, c, local);
            intoBags = true;
        }
        if (!intoBags)
        {
            // into the mailbox: the delay of a mail with items counts from the moment the sender sent it (by the real time), so it is at once when that has passed
            int64_t delayMs = 0;
            if (kind == Kind::Mail && !c.items.empty() && !(c.checked & MAIL_CHECK_MASK_RETURNED))
                delayMs = std::max<int64_t>(0, sentAt + int64_t(sWorld->getIntConfig(CONFIG_MAIL_DELIVERY_DELAY)) * 1000 - now);
            if (kind == Kind::Gift)
            {
                c.subject = "Trade with " + sender->localName;
                c.body = "Your trade with " + sender->localName + " was completed while you were away. Here is what you received.";
            }
            else if (kind == Kind::Loot)
            {
                c.subject = "Loot from " + sender->localName;
                c.body = "Here is what you won in the loot of " + sender->localName + "'s party.";
            }
            MailDraft draft(c.subject, c.body);
            draft.AddMoney(c.money);
            if (c.cod)
                draft.AddCOD(c.cod);
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            for (ItemData const& d : c.items)
                if (Item* item = Create(local, d))
                {
                    item->SaveToDB(trans);
                    draft.AddItem(item);
                }
            draft.SendMailTo(trans, MailReceiver(local, local->GetGUID().GetCounter()), MailSender(MAIL_NORMAL, sender->guid, MailStationery(c.stationery)), MailCheckMask(c.checked),
                uint32(delayMs / 1000), NeverExpiresDays);
            CharacterDatabase.CommitTransaction(trans);
            service.Tell(local, "You have mail from " + sender->localName + ".");
        }
        // the record that a repeat is recognized by is written with the delivery (the in-memory copy at once, so that a repeat that comes first is recognized)
        _x[{id, 1}] = rec;
        Save(rec);
        if (kind == Kind::Payment)
            CloseCod(from, true, c.money, 0);
        else if (kind == Kind::Mail && (c.checked & MAIL_CHECK_MASK_RETURNED))
            CloseCod(from, false, 0, c.items.empty() ? 0 : c.items[0].entry);
        Reply(from, Received, id);
    }

    // the payment of a cash on delivery mail, or the mail returned unpaid, has arrived: the sender's record of it ends
    void Transfers::CloseCod(Id128 const& from, bool payment, uint32_t amount, uint32_t firstItem)
    {
        for (auto it = _x.begin(); it != _x.end(); ++it)
        {
            Xfer& x = it->second;
            if (x.role != 0 || !x.codPending || x.other != from)
                continue;
            OfferContent c;
            if (!OfferContent::Decode(x.offer, c))
                continue;
            bool match = payment ? c.cod == amount : (c.items.empty() ? 0 : c.items[0].entry) == firstItem;
            if (!match)
                continue;
            Erase(x);
            Service::Get().WantRefresh();
            return;
        }
    }

    // ---------------------------------------------------------------- exchanges

    void Transfers::AcceptRecord(Xfer& x, Bytes const& record, Player* local)
    {
        if (x.state != 0)
        {
            // after its outcome a server acknowledges nothing more; a completed one acknowledges again (the other's copy of the acknowledgement may have been lost)
            if (x.state == 1)
                Reply(x.other, Ack, x.id);
            return;
        }
        if (!x.haveTheirs)
        {
            TradeRecord theirs, mine;
            if (!TradeRecord::Decode(record, theirs) || !TradeRecord::Decode(x.mine, mine))
                return;
            if (!mine.Mirrors(theirs))
            {
                // one side saw something the other did not (or a server was tampered with): the trade does not happen; both servers see it
                x.theirs = record;
                FinishExchange(x, Outcome::Cancelled, local);
                return;
            }
            x.theirs = record;
            x.haveTheirs = true;
            Save(x);
        }
        Reply(x.other, Ack, x.id);   // "I hold your record"
        TryComplete(x, local);
    }

    void Transfers::TryComplete(Xfer& x, Player* local)
    {
        if (x.state == 0 && x.haveTheirs && x.theirAck)
            FinishExchange(x, Outcome::Done, local);
    }

    void Transfers::FinishExchange(Xfer& x, Outcome outcome, Player* local)
    {
        TradeRecord mine, theirs;
        TradeRecord::Decode(x.mine, mine);
        bool haveTheirs = !x.theirs.empty() && TradeRecord::Decode(x.theirs, theirs);
        std::string who = NameOf(x.other);
        BeginBatch();   // (the end of the trade is kept at once: the record, the delivery and the player together)
        if (outcome == Outcome::Cancelled)
        {
            ReturnEscrow(x, ("Trade with " + who + " cancelled").c_str(), ("The trade with " + who + " could not be completed. What you offered is returned to you.").c_str());
            x.state = 2;
        }
        else if (outcome == Outcome::NotConfirmed)
        {
            OfferContent theirGoods;
            theirGoods.items = theirs.gives;
            theirGoods.money = mine.receives;
            ReturnEscrow(x, ("Trade with " + who + " not confirmed").c_str(), ("The trade with " + who + " could not be confirmed by both sides. What you offered is returned to you, together with what " + who + " offered.").c_str(), &theirGoods);
            x.state = 3;
        }
        else
            x.state = 1;
        LOG_INFO("module", "[P2P] Trade with {} {}", who, outcome == Outcome::Done ? "completed" : outcome == Outcome::Cancelled ? "cancelled" : "not confirmed");
        Bytes mineBytes = x.mine;
        if (onExchange)
            onExchange(x, outcome, mine, haveTheirs ? theirs : TradeRecord());
        if (x.state == 1)
        {
            // kept to acknowledge a repeat of the other's record; forgotten after the deadline
            Save(x);
        }
        else
            Erase(x);
        EndBatch(local);   // (the goods that came must not be lost to a reload: the whole chain of saving runs now)
    }

    // ---------------------------------------------------------------- upkeep

    void Transfers::CheckDeadlines(Player* local)
    {
        int64_t now = Service::Get().NetNow();
        std::vector<Key> keys;
        for (auto const& [key, x] : _x)
            keys.push_back(key);
        for (Key const& key : keys)
        {
            auto it = _x.find(key);
            if (it == _x.end())
                continue;
            Xfer& x = it->second;
            if (x.role == 1)
            {
                if (now > x.deadline + Hour)
                    Erase(x);   // (a repeat of its offer after the deadline is refused as too late; the record is not needed any more)
                continue;
            }
            if (x.state == 1 && x.kind == Kind::Exchange)
            {
                if (now > x.deadline + Hour)
                    Erase(x);
                continue;
            }
            if (x.state != 0 || now < x.deadline)
                continue;
            if (x.kind == Kind::Exchange)
                FinishExchange(x, x.haveTheirs ? Outcome::NotConfirmed : Outcome::Cancelled, local);
            else
            {
                ReturnEscrow(x, "Trade cancelled", "The goods could not be delivered in time and are returned to you.");
                Erase(x);
            }
        }
    }

    // the mails that the core made for characters of other servers are the escrow of mails: a record is made of each, once
    void Transfers::ScanMail(Player* local)
    {
        Service& service = Service::Get();
        QueryResult r = CharacterDatabase.Query("SELECT m.id, m.sender, m.receiver, m.subject, m.body, m.money, m.cod, m.stationery, m.checked, m.deliver_time, m.has_items "
            "FROM mail m JOIN p2p_character p ON p.guid = m.receiver WHERE p.own = 0 AND m.messageType = 0");
        if (!r)
            return;
        struct Row { uint32 id, sender, receiver, money, cod, checked, deliver; uint8 stationery; bool items; std::string subject, body; };
        std::vector<Row> rows;
        do
        {
            Field* f = r->Fetch();
            Row row;
            row.id = f[0].Get<uint32>(); row.sender = f[1].Get<uint32>(); row.receiver = f[2].Get<uint32>();
            row.subject = f[3].Get<std::string>(); row.body = f[4].Get<std::string>();
            row.money = f[5].Get<uint32>(); row.cod = f[6].Get<uint32>(); row.stationery = f[7].Get<uint8>(); row.checked = f[8].Get<uint32>();
            row.deliver = f[9].Get<uint32>(); row.items = f[10].Get<bool>();
            if (!_mails.count(row.id))
                rows.push_back(std::move(row));
        } while (r->NextRow());
        int64_t now = service.NetNow();
        for (Row const& row : rows)
        {
            KnownCharacter* to = Characters::Get().FindByGuid(row.receiver);
            if (!to)
                continue;
            OfferContent c;
            c.subject = row.subject;
            c.body = row.body;
            c.money = row.money;
            c.cod = row.cod;
            c.stationery = row.stationery;
            c.checked = row.checked & (MAIL_CHECK_MASK_COD_PAYMENT | MAIL_CHECK_MASK_RETURNED);
            if (row.items)
                if (QueryResult ir = CharacterDatabase.Query("SELECT i.itemEntry, i.count, i.durability, i.flags, i.randomPropertyId, i.charges, i.enchantments FROM mail_items mi "
                    "JOIN item_instance i ON i.guid = mi.item_guid WHERE mi.mail_id = {}", row.id))
                    do
                    {
                        Field* g = ir->Fetch();
                        ItemData d;
                        d.entry = g[0].Get<uint32>(); d.count = g[1].Get<uint32>(); d.durability = g[2].Get<uint32>(); d.flags = g[3].Get<uint32>();
                        d.randomProperty = g[4].Get<int32>(); d.charges = g[5].Get<std::string>(); d.enchantments = g[6].Get<std::string>();
                        c.items.push_back(d);
                    }
                    while (ir->NextRow());
            // the moment the sender sent it: the delivery time of the core's mail less the delay it has for mail with items (a payment and a returned mail have none)
            bool delayed = row.items && !(row.checked & (MAIL_CHECK_MASK_COD_PAYMENT | MAIL_CHECK_MASK_RETURNED));
            int64_t sentAt = int64_t(row.deliver) * 1000 - (delayed ? int64_t(sWorld->getIntConfig(CONFIG_MAIL_DELIVERY_DELAY)) * 1000 : 0);
            sentAt = std::min(sentAt, now);
            Kind kind = (row.checked & MAIL_CHECK_MASK_COD_PAYMENT) ? Kind::Payment : Kind::Mail;
            Give(kind, to->cert.id, c, row.sender, sentAt + Week, Id128{}, sentAt, row.id);
        }
        if (!rows.empty() && local)
            Service::Get().KeepNow(local);   // (a mail sent, or a payment: the player's save and the records are kept at once)
    }

    void Transfers::Update(uint32_t diff, Player* local)
    {
        _timer += diff;
        _scanTimer += diff;
        if (_scanTimer >= 1500)
        {
            _scanTimer = 0;
            ScanMail(local);
        }
        if (_timer < 1000)
            return;
        _timer = 0;
        CheckDeadlines(local);
        int64_t now = Service::Get().NetNow();
        for (auto it = _early.begin(); it != _early.end();)
            it = _x.count({it->first, 0}) ? _early.erase(it) : std::next(it);
        for (auto& [key, x] : _x)
            if (x.role == 0 && x.state == 0 && x.giver == local->GetGUID().GetCounter())
            {
                PeerInfo* p = Service::Get().FindPeer(x.other);
                if (p && !p->linked)
                    Service::Get().Need(x.other, true);
                Send(x, false);
            }
        (void)now;
    }

    void AddTransfers()
    {
        Service::Get().AddFeature(&Transfers::Get());
    }
}
