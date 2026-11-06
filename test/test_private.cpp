#include "Check.h"
#include "TestNet.h"
#include "../src/core/Node.h"
using namespace p2p;

struct P : Listener
{
    Account account; Certificate cert; TestHost host; Node node; int announced = 0;
    P(TestNet& net) : account(Account::Generate()), cert(Certificate::Make(account, crypto::RandomId())), host(net), node(Node::Config(), account, cert, host, *this) { net.Register(&host); }
    bool WantLink(Peer const& p) override { return !p.rooms.empty(); }
    void OnPeerAnnounced(Peer&, bool) override { ++announced; }
};

int main()
{
    TestNet net;
    net.now = 1'700'000'000'000;
    P a(net), b(net), spy(net);
    std::vector<P*> all{&a, &b, &spy};
    auto run = [&](int ms) { for (int t = 0; t < ms; t += 50) { net.Advance(50); for (P* p : all) p->node.Tick(net.now); } };
    for (P* p : all) p->node.Start(net.now);
    Id128 party = crypto::RandomId();
    Room room = PrivateRoom(1, "party", PartySecret(party));
    a.node.SetRooms({room});
    b.node.SetRooms({room});
    // the spy learned the room's NAME from the node (it saw the topic) but not the secret: it subscribes to the topic with a wrong proof key
    Room guessed;
    guessed.name = room.name;
    guessed.priv = true;
    guessed.proofKey = crypto::Sha256(ToBytes("a guess"));
    spy.node.SetRooms({guessed});
    run(3000);
    CHECK(a.node.LinkCount() == 1 && b.node.LinkCount() == 1);
    CHECK(!a.node.Find(spy.cert.id));           // its announcements do not carry the room's proof
    CHECK(!b.node.Find(spy.cert.id));
    CHECK(spy.node.PeerCount() == 0);            // it cannot read the members' announcements as proper ones either
    // a hand-made announcement with the right name and no proof is dropped
    Writer w;
    w.U8(1).Str(spy.node.Inbox()).Id(crypto::RandomId()).Str(room.name).Blob(Bytes());
    Bytes forged = Envelope::Make(spy.account, spy.cert, Room{room.name, false, {}}, net.now, w.data);
    HostEvent e; e.kind = HostEvent::RoomMessage; e.name = room.name; e.data = forged;
    a.host._events.push_back(e);
    run(500);
    CHECK(!a.node.Find(spy.cert.id));
    // a room of another realm has an unrelated name even for the same secret
    CHECK(PrivateRoom(2, "party", PartySecret(party)).name != room.name);
    DONE();
}
