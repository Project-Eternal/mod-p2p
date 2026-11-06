// Reliable delivery over lossy routes, several relay nodes, the choice between routes, wills, half-open links and the direct retries.
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
    std::vector<uint32_t> got;
    std::vector<std::string> events, network;
    int announced = 0, linkUps = 0;
    std::set<Id128> blocked;
    std::vector<std::string> touch;   // changes of the register: "in" / "out"

    Player(TestNet& net, Account a, uint32_t realm = 1)
        : account(a), cert(Certificate::Make(a, crypto::RandomId())), host(net), node(MakeConfig(realm), a, cert, host, *this)
    {
        net.Register(&host);
    }
    static Node::Config MakeConfig(uint32_t realm) { Node::Config c; c.realm = realm; return c; }
    bool Blocked(Certificate const& c) override { return blocked.count(c.id) > 0; }
    bool WantLink(Peer const& p) override { return want && !p.rooms.empty(); }
    void OnInTouch(Peer& p, bool in) override { touch.push_back(in ? "in" : "out"); }
    bool want = true;
    void OnPeerAnnounced(Peer& p, bool isNew) override { ++announced; }
    void OnLinkUp(Peer& p) override { events.push_back("up"); ++linkUps; }
    void OnLinkDown(Peer& p, std::string const& why) override { events.push_back("down:" + why); }
    void OnPeerGone(Peer& p) override { events.push_back("gone"); }
    void OnMessage(Peer& p, uint16_t type, Reader& r, Channel) override { got.push_back(r.U32()); }
    void OnNetwork(std::string const& kind, Peer* p, std::string const& text) override { network.push_back(kind + ": " + text); }
    int Count(std::string const& e) const { return (int)std::count(events.begin(), events.end(), e); }
    bool Net(std::string const& kind) const { for (auto const& n : network) if (n.compare(0, kind.size(), kind) == 0) return true; return false; }
};

static std::vector<Player*> g_all;
static void Run(TestNet& net, int64_t ms, int64_t step = 20)
{
    for (int64_t t = 0; t < ms; t += step)
    {
        net.Advance(step);
        for (Player* p : g_all)
            p->node.Tick(net.now);
    }
}
static Room Pub(std::string const& n) { Room r; r.name = n; return r; }
static std::string Tile() { return TileRoom(1, 0, 32, 32); }

static void Start(TestNet& net, std::vector<Player*> ps)
{
    g_all = ps;
    for (Player* p : ps)
    {
        p->node.Start(net.now);
        p->node.SetRooms({Pub(Tile())});
    }
}

static TestNet MakeNet(unsigned nodes)
{
    TestNet net;
    net.now = 1'700'000'000'000;
    net.nodes = nodes;
    return net;
}

