/*
 * Transfers of goods between two servers: mail, the payment of a cash on delivery mail, a gift in a trade, and the exchange of a trade. Two rules,
 * the user's decision, hold whatever happens to the connection: goods are never lost (when a server cannot be sure of the outcome it chooses the one
 * that may duplicate goods), and no side is favoured (both follow the same rules; at the latest at the deadline a player who plays by the rules has
 * got back at least what was given).
 *
 * An escrow is what a player gave, taken out of their bags (or their mail row) and kept by their own server in a record of the characters database.
 * Returning it means making it an ordinary mail in the player's mailbox, made again from the description.
 *
 * A one way transfer needs no agreement: goods that arrive belong to the receiver and the receipt frees the sender; an offer that arrives after
 * its deadline is answered "too late" and not delivered. An exchange needs both servers to know that the other knows (two records and two
 * acknowledgements, see the cases in Transfer.cpp).
 */
#pragma once
#include "Items.h"
#include "P2PService.h"
#include <functional>
#include <vector>

class Player;

namespace p2p
{
    enum class Kind : uint8_t { Mail = 1, Payment = 2, Gift = 3, Exchange = 4, Loot = 5 };

    // what a one way transfer carries
    struct OfferContent
    {
        std::string subject, body;
        uint32_t money = 0, cod = 0;
        uint8_t stationery = 0, checked = 0;     // checked: the core's mail flags (cod payment, returned)
        std::vector<ItemData> items;
        Id128 trade{};                           // a gift: the trade it comes from
        Bytes Encode() const;
        static bool Decode(Bytes const& b, OfferContent& out);
    };

    // the record of an exchange as one side saw its trade: it must be the mirror of the other side's
    struct TradeRecord
    {
        Id128 trade{};
        std::vector<ItemData> gives, gets;       // by slot, as the player saw them
        uint32_t pays = 0, receives = 0;         // the money after settling: at most one of them is not zero
        bool enchant = false;                    // an enchantment is part of the trade
        bool iEnchant = false;                   // ... and this side's player casts it (else the other casts it on this side's item)
        uint32_t spell = 0;
        ItemData enchantedItem;                  // the item it goes on (the owner's, which never leaves its owner)
        std::vector<ItemData> reagents;          // what the enchanter's spell consumes
        Bytes Encode() const;
        static bool Decode(Bytes const& b, TradeRecord& out);
        // true when 'other' is the record of the other side of the same trade
        bool Mirrors(TradeRecord const& other) const;
    };

    enum class Outcome : uint8_t { Done = 1, Cancelled = 2, NotConfirmed = 3 };

    struct Xfer
    {
        Id128 id{};
        uint8_t role = 0;                        // 0: this server gives, 1: it receives
        Kind kind = Kind::Mail;
        Id128 other{};
        uint8_t state = 0;                       // 0 open, 1 done, 2 returned to the giver, 3 both sides given
        int64_t sentAt = 0, deadline = 0;        // network time, milliseconds
        uint32_t giver = 0;                      // the local character that gave (its mailbox gets the goods back)
        uint32_t mailId = 0;                     // a mail: the row that holds it until the receipt
        Bytes offer;                             // gives: OfferContent (what is held in escrow); exchange: the escrow in the same form
        Bytes mine, theirs;                      // exchange: TradeRecord
        bool haveTheirs = false, theirAck = false, myAck = false, codPending = false;
        int64_t lastSend = 0;
    };

    class Transfers : public Feature
    {
    public:
        static Transfers& Get();

        // a one way transfer: the goods have been taken from the giver; the offer is kept and sent until it is received or returned
        void Give(Kind kind, Id128 const& other, OfferContent const& offer, uint32_t giverGuid, int64_t deadlineMs, Id128 id = Id128{}, int64_t sentAt = 0, uint32_t mailId = 0);
        // an exchange: the giver's escrow and the record of the trade as the giver saw it
        void BeginExchange(Id128 const& other, TradeRecord const& mine, OfferContent const& escrow, uint32_t giverGuid, int64_t deadlineMs);
        Xfer* FindOpenExchange(Id128 const& tradeId);
        // mail to a character (this server's own player): the goods made again from the description, available after 'delayMs'
        static void SendMailTo(uint32_t guid, uint32_t fromGuid, OfferContent const& content, int64_t delayMs, uint8_t checked);

        // What a trade does is kept at once (the confirmations that follow assume it): between BeginBatch and EndBatch the records and the mails made here go into one
        // transaction, which EndBatch commits synchronously together with the player's own save, and then has the shim and the page write the files (Service::KeepNow).
        static void BeginBatch();
        static void EndBatch(Player* local);

        // The exchange of a trade ended (completed, cancelled, or not confirmed): the trade code delivers what the other gave, applies the enchantment, closes
        // the window. 'theirs' is the other side's record (empty if it never came). The gift of a trade that arrives: returns true when it was put into the bags.
        std::function<void(Xfer&, Outcome, TradeRecord const& mine, TradeRecord const& theirs)> onExchange;
        std::function<bool(Id128 const& from, OfferContent const& gift, Player* local)> onGift;
        // the same for the loot a party member won on another server: the items go into the bags (what does not fit by mail), the gold into the purse
        std::function<void(Id128 const& from, OfferContent const& loot, Player* local)> onLoot;

        void OnLogin(Player* player) override;
        void OnLogout(Player* player) override;
        void OnRealmUnbind() override { _x.clear(); _mails.clear(); _early.clear(); _loaded = false; }   // (read again from the next realm's database at the first login)
        void OnEvent(InEvent const& e, Player* local) override;
        void Update(uint32_t diff, Player* local) override;
        void Rooms(Player* local, std::vector<Room>& rooms) override;

        // the characters a transfer refers to (their rows are never pruned while one exists)
        static void ReferencedByTransfers(std::vector<std::string>& netIds);

    private:
        using Key = std::pair<Id128, uint8_t>;
        void Load();
        void Save(Xfer const& x);
        void Erase(Xfer const& x);
        void Send(Xfer& x, bool force);
        void OnOffer(Id128 const& from, Id128 const& id, Reader& r, Player* local);
        void AcceptRecord(Xfer& x, Bytes const& record, Player* local);
        void ScanMail(Player* local);
        void CheckDeadlines(Player* local);
        void ReturnEscrow(Xfer& x, char const* subject, char const* body, OfferContent const* extra = nullptr);
        void TryComplete(Xfer& x, Player* local);
        void FinishExchange(Xfer& x, Outcome outcome, Player* local);
        void CloseCod(Id128 const& from, bool payment, uint32_t amount, uint32_t firstItem);
        void Reply(Id128 const& to, uint8_t step, Id128 const& id, Bytes const& extra = {});
        std::string NameOf(Id128 const& peer) const;

        std::map<Key, Xfer> _x;
        std::map<uint32_t, Id128> _mails;       // mail row -> the transfer made of it
        std::map<Id128, Bytes> _early;          // records that arrived before this side's own (by trade)
        uint32_t _timer = 0, _scanTimer = 0;
        bool _loaded = false;
    };
}
