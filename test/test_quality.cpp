// The connection quality indicator's formulas (src/core/Quality.h): effective latency, weights, the weighted means, the states, the text.
#include "Check.h"
#include "../src/core/Quality.h"
#include "../src/core/Host.h"
using namespace p2p::quality;

static bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

int main()
{
    // e = (t/2)(1+l)/(1-l): no loss is half the round trip; the one the document gives: 40 ms and 20 % loss -> 30 ms
    CHECK(Near(EffectiveLatency(40, 0), 20));
    CHECK(Near(EffectiveLatency(40, 0.2), 30));
    CHECK(Near(EffectiveLatency(100, 0.5), 150));
    // dead or unmeasured: the worst value, and the cap
    CHECK(Near(EffectiveLatency(-1, 0), WorstMs));
    CHECK(Near(EffectiveLatency(30, 1), WorstMs));
    CHECK(Near(EffectiveLatency(5000, 0), WorstMs));
    CHECK(Near(EffectiveLatency(100, 0.95), WorstMs));
    CHECK(Near(EffectiveLatency(0, 0), 0));

    CHECK(Near(Weight(0), 1));
    CHECK(Near(Weight(10), 0.5));
    CHECK(Near(Weight(90), 0.1));

    CHECK(Combine({}).state == State::None);
    CHECK(Combine({}).count == 0);

    // nobody around: all alike, plain mean
    {
        Sample a, b;
        a.rttMs = 40; a.loss = 0;      // 20 ms
        b.rttMs = 200; b.loss = 0.2;   // 150 ms
        Result r = Combine({a, b});
        CHECK(r.state == State::Everyone);
        CHECK(r.count == 2);
        CHECK(Near(r.latency, 85));
        CHECK(Near(r.loss, 0.1));
    }
    // around: the ones far away or in another place do not count; weights by distance
    {
        Sample near, far, other;
        near.rttMs = 40; near.loss = 0; near.around = true; near.yards = 0;       // 20 ms, weight 1
        far.rttMs = 400; far.loss = 0; far.around = true; far.yards = 90;         // 200 ms, weight 0.1
        other.rttMs = 2000; other.around = false;                                 // not counted
        Result r = Combine({near, far, other});
        CHECK(r.state == State::Around);
        CHECK(r.count == 2);
        CHECK(Near(r.latency, (20 + 0.1 * 200) / 1.1));
        CHECK(Near(r.loss, 0));
    }
    // a dead connection counts as 1000 ms and loss 1 and does not hide the others
    {
        Sample good, dead, none;
        good.rttMs = 20; dead.rttMs = 20; dead.loss = 1; none.rttMs = -1;
        Result r = Combine({good, dead, none});
        CHECK(r.count == 3);
        CHECK(Near(r.latency, (10 + 1000 + 1000) / 3.0));
        CHECK(Near(r.loss, 2 / 3.0));
    }
    // the text of the command
    {
        Result r;
        r.state = State::Around; r.latency = 30.04; r.loss = 0.04; r.count = 3;
        CHECK(Text(r) == "around 30.0 0.0400 3");
        CHECK(Text(Result{}) == "none 0.0 0.0000 0");
        CHECK(Text(Stopped()) == "off 0.0 0.0000 0");
    }
    // the host command exists in the byte form
    {
        p2p::HostCommand c, d;
        c.kind = p2p::HostCommand::Quality;
        c.text = "everyone 12.5 0.0000 2";
        p2p::Bytes b = p2p::Encode(c);
        CHECK(p2p::Decode(b.data(), b.size(), d));
        CHECK(d.kind == p2p::HostCommand::Quality && d.text == c.text && static_cast<int>(d.kind) == 12);
    }
    DONE();
}
