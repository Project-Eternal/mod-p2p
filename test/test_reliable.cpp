#include "Check.h"
#include "../src/core/Reliable.h"
#include <random>
using namespace p2p;

// Two ends over a lossy, reordering, duplicating pipe: everything arrives once, in order.
struct Pipe
{
    struct Item { int64_t at; bool ack; uint64_t seq, a, b; Bytes msg; bool toB; };
    std::vector<Item> items;
    std::mt19937 rng{7};
    double loss, dup;
    int64_t delay, jitter;
    Pipe(double l, double d, int64_t dl, int64_t j) : loss(l), dup(d), delay(dl), jitter(j) { }
    void Put(Item it, int64_t now)
    {
        if (std::uniform_real_distribution<>(0, 1)(rng) < loss)
            return;
        it.at = now + delay + (jitter ? (int64_t)(rng() % jitter) : 0);
        items.push_back(it);
        if (std::uniform_real_distribution<>(0, 1)(rng) < dup)
        {
            it.at += (int64_t)(rng() % 50);
            items.push_back(it);
        }
    }
};

static void Scenario(double loss, double dup, int64_t delay, int64_t jitter, int count)
{
    ReliableStream a, b;
    Pipe pipe(loss, dup, delay, jitter);
    std::vector<Bytes> got;
    for (int i = 0; i < count; ++i)
        CHECK(a.Queue(Writer().U32(i).data));
    int64_t now = 0;
    for (; now < 600000 && (got.size() < (size_t)count || a.Pending()); now += 10)
    {
        a.Due(now, 2 * delay, [&](ReliableStream::Frame f) { pipe.Put({0, false, f.seq, 0, 0, *f.message, true}, now); });
        if (b.AckDue())
        {
            uint64_t c, e;
            b.MakeAck(c, e);
            pipe.Put({0, true, 0, c, e, {}, false}, now);
        }
        for (size_t i = 0; i < pipe.items.size();)
        {
            if (pipe.items[i].at > now) { ++i; continue; }
            Pipe::Item it = pipe.items[i];
            pipe.items.erase(pipe.items.begin() + i);
            if (it.ack)
                a.OnAck(it.a, it.b);
            else
                b.OnData(it.seq, it.msg, got);
        }
    }
    CHECK(got.size() == (size_t)count);
    for (size_t i = 0; i < got.size(); ++i)
    {
        Reader r(got[i]);
        CHECK(r.U32() == i);
    }
    CHECK(a.Pending() == 0);
}

int main()
{
    Scenario(0, 0, 20, 0, 1000);          // clean
    Scenario(0.3, 0, 40, 30, 1000);       // 30% loss, reordering by jitter
    Scenario(0.1, 0.3, 40, 100, 1500);    // duplicates, heavy reordering
    Scenario(0.6, 0.1, 80, 50, 300);      // very poor
    // the window: more than fits waits in the queue, nothing is lost
    {
        ReliableStream::Config c;
        c.window = 4;
        ReliableStream a(c), b;
        for (int i = 0; i < 10; ++i)
            a.Queue(Writer().U32(i).data);
        int sent = 0;
        a.Due(0, 10, [&](ReliableStream::Frame) { ++sent; });
        CHECK(sent == 4 && a.InFlight() == 4 && a.Pending() == 10);
        a.OnAck(2, 0);   // 1 and 2 arrived
        a.Due(1, 10, [&](ReliableStream::Frame) { ++sent; });
        CHECK(sent == 6 && a.InFlight() == 4);
    }
    // the bitmap: 1 arrived, 2 lost, 3 and 5 arrived early
    {
        ReliableStream a, b;
        for (int i = 0; i < 5; ++i)
            a.Queue(Writer().U32(i).data);
        std::vector<std::pair<uint64_t, Bytes>> frames;
        a.Due(0, 10, [&](ReliableStream::Frame f) { frames.emplace_back(f.seq, *f.message); });
        std::vector<Bytes> got;
        for (int i : {0, 2, 4})
            b.OnData(frames[i].first, frames[i].second, got);
        CHECK(got.size() == 1);
        uint64_t c, e;
        b.MakeAck(c, e);
        CHECK(c == 1 && e == 0b101);   // 3 -> bit 0, 5 -> bit 2
        a.OnAck(c, e);
        CHECK(a.InFlight() == 2);      // 2 and 4 are still waiting
        int again = 0;
        uint64_t firstResent = 0;
        a.Due(5000, 10, [&](ReliableStream::Frame f) { if (!again++) firstResent = f.seq; });
        CHECK(again == 2 && firstResent == 2);
    }
    DONE();
}
