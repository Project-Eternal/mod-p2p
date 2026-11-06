#include "Check.h"
#include "TestNet.h"
#include "../src/core/Node.h"
using namespace p2p;

struct Player : Listener
{
    Account account;
    Certificate cert;
    TestHost host;
    Node node;
    std::set<Id128> blocked;
    bool want = true;
    std::vector<std::pair<uint16_t, std::string>> got;
    std::vector<std::string> events;
    int announced = 0;

    Player(TestNet& net, Account a, uint32_t realm = 1)
        : account(a), cert(Certificate::Make(a, crypto::RandomId())), host(net), node(MakeConfig(realm), a, cert, host, *this)
    {
        net.Register(&host);
    }
    static Node::Config MakeConfig(uint32_t realm) { Node::Config c; c.realm = realm; return c; }
    bool Blocked(Certificate const& c) override { return blocked.count(c.id) > 0; }
    bool WantLink(Peer const& p) override { return want && !p.rooms.empty(); }
    void OnPeerAnnounced(Peer& p, bool isNew) override { ++announced; }
    void OnLinkUp(Peer& p) override { events.push_back("up"); }
    void OnLinkDown(Peer& p, std::string const& why) override { events.push_back("down:" + why); }
    void OnPeerGone(Peer& p) override { events.push_back("gone"); }
    void OnMessage(Peer& p, uint16_t type, Reader& r, Channel) override { got.emplace_back(type, r.Str()); }
    void OnNodeState(bool up) override { events.push_back(up ? "node-up" : "node-down"); }
};

static std::vector<Player*> g_all;
static void Run(TestNet& net, int64_t ms, int64_t step = 50)
{
    for (int64_t t = 0; t < ms; t += step)
    {
        net.Advance(step);
        for (Player* p : g_all)
            p->node.Tick(net.now);
    }
}

static Room Pub(std::string const& n) { Room r; r.name = n; return r; }

