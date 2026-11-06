#include "Check.h"
#include "TestNet.h"
#include "../src/core/Node.h"
using namespace p2p;

struct P : Listener
{
    Account account;
    Certificate cert;
    TestHost host;
    Node node;
    P(TestNet& net, Node::Config c) : account(Account::Generate()), cert(Certificate::Make(account, crypto::RandomId())), host(net), node(c, account, cert, host, *this)
    {
        net.Register(&host);
    }
    bool WantLink(Peer const& p) override { return !p.rooms.empty(); }
};

static Node::Config Config(bool shard, std::string const& identifier)
{
    Node::Config c;
    c.realm = 1;
    if (shard)
    {
        c.shard = true;
        c.shardKey = Scope::DeriveShardKey(identifier, 1, 1000);   // (few rounds: the test is not about the cost)
    }
    return c;
}

int main()
{
    // derivation: deterministic, depends on the identifier and the realm; names and proofs differ per shard
    Key32 k1 = Scope::DeriveShardKey("alpha", 1, 1000), k1b = Scope::DeriveShardKey("alpha", 1, 1000), k2 = Scope::DeriveShardKey("beta", 1, 1000), k3 = Scope::DeriveShardKey("alpha", 2, 1000);
    CHECK(k1 == k1b && k1 != k2 && k1 != k3);
    Scope pub(1), s1 = Scope::Shard(1, k1), s2 = Scope::Shard(1, k2);
    CHECK(TileRoom(pub, 0, 1, 1) != TileRoom(s1, 0, 1, 1) && TileRoom(s1, 0, 1, 1) != TileRoom(s2, 0, 1, 1));
    CHECK(TileRoom(pub, 0, 1, 1) == "/realm/1/world/0/1_1");
    CHECK(s1.prefix.compare(0, 14, "/realm/1/shard") == 0 && s1.prefix.find("alpha") == std::string::npos && s1.prefix.find(Hex(k1)) == std::string::npos);
    Bytes party = PartySecret(crypto::RandomId());
    CHECK(PrivateRoom(pub, "party", party).name != PrivateRoom(s1, "party", party).name);
    CHECK(PrivateRoom(s1, "party", party).name != PrivateRoom(s2, "party", party).name);
    CHECK(PrivateRoom(s1, "party", party).proofKey != PrivateRoom(pub, "party", party).proofKey);
    CHECK(InboxTopic(s1, "ab") != InboxTopic(pub, "ab"));
    // the full-strength derivation works and takes visible time
    CHECK(Scope::DeriveShardKey("alpha", 1) != k1);

    auto meet = [](Node::Config ca, Node::Config cb, std::string const& roomA, std::string const& roomB, bool& linked, bool& seen)
    {
        TestNet net;
        net.now = 1'700'000'000'000;
        P a(net, ca), b(net, cb);
        std::vector<P*> all = {&a, &b};
        for (P* p : all) p->node.Start(net.now);
        Room ra, rb; ra.name = roomA; rb.name = roomB;
        a.node.SetRooms({ra});
        b.node.SetRooms({rb});
        for (int t = 0; t < 8000; t += 50) { net.Advance(50); for (P* p : all) p->node.Tick(net.now); }
        linked = a.node.LinkCount() == 1 && b.node.LinkCount() == 1;
        seen = a.node.Find(b.cert.id) != nullptr;
    };
    bool linked, seen;
    // the same identifier: they meet and link
    meet(Config(true, "alpha"), Config(true, "alpha"), TileRoom(Scope::Shard(1, k1), 0, 1, 1), TileRoom(Scope::Shard(1, k1), 0, 1, 1), linked, seen);
    // (the test's keys use 1000 rounds on both sides: same key)
    CHECK(linked);
    // different identifiers: other rooms, so they never meet
    meet(Config(true, "alpha"), Config(true, "beta"), TileRoom(s1, 0, 1, 1), TileRoom(s2, 0, 1, 1), linked, seen);
    CHECK(!linked && !seen);
    // no identifier against an identifier: other rooms
    meet(Config(false, ""), Config(true, "alpha"), TileRoom(pub, 0, 1, 1), TileRoom(s1, 0, 1, 1), linked, seen);
    CHECK(!linked && !seen);
    // knowing the room is not enough: a player of the public world (or with another key) that listens in the shard's room is seen, and the link is refused
    meet(Config(true, "alpha"), Config(false, ""), TileRoom(s1, 0, 1, 1), TileRoom(s1, 0, 1, 1), linked, seen);
    CHECK(!linked);
    meet(Config(true, "alpha"), Config(true, "beta"), TileRoom(s1, 0, 1, 1), TileRoom(s1, 0, 1, 1), linked, seen);
    CHECK(!linked);
    // the public world works as before
    meet(Config(false, ""), Config(false, ""), TileRoom(pub, 0, 1, 1), TileRoom(pub, 0, 1, 1), linked, seen);
    CHECK(linked);
    DONE();
}
