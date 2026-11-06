#include "Check.h"
#include "TestNet.h"
#include "../src/core/Node.h"
#include <random>
using namespace p2p;

// ---- the stream alone, over a pipe that loses a share of the messages and reorders the rest
struct Pipe
{
    struct Item { int64_t at; bool ack; uint64_t key, version; Bytes msg; std::vector<std::pair<uint64_t, uint64_t>> acks; };
    std::vector<Item> items;
    std::mt19937 rng{3};
    double loss;
    int64_t delay;
    Pipe(double l, int64_t d) : loss(l), delay(d) { }
    void Put(Item it, int64_t now)
    {
        if (std::uniform_real_distribution<>(0, 1)(rng) < loss)
            return;
        it.at = now + delay + (int64_t)(rng() % 60);
        items.push_back(std::move(it));
    }
};

static void StreamAtLoss(double loss)
{
    KeyedStream a, b;
    Pipe pipe(loss, 40);
    // 3 keys, a new value of each every 100 ms for 20 s; a value is "applied" when the receiver finds it newer
    std::map<uint64_t, uint32_t> latest, applied;
    std::map<uint64_t, int64_t> sentAt;
    int64_t maxLag = 0;
    bool wentBack = false;
    int64_t now = 0;
    for (; now < 90000; now += 10)
    {
        if (now < 20000 && now % 100 == 0)
            for (uint64_t k = 1; k <= 3; ++k)
            {
                latest[k]++;
                a.Set(k, Writer().U32(latest[k]).data);
                sentAt[k * 1000000 + latest[k]] = now;
            }
        a.Due(now, 80, [&](uint64_t key, uint64_t version, Bytes const& m) { pipe.Put({0, false, key, version, m, {}}, now); });
        while (b.AckDue())
            pipe.Put({0, true, 0, 0, {}, b.TakeAcks()}, now);
        for (size_t i = 0; i < pipe.items.size();)
        {
            if (pipe.items[i].at > now) { ++i; continue; }
            Pipe::Item it = std::move(pipe.items[i]);
            pipe.items.erase(pipe.items.begin() + i);
            if (it.ack)
                for (auto const& [k, v] : it.acks)
                    a.OnAck(k, v);
            else if (b.OnData(it.key, it.version))
            {
                Reader r(it.msg);
                uint32_t value = r.U32();
                if (value < applied[it.key])
                    wentBack = true;
                applied[it.key] = value;
                maxLag = std::max(maxLag, now - sentAt[it.key * 1000000 + value]);
            }
        }
    }
    for (uint64_t k = 1; k <= 3; ++k)
        CHECK(applied[k] == latest[k]);   // the newest of each key arrived
    CHECK(!wentBack);
    CHECK(a.Pending() == 0);
    std::printf("  loss %.0f%%: max age of an applied value %lld ms, resent %llu\n", loss * 100, (long long)maxLag, (unsigned long long)a.Resent());
    CHECK(maxLag < 6000 + (int64_t)(loss * 4000));
}

// ---- over the node: a lossy link, movement keyed next to ordered events
struct P : Listener
{
    Account account;
    Certificate cert;
    TestHost host;
    Node node;
    std::vector<uint32_t> moves, events;
    std::vector<int64_t> moveAt;
    int64_t* clock;
    P(TestNet& net) : account(Account::Generate()), cert(Certificate::Make(account, crypto::RandomId())), host(net), node(Node::Config(), account, cert, host, *this), clock(&net.now)
    {
        net.Register(&host);
    }
    bool WantLink(Peer const& p) override { return !p.rooms.empty(); }
    void OnMessage(Peer&, uint16_t type, Reader& r, Channel) override
    {
        if (type == 30) { moves.push_back(r.U32()); moveAt.push_back(*clock); }
        if (type == 31) events.push_back(r.U32());
    }
};

int main()
{
    StreamAtLoss(0.0);
    StreamAtLoss(0.3);
    StreamAtLoss(0.6);
    StreamAtLoss(0.8);

    TestNet net;
    net.now = 1'700'000'000'000;
    net.latency = 30;
    P a(net), b(net);
    std::vector<P*> all = {&a, &b};
    auto run = [&](int64_t ms) { for (int64_t t = 0; t < ms; t += 20) { net.Advance(20); for (P* p : all) p->node.Tick(net.now); } };
    for (P* p : all) p->node.Start(net.now);
    Room room; room.name = TileRoom(1, 0, 1, 1);
    a.node.SetRooms({room});
    b.node.SetRooms({room});
    run(3000);
    CHECK(a.node.LinkCount() == 1 && b.node.LinkCount() == 1);
    for (int i = 0; i <= MaxNodes; ++i)
        net.routeLoss[i] = 0.5;
    std::map<uint32_t, int64_t> sentAt;
    int count = 0;
    for (int t = 0; t < 600; ++t)   // 12 s at 50 per second
    {
        if (t % 2 == 0)
        {
            ++count;
            sentAt[count] = net.now;
            a.node.SendKeyed(b.cert.id, 77, 30, Writer().U32(count).data);
        }
        if (t % 20 == 0)
            a.node.Send(b.cert.id, 31, Writer().U32(t / 20).data);
        run(20);
    }
    for (int i = 0; i <= MaxNodes; ++i)
        net.routeLoss[i] = 0.0;
    run(30000);
    CHECK(!b.moves.empty() && b.moves.back() == (uint32_t)count);   // the last movement arrived
    CHECK(std::is_sorted(b.moves.begin(), b.moves.end()));            // never an older one after a newer
    int64_t maxAge = 0;
    for (size_t i = 0; i < b.moves.size(); ++i)
        maxAge = std::max(maxAge, b.moveAt[i] - sentAt[b.moves[i]]);
    std::printf("  node at 50%% loss: %zu of %d movements applied, oldest applied movement was %lld ms old\n", b.moves.size(), count, (long long)maxAge);
    CHECK(maxAge < 5000);
    CHECK(b.events.size() == 30);                                      // the ordered events all arrived, once and in order
    for (size_t i = 0; i < b.events.size(); ++i)
        CHECK(b.events[i] == i);
    DONE();
}
