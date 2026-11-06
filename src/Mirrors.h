/*
 * Mirrors: another server's player appears in this server's world as an ordinary Player object attached to a session without a connection
 * (a "headless" session, which the core has). What the other player does reaches the mirror in the form of the same requests a game client
 * would send, so that the server's own movement handling and checks see them; what the other player looks like arrives as snapshots.
 */
#pragma once
#include "P2PService.h"
#include "Characters.h"
#include "ObjectGuid.h"
#include <map>
#include <mutex>

class WorldSession;
class Aura;
class Unit;

namespace p2p
{
    class Mirrors : public Feature
    {
    public:
        static Mirrors& Get();

        void OnLogin(Player* player) override;
        void OnLogout(Player* player) override;
        void OnEvent(InEvent const& e, Player* local) override;
        void Update(uint32_t diff, Player* local) override;

        Player* MirrorOf(Id128 const& peer) const;
        std::vector<std::string> Describe() const;   // one line per mirror, for .p2p status
        Id128 PeerOf(ObjectGuid guid) const;   // zero if not a mirror
        // the local player's movement packet, to be told to the peers that can see it
        void LocalMovement(Player* player, uint16_t opcode, Bytes const& payload, bool heartbeat);
        void SendSnapshotTo(Id128 const& peer, bool force);

        static game::Snapshot SnapshotOf(Player* player);
        // a hook (map thread) saw an aura of the local player's (or of a creature this server decides) land on a mirror: its owner must have it too
        void LocalAura(Player* mirror, Aura* aura);
        static Id128 CasterId(ObjectGuid guid);
        // the auras of a unit that show (a character's snapshot, a creature's state), and a unit set to such a list: those the list lacks come, those it does not have go
        static void CollectAuras(Unit* unit, std::vector<game::AuraRecord>& out);
        void SyncAuras(Unit* unit, std::vector<game::AuraRecord> const& auras);
        static bool Syncing();   // the world thread is setting auras from a record: that is not news to tell

    private:
        struct Mirror
        {
            ObjectGuid guid;
            WorldSession* session = nullptr;
            game::Snapshot snapshot;
            bool haveSnapshot = false;
            bool channelFromRecord = false;   // its channel fields were set from a record, not from the channel's own events
        };

        void OnSnapshot(PeerInfo& peer, Bytes const& data, Player* local);
        void OnMove(Id128 const& peerId, Bytes const& data);
        void OnAura(Id128 const& peerId, Bytes const& data, Player* local);
        void SyncMovement(Player* p, game::Snapshot const& s);
        void SyncChannel(struct Mirror& mirror, Player* p, game::Snapshot const& s);
        bool Wanted(PeerInfo const& peer, game::Snapshot const& snap, Player* local, bool check = false) const;
        bool Plausible(game::Snapshot const& snap, Player* local) const;
        Player* Create(PeerInfo& peer, game::Snapshot const& snap, Player* local);
        void Apply(Mirror& mirror, Player* player, game::Snapshot const& snap, bool first);
        void Remove(Id128 const& peer);
        void RemoveAll();

        std::map<Id128, Mirror> _mirrors;
        std::map<Id128, int64_t> _rejected;
        std::map<Id128, game::Snapshot> _snapshots;   // the last snapshot of every peer that sent one (also those without a mirror)
        game::Snapshot _lastSent;
        bool _haveLastSent = false;
        uint32_t _seq = 0;
        int64_t _lastSentAt = 0;
        std::mutex _freshMutex;
        std::map<std::pair<uint64_t, uint32_t>, int64_t> _fresh;   // (mirror guid, spell) -> when a local caster put the aura on the mirror
        uint32_t _timer = 0, _checkTimer = 0;
    };
}