int main()
{
    TestNet net;
    net.now = 1'700'000'000'000;
    Player a(net, Account::Generate()), b(net, Account::Generate()), c(net, Account::Generate());
    g_all = {&a, &b, &c};
    std::string tile = TileRoom(1, 0, 32, 32), other = TileRoom(1, 0, 33, 32);
    for (Player* p : g_all)
        p->node.Start(net.now);
    a.node.SetRooms({Pub(tile)});
    b.node.SetRooms({Pub(tile)});
    c.node.SetRooms({Pub(other)});
    Run(net, 2000);
    CHECK(a.node.NodeUp());
    // a and b meet in the room and link; c is somewhere else
    CHECK(a.node.Find(b.cert.id) && b.node.Find(a.cert.id));
    CHECK(!a.node.Find(c.cert.id));
    CHECK(a.node.LinkCount() == 1 && b.node.LinkCount() == 1);
    CHECK(a.node.Find(b.cert.id)->direct);   // (the offer is made by the lower identifier)
    CHECK(a.node.Send(b.cert.id, 20, Writer().Str("hello b").data));
    Run(net, 200);
    CHECK(b.got.size() == 1 && b.got[0].first == 20 && b.got[0].second == "hello b");
    // the node saw only encrypted bytes on inboxes (and signed envelopes in rooms)
    for (auto const& [topic, data] : net.wire)
        CHECK(ToString(data).find("hello b") == std::string::npos);

    // c walks into the tile: all three link
    c.node.SetRooms({Pub(tile)});
    Run(net, 2000);
    CHECK(a.node.LinkCount() == 2 && b.node.LinkCount() == 2 && c.node.LinkCount() == 2);
    CHECK(a.node.Broadcast(nullptr, 21, Writer().Str("all").data) == 2);
    Run(net, 100);
    CHECK(b.got.size() == 2 && c.got.size() == 1);

    // c leaves again: links close after the grace time
    c.node.SetRooms({Pub(other)});
    Run(net, 1000);
    CHECK(!a.node.Find(c.cert.id) || a.node.Find(c.cert.id)->rooms.empty());
    Run(net, 8000);
    CHECK(a.node.LinkCount() == 1 && c.node.LinkCount() == 0);
    CHECK(c.events.back().compare(0, 5, "down:") == 0);

    // blocked (ignored): a character that b ignores never appears for b
    {
        Player d(net, Account::Generate());
        g_all.push_back(&d);
        d.node.Start(net.now);
        b.blocked.insert(d.cert.id);
        d.node.SetRooms({Pub(tile)});
        Run(net, 2000);
        CHECK(!b.node.Find(d.cert.id));
        CHECK(a.node.Find(d.cert.id) && a.node.LinkCount() == 2);
        g_all.pop_back();
        net.Kill(&d.host);
    }
    Run(net, 500);

    // a killed connection: the last will tells the peers at once
    a.events.clear();
    net.Kill(&b.host);
    Run(net, 300);
    CHECK(!a.node.Find(b.cert.id));
    CHECK(std::find(a.events.begin(), a.events.end(), "gone") != a.events.end());

    // the node is lost: links keep working, nothing new is found
    {
        net.nodeUp = true;
        Player e(net, Account::Generate());
        g_all.push_back(&e);
        e.node.Start(net.now);
        e.node.SetRooms({Pub(tile)});
        Run(net, 2000);
        CHECK(a.node.Find(e.cert.id) && a.node.LinkCount() >= 1);
        net.nodeUp = false;
        CHECK(a.node.Send(e.cert.id, 22, Writer().Str("still direct").data));
        Run(net, 200);
        CHECK(!e.got.empty() && e.got.back().second == "still direct");
        net.nodeUp = true;
        net.Kill(&e.host);
        g_all.pop_back();
    }

    // relay only: no direct link possible, everything goes through the node's inbox topics and still works
    {
        TestNet n2;
        n2.now = 1'700'000'000'000;
        Player x(n2, Account::Generate()), y(n2, Account::Generate());
        x.host.directAllowed = y.host.directAllowed = false;
        g_all = {&x, &y};
        x.node.Start(n2.now);
        y.node.Start(n2.now);
        x.node.SetRooms({Pub(tile)});
        y.node.SetRooms({Pub(tile)});
        Run(n2, 2000);
        CHECK(x.node.LinkCount() == 1 && !x.node.Find(y.cert.id)->direct);
        CHECK(x.node.Send(y.cert.id, 30, Writer().Str("relayed").data));
        Run(n2, 200);
        CHECK(y.got.size() == 1 && y.got[0].second == "relayed");
        for (auto const& [topic, data] : n2.wire)
            CHECK(ToString(data).find("relayed") == std::string::npos);
        // a replay of a relayed frame is dropped
        Bytes last;
        for (auto const& [topic, data] : n2.wire)
            if (data.size() > 2 && data[0] == 'L' && data[1] == 2)
                last = data;
        HostEvent e;
        e.kind = HostEvent::InboxMessage;
        e.name = x.node.Inbox();
        e.data = last;
        y.host._events.push_back(e);
        Run(n2, 100);
        CHECK(y.got.size() == 1);
        // a forged frame from the node's own made-up peer does nothing
        e.data = Bytes{'L', 2, 0, 1, 0, 0, 0, 0, 0, 0, 0, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
        y.host._events.push_back(e);
        Run(n2, 100);
        CHECK(y.got.size() == 1);
        // a replayed announcement of x (the node keeps what it saw) is not taken again as new; an old one is out of the window
        for (auto const& [topic, data] : n2.wire)
            if (topic == tile)
            {
                HostEvent r;
                r.kind = HostEvent::RoomMessage;
                r.name = topic;
                r.data = data;
                y.host._events.push_back(r);
            }
        int before = y.announced;
        Run(n2, 100);
        CHECK(y.announced == before);
        n2.now += 10 * 60 * 1000;
        for (auto const& [topic, data] : n2.wire)
            if (topic == tile)
            {
                HostEvent r;
                r.kind = HostEvent::RoomMessage;
                r.name = topic;
                r.data = data;
                y.host._events.push_back(r);
            }
        // (the peers' own re-announcements make 'announced' move: compare the peer table instead)
        size_t peers = y.node.PeerCount();
        Run(n2, 100);
        CHECK(y.node.PeerCount() == peers);
    }

    // the network clock converges: two nodes with clocks 3 s apart
    {
        TestNet n3;
        n3.now = 1'700'000'000'000;
        n3.latency = 40;
        Player x(n3, Account::Generate()), y(n3, Account::Generate());
        g_all = {&x, &y};
        x.node.Start(n3.now);
        y.node.Start(n3.now);
        x.node.SetRooms({Pub(tile)});
        y.node.SetRooms({Pub(tile)});
        // y's machine clock is 3 s ahead: emulate by ticking y with a shifted time
        for (int64_t t = 0; t < 120000; t += 50)
        {
            n3.Advance(50);
            x.node.Tick(n3.now);
            y.node.Tick(n3.now + 3000);
        }
        int64_t nx = x.node.NetNow(n3.now), ny = y.node.NetNow(n3.now + 3000);
        CHECK(std::llabs(nx - ny) < 100);
        CHECK(x.node.LinkCount() == 1);
        // the clock is slewed, never stepped: no single correction larger than maxSlewMs
        CHECK(std::llabs(x.node.Offset()) <= 3000 && std::llabs(y.node.Offset()) <= 3000);
    }
    DONE();
}