int main()
{
    // ---- reliable delivery over relayed routes that lose 40% of the frames, in order, once
    {
        TestNet net = MakeNet(2);
        net.latency = 30;
        Player a(net, Account::Generate()), b(net, Account::Generate());
        a.host.directAllowed = b.host.directAllowed = false;
        Start(net, {&a, &b});
        Run(net, 3000);
        CHECK(a.node.LinkCount() == 1 && b.node.LinkCount() == 1);
        net.routeLoss[1] = net.routeLoss[2] = 0.4;
        for (uint32_t i = 0; i < 300; ++i)
        {
            a.node.Send(b.cert.id, 40, Writer().U32(i).data);
            Run(net, 20);
        }
        Run(net, 40000);
        CHECK(b.got.size() == 300);
        bool ordered = true;
        for (size_t i = 0; i < b.got.size(); ++i)
            ordered = ordered && b.got[i] == i;
        CHECK(ordered);
        CHECK(a.node.LinkCount() == 1);
    }

    // ---- a poor connection does not end the link: 80% loss for 100 s, nothing arrives for long stretches, the link stays and repairs
    {
        TestNet net = MakeNet(1);
        net.latency = 100;
        Player a(net, Account::Generate()), b(net, Account::Generate());
        a.host.directAllowed = b.host.directAllowed = false;
        Start(net, {&a, &b});
        Run(net, 3000);
        net.routeLoss[1] = 0.8;
        Run(net, 100000);
        CHECK(a.node.LinkCount() == 1 && b.node.LinkCount() == 1);
        CHECK(a.Count("down:timeout") == 0 && b.Count("down:timeout") == 0);
        net.routeLoss[1] = 0;
        a.node.Send(b.cert.id, 40, Writer().U32(7).data);
        Run(net, 5000);
        CHECK(!b.got.empty() && b.got.back() == 7);
    }

    // ---- two nodes, a player on both is found once; the connection takes the better node, and moves when the other becomes clearly better
    {
        TestNet net = MakeNet(2);
        net.routeLatency[1] = 30;
        net.routeLatency[2] = 30;
        Player a(net, Account::Generate()), b(net, Account::Generate());
        a.host.directAllowed = b.host.directAllowed = false;
        Start(net, {&a, &b});
        Run(net, 4000);
        CHECK(a.node.PeerCount() == 1 && b.node.PeerCount() == 1);
        CHECK(a.node.LinkCount() == 1);
        CHECK(a.linkUps == 1 && b.linkUps == 1);
        Peer* pa = a.node.Find(b.cert.id);
        CHECK(pa && pa->route >= 1);
        // node 0 becomes slow: after three evaluations and the pause, the connection moves to node 1 (route 2)
        net.routeLatency[1] = 400;
        Run(net, 90000);
        CHECK(a.node.Find(b.cert.id)->route == 2 && b.node.Find(a.cert.id)->route == 2);
        CHECK(a.Net("route"));
        size_t movesBefore = std::count_if(a.network.begin(), a.network.end(), [](std::string const& s) { return s.compare(0, 5, "route") == 0; });
        // similar quality: no flapping in 5 minutes
        net.routeLatency[1] = 60;
        net.routeLatency[2] = 41;   // (the test ticks every 20 ms: both are 60 ms, 120 ms there and back)
        Run(net, 300000);
        size_t movesAfter = std::count_if(a.network.begin(), a.network.end(), [](std::string const& s) { return s.compare(0, 5, "route") == 0; });
        if (movesAfter != movesBefore) for (auto const& n : a.network) std::printf("  %s\n", n.c_str());
        CHECK(movesAfter == movesBefore);
        // the node in use goes away: the connection moves at once and nothing is lost
        a.node.Send(b.cert.id, 40, Writer().U32(1).data);
        net.NodeFails(1);
        a.node.Send(b.cert.id, 40, Writer().U32(2).data);
        Run(net, 3000);
        CHECK(a.node.Find(b.cert.id)->route == 1);
        CHECK(a.node.LinkCount() == 1 && b.node.LinkCount() == 1);
        CHECK(b.got.size() >= 2 && b.got[b.got.size() - 2] == 1 && b.got.back() == 2);
        CHECK(a.Net("node-lost"));
        net.NodeReturns(1);
        Run(net, 2000);
        CHECK(!a.Net("node-back"));   // (told only after two stable minutes)
        // a node that keeps coming and going: one "lost" in the chat, one "usable again" once it stayed
        auto count = [&](std::string const& kind) { return (int)std::count_if(a.network.begin(), a.network.end(), [&](std::string const& n) { return n.compare(0, kind.size() + 1, kind + ":") == 0; }); };
        for (int i = 0; i < 4; ++i)
        {
            net.NodeFails(1);
            Run(net, 1000);
            net.NodeReturns(1);
            Run(net, 1000);
        }
        CHECK(count("node-lost") == 1 && count("node-back") == 0 && count("node-quiet") >= 5);
        Run(net, 125000);
        CHECK(count("node-back") == 1 && count("node-lost") == 1);
        Run(net, 125000);
        CHECK(count("node-back") == 1);
        // a node that cannot be reached at all is named once (the host reports each failed attempt)
        for (int i = 0; i < 3; ++i)
        {
            HostEvent e;
            e.kind = HostEvent::NodeFailed;
            e.node = 5;
            e.name = "wss://nowhere.example";
            e.data = ToBytes("the address does not answer");
            net.Deliver(&a.host, e);
            Run(net, 100);
        }
        CHECK(count("node-failed") == 1);
    }

    // ---- wills: a peer that leaves one of two nodes is not gone; gone when every node delivered its will
    {
        TestNet net = MakeNet(2);
        Player a(net, Account::Generate()), b(net, Account::Generate());
        a.host.directAllowed = b.host.directAllowed = false;
        Start(net, {&a, &b});
        Run(net, 3000);
        CHECK(a.node.PeerCount() == 1);
        net.LeaveNode(&b.host, 0);
        Run(net, 500);
        CHECK(a.node.PeerCount() == 1 && a.Count("gone") == 0 && a.node.LinkCount() == 1);
        a.node.Send(b.cert.id, 40, Writer().U32(5).data);
        Run(net, 500);
        CHECK(!b.got.empty() && b.got.back() == 5);
        net.LeaveNode(&b.host, 1);
        Run(net, 500);
        CHECK(a.Count("gone") == 1 && a.node.PeerCount() == 0);
    }

    // ---- a clean stop publishes the will (the broker would not)
    {
        TestNet net = MakeNet(1);
        Player a(net, Account::Generate()), b(net, Account::Generate());
        Start(net, {&a, &b});
        Run(net, 3000);
        b.node.Stop(net.now);
        Run(net, 500);
        CHECK(a.node.PeerCount() == 0 || a.Count("gone") + a.Count("down:peer closed") >= 1);
        CHECK(b.host.counts[HostCommand::Will] == 1);
    }

    // ---- a half-open link repairs itself: b's frames to a are lost during the handshake and for some time after
    {
        TestNet net = MakeNet(1);
        net.latency = 20;
        Player a(net, Account::Generate()), b(net, Account::Generate());
        a.host.directAllowed = b.host.directAllowed = false;
        g_all = {&a, &b};
        a.node.Start(net.now);
        b.node.Start(net.now);
        net.muted.insert(b.host.inbox);   // (the announcements too are lost to a: it learns of b by b's hello or reset)
        a.node.SetRooms({Pub(Tile())});
        b.node.SetRooms({Pub(Tile())});
        Run(net, 8000);
        net.muted.clear();
        Run(net, 30000);
        CHECK(a.node.LinkCount() == 1 && b.node.LinkCount() == 1);
        a.node.Send(b.cert.id, 40, Writer().U32(9).data);
        b.node.Send(a.cert.id, 40, Writer().U32(10).data);
        Run(net, 3000);
        CHECK(!b.got.empty() && b.got.back() == 9);
        CHECK(!a.got.empty() && a.got.back() == 10);
    }

    // ---- direct connection: tried again, and the reason is known
    {
        TestNet net = MakeNet(1);
        Player a(net, Account::Generate()), b(net, Account::Generate());
        b.host.directAllowed = false;
        Start(net, {&a, &b});
        Run(net, 3000);
        Peer* low = a.cert.id < b.cert.id ? a.node.Find(b.cert.id) : b.node.Find(a.cert.id);
        Peer* high = a.cert.id < b.cert.id ? b.node.Find(a.cert.id) : a.node.Find(b.cert.id);
        Player& lowPlayer = a.cert.id < b.cert.id ? a : b;
        CHECK(low && high);
        CHECK(low->directReason == DirectNoPath || low->directReason == DirectDisabled);
        CHECK(high->directReason == DirectNotTried);
        CHECK(lowPlayer.Net("direct-failed"));
        int offers = lowPlayer.host.counts[HostCommand::Connect];
        Run(net, 70000);   // (offered again after a minute)
        CHECK(lowPlayer.host.counts[HostCommand::Connect] == offers + 1);
        // the other side lets direct links through now: the next offer opens one and the connection moves to it
        a.host.directAllowed = b.host.directAllowed = true;
        Run(net, 200000);
        CHECK(a.node.Find(b.cert.id)->direct && b.node.Find(a.cert.id)->direct);
    }

    // ---- quality figures: the delay and the loss of a route are measured
    {
        TestNet net = MakeNet(1);
        net.latency = 50;
        Player a(net, Account::Generate()), b(net, Account::Generate());
        a.host.directAllowed = b.host.directAllowed = false;
        Start(net, {&a, &b});
        net.routeLoss[1] = 0.25;
        Run(net, 120000);
        Peer* p = a.node.Find(b.cert.id);
        CHECK(p && p->routes[1].Finished() >= 10);
        CHECK(p->routes[1].Median() >= 90 && p->routes[1].Median() <= 130);
        CHECK(p->routes[1].Loss() > 0.05 && p->routes[1].Loss() < 0.8);
    }

    // ---- the register of who is in touch: the same on both sides, one change reported once; a peer that ignores us never answers
    {
        TestNet net = MakeNet(1);
        Player a(net, Account::Generate()), b(net, Account::Generate()), c(net, Account::Generate());
        a.want = b.want = c.want = false;   // (no links: only rooms and answers)
        Start(net, {&a, &b, &c});
        c.blocked.insert(a.cert.id);        // c ignores a
        Run(net, 5000);
        Peer* ab = a.node.Find(b.cert.id);
        Peer* ba = b.node.Find(a.cert.id);
        CHECK(ab && ba && ab->inTouch && ba->inTouch);
        CHECK(a.touch.size() >= 1 && std::count(a.touch.begin(), a.touch.end(), "in") == 1);   // reported once
        Peer* ac = a.node.Find(c.cert.id);
        CHECK(!ac || !ac->inTouch);          // c ignores a: never answers, so a does not see it as in touch
        CHECK(!c.node.Find(a.cert.id));
        Peer* ca = c.node.Find(b.cert.id);
        CHECK(ca && ca->inTouch);
        // b leaves the room: out, once
        b.node.SetRooms({});
        Run(net, 1000);
        CHECK(a.touch.back() == "out" && std::count(a.touch.begin(), a.touch.end(), "out") == 1);
    }
    DONE();
}
