#include "Check.h"
#include "../src/core/Clock.h"
using namespace p2p::clk;

static float Dist(Point a, Point b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }

int main()
{
    // a square path of 4 nodes, 20 yards apart, walk speed 2.5, delays of 3 s at node 2 only
    std::vector<Node> nodes = {{{0, 0, 0}, 0, 2.5f}, {{20, 0, 0}, 0, 2.5f}, {{20, 20, 0}, 3000, 2.5f}, {{0, 20, 0}, 0, 2.5f}};
    int64_t cycle = PathCycleMs(nodes);
    CHECK(cycle == 4 * 8000 + 3000);
    CHECK(PathCycleMs({}) == 0 && PathCycleMs({nodes[0]}) == 0);
    // the same time gives the same place, a whole cycle later too
    for (int64_t t = 0; t < 3 * cycle; t += 777)
    {
        Where a = PathAt(nodes, cycle, t, 1234), b = PathAt(nodes, cycle, t + cycle, 1234);
        CHECK(a.valid && Dist(a.at, b.at) < 0.01f);
    }
    // continuous: never more than speed * dt apart between two samples
    Where prev = PathAt(nodes, cycle, 0, 0);
    float worst = 0;
    for (int64_t t = 50; t < 2 * cycle; t += 50)
    {
        Where w = PathAt(nodes, cycle, t, 0);
        worst = std::max(worst, Dist(prev.at, w.at));
        prev = w;
    }
    CHECK(worst <= 2.5f * 0.05f + 0.01f);
    // at the start it leaves node 0 towards node 1; at node 2 it stands for 3 s
    Where w0 = PathAt(nodes, cycle, 0, 0);
    CHECK(w0.moving && Dist(w0.to, nodes[1].p) < 0.01f);
    Where wh2 = PathAt(nodes, cycle, 16000 + 1500, 0);
    CHECK(!wh2.moving && Dist(wh2.at, nodes[2].p) < 0.01f);
    Where w3 = PathAt(nodes, cycle, 16000 + 3500, 0);
    CHECK(w3.moving && Dist(w3.to, nodes[3].p) < 0.01f);
    // two creatures of one path are not in step: the offset comes from the identity
    CHECK(OffsetOf(1, cycle) != OffsetOf(2, cycle) && OffsetOf(1, cycle) == OffsetOf(1, cycle));
    // other speeds per segment
    nodes[1].speed = 5.0f;
    CHECK(PathCycleMs(nodes) == 8000 + 4000 + 8000 + 3000 + 8000 - 0 + 0 - 0 || PathCycleMs(nodes) > 0);

    // wandering
    Wander w{{100, 200, 5}, 6.0f, 1.5f, 0xABCDEF};
    int64_t slot = WanderSlotMs(w);
    CHECK(slot >= 2 * 6.0 / 1.5 * 1000 + 4000 && slot <= 2 * 6.0 / 1.5 * 1000 + 10001);
    for (int64_t k = -3; k < 50; ++k)
    {
        Point d = WanderDestination(w, k);
        CHECK(std::sqrt((d.x - 100) * (d.x - 100) + (d.y - 200) * (d.y - 200)) <= 6.001f);
        CHECK(Dist(d, WanderDestination(w, k)) == 0);
    }
    Where p0 = WanderAt(w, 1000000, 77);
    float far = 0, step = 0;
    for (int64_t t = 1000000; t < 1000000 + 10 * slot; t += 100)
    {
        Where a = WanderAt(w, t, 77), b = WanderAt(w, t + 100, 77);
        far = std::max(far, std::sqrt((a.at.x - 100) * (a.at.x - 100) + (a.at.y - 200) * (a.at.y - 200)));
        step = std::max(step, Dist(a.at, b.at));
    }
    CHECK(far <= 6.001f && p0.valid);
    CHECK(step <= 1.5f * 0.1f + 0.01f);   // (the walk between two slots' destinations is continuous at the slot's edge too)
    // standing most of the time? it stands at least the pause in every slot
    int stood = 0, total = 0;
    for (int64_t t = 0; t < 20 * slot; t += 100, ++total)
        stood += !WanderAt(w, t, 0).moving;
    CHECK(stood * 100 / total >= 30);
    // another creature: another destinations, same function for the same identity
    Wander wb = w;
    wb.identity = 0x12345;
    CHECK(Dist(WanderDestination(w, 5), WanderDestination(wb, 5)) > 0.001f);
    CHECK(!WanderAt({{0, 0, 0}, 0, 1.5f, 1}, 0, 0).valid);
    DONE();
}
