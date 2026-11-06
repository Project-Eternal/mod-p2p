/*
 * Trade between players of two servers. The trade window is the core's own: each server runs it between its local player and the mirror of the other, and
 * what a player does in the window (open, accept, put an item, gold or an enchantment, take it back, cancel) is told to the other server, which does the same
 * through the mirror's session. The mirror holds copies of the items offered (made from a description sent with them).
 *
 * The core's completion of a trade (moving the items between the two Player objects) is not done. When both players have accepted:
 *   - the trade is classified from this side's view: nothing changes hands, a gift (one side gives) or an exchange (both give, or an enchantment is part of
 *     it); money is settled first, so that only the difference changes hands;
 *   - what this player gives is taken out of the bags and held (the escrow, Transfer.cpp), after the record of it is written;
 *   - the window stays open until the goods of the other side have been delivered; they go into the bags if the window is still open, else into the mailbox.
 * The enchantment is part of both records. The item it goes on never leaves its owner, whose server applies the enchantment when the trade succeeds.
 * The window is a state like the others: each server tells whether its window is open (with the trade's identifier) when this changes, and a server whose
 * partner's window closed closes its own.
 */
#include "Mirrors.h"
#include "CharacterCache.h"
#include "Item.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Tokenize.h"
#include "TradeData.h"
#include "Transfer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace p2p
{
    extern thread_local int g_ignoreMirrorStores;   // Loot.cpp
    namespace
    {
        enum Step : uint8_t { Initiate = 1, Begin = 2, Accept = 3, Unaccept = 4, Cancel = 5, Gold = 6, SetItem = 7, ClearItem = 8, Busy = 9, SpellSet = 10, Window = 11 };
        constexpr int64_t Hour = 3600 * 1000LL;
        constexpr uint32_t HoldMs = 20000;   // how long the window waits for the goods of the other side before it closes (they come by mail then)
    }

    class Trade : public Feature
    {
    public:
        static Trade& Get()
        {
            static Trade trade;
            return trade;
        }

        void OnLogin(Player* player) override
        {
            _partner = Id128{};
            _tradeId = Id128{};
            _temps.clear();
            _held = false;
            _lastOpen = false;
            _lastSpell = 0;
            Transfers::Get().onExchange = [this](Xfer& x, Outcome o, TradeRecord const& mine, TradeRecord const& theirs) { Finished(x, o, mine, theirs); };
            Transfers::Get().onGift = [this](Id128 const& from, OfferContent const& gift, Player* local) { return GiftArrived(from, gift, local); };
        }

        // the server stops in order: a trade window that is not yet accepted is cancelled (an accepted trade is a set of records, safe on disk)
        void OnStopping(Player* local) override
        {
            if (local->GetTradeData() && !_held)
            {
                if (!(_partner == Id128{}))
                {
                    Writer w;
                    w.U8(Cancel);
                    Service::Get().Say(_partner, game::MsgTrade, w.data);
                }
                local->TradeCancel(true);
            }
        }

        // ------------------------------------------------------------ what the local player does
        // true: the packet is handled here and must not reach the core
        bool LocalPacket(WorldSession* session, WorldPacket const& packet)
        {
            Service& service = Service::Get();
            Player* local = service.Local();
            if (!service.Enabled() || !local || session != local->GetSession())
                return false;
            WorldPacket p(packet);
            p.rpos(0);
            Writer w;
            Id128 to = _partner;
            bool block = false;
            switch (packet.GetOpcode())
            {
                case CMSG_INITIATE_TRADE:
                {
                    ObjectGuid guid;
                    p >> guid;
                    Id128 peer = Mirrors::Get().PeerOf(guid);
                    if (peer == Id128{})
                        return false;
                    _partner = to = peer;
                    _tradeId = crypto::RandomId();
                    _held = false;
                    service.Meet(peer);
                    w.U8(Initiate).Id(_tradeId);
                    break;
                }
                case CMSG_BEGIN_TRADE: w.U8(Begin); break;
                case CMSG_ACCEPT_TRADE:
                {
                    w.U8(Accept);
                    TradeData* mine = local->GetTradeData();
                    TradeData* his = mine ? mine->GetTraderData() : nullptr;
                    if (mine && his && his->IsAccepted() && !_held)
                    {
                        // the other player accepted before: this is the acceptance that completes the trade, and the core must not complete it
                        mine->SetAccepted(true);
                        block = true;
                    }
                    break;
                }
                case CMSG_UNACCEPT_TRADE: w.U8(Unaccept); break;
                case CMSG_CANCEL_TRADE: w.U8(Cancel); break;
                case CMSG_BUSY_TRADE: case CMSG_IGNORE_TRADE: w.U8(Busy); break;
                case CMSG_SET_TRADE_GOLD:
                {
                    uint32 gold;
                    p >> gold;
                    w.U8(Gold).U32(gold);
                    break;
                }
                case CMSG_SET_TRADE_ITEM:
                {
                    uint8 tradeSlot, bag, slot;
                    p >> tradeSlot >> bag >> slot;
                    Item* item = local->GetItemByPos(bag, slot);
                    if (!item)
                        return false;
                    w.U8(SetItem).U8(tradeSlot);
                    Write(w, Describe(item));
                    break;
                }
                case CMSG_CLEAR_TRADE_ITEM:
                {
                    uint8 tradeSlot;
                    p >> tradeSlot;
                    w.U8(ClearItem).U8(tradeSlot);
                    break;
                }
                default:
                    return false;
            }
            if (!(to == Id128{}))
                service.Say(to, game::MsgTrade, w.data);
            if (packet.GetOpcode() == CMSG_CANCEL_TRADE || packet.GetOpcode() == CMSG_BUSY_TRADE)
                _held = false;
            if (block)
                BothAccepted(local);
            return block;
        }

        // ------------------------------------------------------------ what the others do
        void OnEvent(InEvent const& e, Player* local) override
        {
            if (e.kind == InEvent::LinkDown || e.kind == InEvent::Gone)
            {
                if (e.peer == _partner && local && local->GetTradeData() && !_held)
                {
                    local->TradeCancel(true);
                    Cleanup(e.peer);
                }
                return;
            }
            if (e.kind != InEvent::Message || e.type != game::MsgTrade || !local || !local->IsInWorld())
                return;
            Player* actor = Mirrors::Get().MirrorOf(e.peer);
            if (!actor)
                return;
            Reader r(e.data);
            Step step = static_cast<Step>(r.U8());
            WorldSession* s = actor->GetSession();
            switch (step)
            {
                case Initiate:
                {
                    Id128 id = r.Id();
                    if (!r.ok())
                        break;
                    _partner = e.peer;
                    _tradeId = id;
                    _held = false;
                    Service::Get().Meet(e.peer);
                    WorldPacket data(CMSG_INITIATE_TRADE, 8);
                    data << local->GetGUID();
                    s->HandleInitiateTradeOpcode(data);
                    break;
                }
                case Begin: { WorldPacket d(CMSG_BEGIN_TRADE, 0); s->HandleBeginTradeOpcode(d); break; }
                case Accept:
                {
                    TradeData* mine = local->GetTradeData();
                    TradeData* his = actor->GetTradeData();
                    if (mine && his && mine->IsAccepted() && !_held && e.peer == _partner)
                    {
                        // both have accepted: the core must not complete it (it would move the items on this side only)
                        his->SetAccepted(true);
                        TradeStatusInfo info;
                        info.Status = TRADE_STATUS_TRADE_ACCEPT;
                        local->GetSession()->SendTradeStatus(info);
                        BothAccepted(local);
                    }
                    else
                    {
                        WorldPacket d(CMSG_ACCEPT_TRADE, 4);
                        d << uint32(0);
                        s->HandleAcceptTradeOpcode(d);
                    }
                    break;
                }
                case Unaccept: { WorldPacket d(CMSG_UNACCEPT_TRADE, 0); s->HandleUnacceptTradeOpcode(d); break; }
                case Cancel:
                    if (!_held)
                    {
                        WorldPacket d(CMSG_CANCEL_TRADE, 0);
                        s->HandleCancelTradeOpcode(d);
                        Cleanup(e.peer);
                    }
                    break;
                case Busy: { WorldPacket d(CMSG_BUSY_TRADE, 0); s->HandleBusyTradeOpcode(d); Cleanup(e.peer); break; }
                case Window:
                {
                    // the other side's window closed (it walked away, entered combat, ...): ours closes too, unless the trade is being settled
                    bool open = r.U8() != 0;
                    Id128 id = r.Id();
                    if (r.ok() && !open && !_held && id == _tradeId && e.peer == _partner && local->GetTradeData())
                    {
                        local->TradeCancel(true);
                        Cleanup(e.peer);
                    }
                    break;
                }
                case SpellSet:
                {
                    uint32 spell = r.U32();
                    if (r.ok() && actor->GetTradeData())
                        actor->GetTradeData()->SetSpell(spell, nullptr);
                    break;
                }
                case Gold:
                {
                    uint32 gold = r.U32();
                    if (!r.ok())
                        break;
                    actor->SetMoney(std::max<uint32>(actor->GetMoney(), gold));   // (the mirror holds what the other player offers)
                    WorldPacket d(CMSG_SET_TRADE_GOLD, 4);
                    d << gold;
                    s->HandleSetTradeGoldOpcode(d);
                    break;
                }
                case SetItem:
                {
                    uint8 tradeSlot = r.U8();
                    ItemData item = ReadItem(r);
                    if (!r.ok() || !Plausible(item) || tradeSlot >= TRADE_SLOT_COUNT)
                        break;
                    ItemPosCountVec dest;
                    if (actor->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item.entry, item.count) != EQUIP_ERR_OK)
                        break;
                    ++g_ignoreMirrorStores;   // (Loot.cpp: this is not an item won by the member)
                    Item* copy = actor->StoreNewItem(dest, item.entry, true, item.randomProperty);
                    --g_ignoreMirrorStores;
                    if (!copy)
                        break;
                    if (item.durability)
                        copy->SetUInt32Value(ITEM_FIELD_DURABILITY, std::min<uint32>(item.durability, copy->GetUInt32Value(ITEM_FIELD_MAXDURABILITY)));
                    copy->SetUInt32Value(ITEM_FIELD_FLAGS, item.flags);
                    auto tokens = Acore::Tokenize(item.enchantments, ' ', false);
                    for (uint32 sl = 0; sl < MAX_ENCHANTMENT_SLOT && sl * 3 + 2 < tokens.size(); ++sl)
                        if (atoi(std::string(tokens[sl * 3]).c_str()))
                            copy->SetEnchantment(EnchantmentSlot(sl), atoi(std::string(tokens[sl * 3]).c_str()), atoi(std::string(tokens[sl * 3 + 1]).c_str()),
                                atoi(std::string(tokens[sl * 3 + 2]).c_str()));
                    // an earlier copy in that slot goes
                    DropTemp(actor, e.peer, tradeSlot);
                    _temps[e.peer][tradeSlot] = copy->GetGUID();
                    WorldPacket d(CMSG_SET_TRADE_ITEM, 3);
                    d << tradeSlot << uint8(copy->GetBagSlot()) << uint8(copy->GetSlot());
                    s->HandleSetTradeItemOpcode(d);
                    break;
                }
                case ClearItem:
                {
                    uint8 tradeSlot = r.U8();
                    WorldPacket d(CMSG_CLEAR_TRADE_ITEM, 1);
                    d << tradeSlot;
                    s->HandleClearTradeItemOpcode(d);
                    DropTemp(actor, e.peer, tradeSlot);
                    break;
                }
            }
        }

        void Update(uint32_t diff, Player* local) override
        {
            // the window is a state: its closing is told with the trade's identifier, whoever closed it and for whatever reason
            bool open = local->GetTradeData() != nullptr;
            if (open != _lastOpen)
            {
                _lastOpen = open;
                if (!open && !(_partner == Id128{}) && !_held)
                {
                    Writer w;
                    w.U8(Window).U8(0).Id(_tradeId);
                    Service::Get().Say(_partner, game::MsgTrade, w.data);
                    Cleanup(_partner);
                }
                if (!open && _held)
                    _held = false;
            }
            // an enchantment cast on the other's item in the trade slot
            if (TradeData* mine = local->GetTradeData())
                if (mine->GetSpell() != _lastSpell && !(_partner == Id128{}))
                {
                    _lastSpell = mine->GetSpell();
                    Writer w;
                    w.U8(SpellSet).U32(_lastSpell);
                    Service::Get().Say(_partner, game::MsgTrade, w.data);
                }
            // the goods of the other side did not come in time: the window closes, they come by mail
            if (_held && local->GetTradeData() && _heldFor.time && Service::Get().NetNow() - _heldFor.time > HoldMs)
            {
                local->TradeCancel(true, TRADE_STATUS_TRADE_COMPLETE);
                _held = false;
                Cleanup(_partner);
            }
        }

        // ------------------------------------------------------------ both have accepted
        void BothAccepted(Player* local)
        {
            Service& service = Service::Get();
            TradeData* mine = local ? local->GetTradeData() : nullptr;
            TradeData* his = mine ? mine->GetTraderData() : nullptr;
            KnownCharacter* partner = Characters::Get().Meet(_partner);
            if (!mine || !his || !partner)
                return;
            auto cancel = [&](char const* why)
            {
                LOG_INFO("module", "[P2P] Trade not completed: {}", why);
                Writer w;
                w.U8(Cancel);
                service.Say(_partner, game::MsgTrade, w.data);
                local->TradeCancel(true);
                Cleanup(_partner);
            };
            if (!local->HasEnoughMoney(mine->GetMoney()))
                return cancel("not enough money");

            TradeRecord record;
            record.trade = _tradeId;
            OfferContent escrow;
            std::vector<Item*> given;
            for (uint8 i = 0; i < TRADE_SLOT_TRADED_COUNT; ++i)
            {
                if (Item* item = mine->GetItem(TradeSlots(i)))
                {
                    if (!item->CanBeTraded(false, true))
                        return cancel("an item cannot be traded");
                    record.gives.push_back(Describe(item));
                    given.push_back(item);
                }
                if (Item* item = his->GetItem(TradeSlots(i)))
                    record.gets.push_back(Describe(item));
            }
            uint32 a = mine->GetMoney(), b = his->GetMoney();
            record.pays = a > b ? a - b : 0;
            record.receives = b > a ? b - a : 0;
            // the enchantment: this side's spell on the other's item, or the other's on this side's item
            uint32 mySpell = mine->GetSpell(), hisSpell = his->GetSpell();
            if (mySpell && hisSpell)
                return cancel("two enchantments in one trade");
            std::vector<ItemData> reagents;
            ObjectGuid enchantGuid;
            if (mySpell || hisSpell)
            {
                SpellInfo const* info = sSpellMgr->GetSpellInfo(mySpell ? mySpell : hisSpell);
                Item* target = mySpell ? his->GetItem(TRADE_SLOT_NONTRADED) : mine->GetItem(TRADE_SLOT_NONTRADED);
                if (!info || !target)
                    return cancel("the enchantment has no item");
                record.enchant = true;
                record.iEnchant = mySpell != 0;
                record.spell = info->Id;
                record.enchantedItem = Describe(target);
                if (!mySpell)
                    enchantGuid = target->GetGUID();
                else
                {
                    // the materials the spell uses leave the enchanter's bags with the rest of what the enchanter gives
                    for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                        if (info->Reagent[i] > 0 && info->ReagentCount[i] > 0)
                        {
                            if (!local->HasItemCount(info->Reagent[i], info->ReagentCount[i]))
                                return cancel("the materials are missing");
                            ItemData d;
                            d.entry = info->Reagent[i];
                            d.count = info->ReagentCount[i];
                            reagents.push_back(d);
                        }
                    if (Item* cast = mine->GetSpellCastItem())
                    {
                        ItemData d = Describe(cast);
                        d.count = 1;
                        reagents.push_back(d);
                    }
                    record.reagents = reagents;
                }
            }
            bool iGive = !record.gives.empty() || record.pays > 0 || !reagents.empty();
            bool theyGive = !record.gets.empty() || record.receives > 0;
            _enchantItem = enchantGuid;
            _held = true;
            _heldFor.time = service.NetNow();
            _heldFor.trade = _tradeId;
            int64_t deadline = service.NetNow() + Hour;

            if (!iGive && !theyGive && !record.enchant)
            {
                // nothing changes hands
                _held = false;
                local->TradeCancel(true, TRADE_STATUS_TRADE_COMPLETE);
                Cleanup(_partner);
                return;
            }
            if (!iGive && !record.enchant)
                return;   // the gift of the other side: the window waits for it (GiftArrived)

            escrow.money = record.pays;
            escrow.items = record.gives;
            escrow.items.insert(escrow.items.end(), reagents.begin(), reagents.end());
            escrow.trade = _tradeId;
            escrow.stationery = 41;   // (the default stationery)
            // the record first, then the goods leave the bags: a server that stops in between has the goods in its record and in its bags (duplicated, never lost).
            // The record, the escrow and the player's save go into one transaction, committed before the other side hears of the acceptance
            Transfers::BeginBatch();
            if (record.enchant || (iGive && theyGive))
                Transfers::Get().BeginExchange(_partner, record, escrow, local->GetGUID().GetCounter(), deadline);
            else
                Transfers::Get().Give(Kind::Gift, _partner, escrow, local->GetGUID().GetCounter(), deadline, _tradeId);
            for (uint8 i = 0; i < TRADE_SLOT_TRADED_COUNT; ++i)
                if (Item* item = mine->GetItem(TradeSlots(i)))
                {
                    mine->SetItem(TradeSlots(i), nullptr);
                    local->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
                }
            for (ItemData const& d : reagents)
                local->DestroyItemCount(d.entry, d.count, true);
            if (record.pays)
                local->ModifyMoney(-int64(record.pays));
            Transfers::EndBatch(local);
        }

        // ------------------------------------------------------------ the goods of the other side
        // put what the other side gave into the bags; what does not fit goes to the mailbox
        static void Deliver(Player* local, std::vector<ItemData> const& items, uint32 money, KnownCharacter* from, char const* subject, char const* body)
        {
            OfferContent mailed;
            for (ItemData const& d : items)
            {
                ItemPosCountVec dest;
                if (Plausible(d) && local->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, d.entry, d.count) == EQUIP_ERR_OK)
                {
                    if (Item* item = Create(local, d))
                    {
                        local->StoreItem(dest, item, true);
                        continue;
                    }
                }
                mailed.items.push_back(d);
            }
            if (money)
                local->ModifyMoney(money);
            if (!mailed.items.empty())
            {
                mailed.subject = subject;
                mailed.body = body;
                Transfers::SendMailTo(local->GetGUID().GetCounter(), from ? from->guid : 0, mailed, 0, 0);
            }
        }

        bool GiftArrived(Id128 const& from, OfferContent const& gift, Player* local)
        {
            if (!_held || !(gift.trade == _heldFor.trade) || from != _partner || !local->GetTradeData())
                return false;
            KnownCharacter* k = Characters::Get().Meet(from);
            Deliver(local, gift.items, gift.money, k, "Trade", "Here is what did not fit into your bags.");
            _held = false;
            local->TradeCancel(true, TRADE_STATUS_TRADE_COMPLETE);
            Cleanup(from);
            return true;
        }

        // the exchange has ended: what the other side gave arrives, the enchantment is applied, the window closes
        void Finished(Xfer& x, Outcome outcome, TradeRecord const& mine, TradeRecord const& theirs)
        {
            Player* local = Service::Get().Local();
            if (!local)
                return;
            KnownCharacter* k = Characters::Get().Meet(x.other);
            std::string who = k ? k->localName : "the other player";
            bool inWindow = _held && x.id == _heldFor.trade && local->GetTradeData() != nullptr;
            if (outcome != Outcome::Cancelled)
            {
                // the owner of the item applies the enchantment that the trade is for
                if (mine.enchant && !mine.iEnchant)
                    ApplyEnchant(local, mine);
                if (outcome == Outcome::Done)
                {
                    std::string subject = "Trade with " + who;
                    if (inWindow)
                        Deliver(local, theirs.gives, mine.receives, k, subject.c_str(), "Here is what did not fit into your bags.");
                    else
                    {
                        // the window is gone (the player left it, or reloaded the page): everything comes by mail, available at once
                        OfferContent c;
                        c.subject = subject;
                        c.body = "Your trade with " + who + " was completed while you were away. Here is what you received.";
                        c.items = theirs.gives;
                        c.money = mine.receives;
                        c.stationery = 41;
                        if (!c.items.empty() || c.money)
                            Transfers::SendMailTo(local->GetGUID().GetCounter(), k ? k->guid : 0, c, 0, 0);
                    }
                }
                // (a trade that could not be confirmed: what the other side gave came with the mail that returned what this side gave)
            }
            if (inWindow)
            {
                _held = false;
                local->TradeCancel(true, outcome == Outcome::Cancelled ? TRADE_STATUS_TRADE_CANCELED : TRADE_STATUS_TRADE_COMPLETE);
                Cleanup(x.other);
            }
        }

        // the enchantment of the trade goes on the item of this side that never left it
        void ApplyEnchant(Player* local, TradeRecord const& mine)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(mine.spell);
            Item* item = _enchantItem ? local->GetItemByGuid(_enchantItem) : nullptr;
            if (!item)
                item = local->GetItemByEntry(mine.enchantedItem.entry);   // (the trade was settled after a restart: the item the record describes)
            if (!info || !item)
                return;
            for (SpellEffectInfo const& effect : info->Effects)
            {
                if (effect.Effect != SPELL_EFFECT_ENCHANT_ITEM && effect.Effect != SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY)
                    continue;
                uint32 enchantId = effect.MiscValue;
                if (!enchantId || !sSpellItemEnchantmentStore.LookupEntry(enchantId))
                    continue;
                EnchantmentSlot slot = effect.Effect == SPELL_EFFECT_ENCHANT_ITEM ? PERM_ENCHANTMENT_SLOT : TEMP_ENCHANTMENT_SLOT;
                local->ApplyEnchantment(item, slot, false);
                item->SetEnchantment(slot, enchantId, slot == TEMP_ENCHANTMENT_SLOT ? 3600 * 1000 : 0, 0);
                local->ApplyEnchantment(item, slot, true);
                break;
            }
        }

        void DropTemp(Player* actor, Id128 const& peer, uint8 slot)
        {
            auto it = _temps[peer].find(slot);
            if (it == _temps[peer].end())
                return;
            if (Item* item = actor->GetItemByGuid(it->second))
                if (!actor->GetTradeData() || !actor->GetTradeData()->HasItem(it->second))
                    actor->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
            _temps[peer].erase(it);
        }

        // the trade is over: copies still in the mirror's bags are removed
        void Cleanup(Id128 const& peer)
        {
            if (Player* actor = Mirrors::Get().MirrorOf(peer))
                for (auto const& [slot, guid] : _temps[peer])
                    if (Item* item = actor->GetItemByGuid(guid))
                        actor->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
            _temps.erase(peer);
            _partner = Id128{};
            _lastSpell = 0;
        }

    private:
        struct Held { int64_t time = 0; Id128 trade{}; };
        Id128 _partner{}, _tradeId{};
        std::map<Id128, std::map<uint8, ObjectGuid>> _temps;
        Held _heldFor;
        ObjectGuid _enchantItem;
        bool _held = false, _lastOpen = false;
        uint32 _lastSpell = 0;
    };

    class TradePackets : public ServerScript
    {
    public:
        TradePackets() : ServerScript("P2PTrade", { SERVERHOOK_CAN_PACKET_RECEIVE }) { }
        bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
        {
            switch (packet.GetOpcode())
            {
                case CMSG_INITIATE_TRADE: case CMSG_BEGIN_TRADE: case CMSG_ACCEPT_TRADE: case CMSG_UNACCEPT_TRADE: case CMSG_CANCEL_TRADE:
                case CMSG_BUSY_TRADE: case CMSG_IGNORE_TRADE: case CMSG_SET_TRADE_GOLD: case CMSG_SET_TRADE_ITEM: case CMSG_CLEAR_TRADE_ITEM:
                    return !Trade::Get().LocalPacket(session, packet);
                default:
                    break;
            }
            return true;
        }
    };

    void AddTrade()
    {
        Service::Get().AddFeature(&Trade::Get());
        new TradePackets();
    }
}
