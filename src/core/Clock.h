/*
 * Movement by the clock: where an untouched creature is at a given network time, as a pure function of the time, of data every
 * server has (the points of its path, or its spawn point and wander distance) and of its identity (a hash of its spawn key). No server decides it and
 * none sends it. Floating point only through standard functions, so servers of one build agree to the last bit.
 *
 * The core walks a waypoint path as a ring: at each node it waits the node's delay, then walks in a straight line to the next one, the last node being
 * followed by the first (the way back of a path that does not loop is not walked by this core: it stops at the last node, which a creature on the clock
 * is not made to do). The wandering of the core is random; here it is a sequence of slots, each with a destination from the hash.
 */
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

namespace p2p::clk
{
    inline uint64_t Mix(uint64_t x)   // splitmix64
    {
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }
    inline double Unit01(uint64_t h) { return double(h >> 11) / double(1ull << 53); }

    struct Point { float x = 0, y = 0, z = 0; };

    // one node of a path: where, how long the creature waits there (ms), and the speed (yards per second) of the walk that ENDS at this node
    struct Node { Point p; uint32_t delayMs = 0; float speed = 0; };

    struct Where
    {
        bool valid = false;
        Point at;                  // the place
        bool moving = false;       // walking at 'time' (else standing)
        Point to;                  // when moving: the end of the straight walk it is on
        float speed = 0;           // when moving: yards per second
        uint32_t node = 0;         // path: the node it walks to, or waits at
    };

    // The length in milliseconds of one round of the ring (0 when the path cannot be walked: fewer than two nodes, or no speed)
    inline int64_t PathCycleMs(std::vector<Node> const& nodes)
    {
        if (nodes.size() < 2)
            return 0;
        double total = 0;
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            Node const& a = nodes[(i + nodes.size() - 1) % nodes.size()];
            Node const& b = nodes[i];
            if (b.speed <= 0.0f)
                return 0;
            double d = std::sqrt(double(b.p.x - a.p.x) * (b.p.x - a.p.x) + double(b.p.y - a.p.y) * (b.p.y - a.p.y) + double(b.p.z - a.p.z) * (b.p.z - a.p.z));
            total += d / b.speed * 1000.0 + b.delayMs;
        }
        return static_cast<int64_t>(std::llround(total));
    }

    // The place on the ring at network time 'timeMs' (the creature's own 'offsetMs' added): the cycle starts as the creature leaves node 0.
    inline Where PathAt(std::vector<Node> const& nodes, int64_t cycleMs, int64_t timeMs, int64_t offsetMs)
    {
        Where w;
        if (cycleMs <= 0 || nodes.size() < 2)
            return w;
        int64_t t = (timeMs + offsetMs) % cycleMs;
        if (t < 0)
            t += cycleMs;
        double rest = double(t);
        // the cycle: from node 0 (after its delay) walk to 1, wait, walk to 2, wait, ..., walk to 0, wait (the delay of node 0 closes the ring)
        for (size_t k = 1; k <= nodes.size(); ++k)
        {
            Node const& a = nodes[(k - 1) % nodes.size()];
            Node const& b = nodes[k % nodes.size()];
            double d = std::sqrt(double(b.p.x - a.p.x) * (b.p.x - a.p.x) + double(b.p.y - a.p.y) * (b.p.y - a.p.y) + double(b.p.z - a.p.z) * (b.p.z - a.p.z));
            double walk = d / b.speed * 1000.0;
            if (rest < walk)
            {
                double f = walk > 0 ? rest / walk : 1.0;
                w.valid = true;
                w.at = {float(a.p.x + (b.p.x - a.p.x) * f), float(a.p.y + (b.p.y - a.p.y) * f), float(a.p.z + (b.p.z - a.p.z) * f)};
                w.moving = true;
                w.to = b.p;
                w.speed = b.speed;
                w.node = static_cast<uint32_t>(k % nodes.size());
                return w;
            }
            rest -= walk;
            if (rest < b.delayMs)
            {
                w.valid = true;
                w.at = b.p;
                w.node = static_cast<uint32_t>(k % nodes.size());
                return w;
            }
            rest -= b.delayMs;
        }
        w.valid = true;   // (rounding at the very end of the ring)
        w.at = nodes[0].p;
        return w;
    }

    // ---- wandering
    struct Wander
    {
        Point spawn;
        float radius = 0;      // the spawn's wander distance
        float speed = 0;       // the walk speed
        uint64_t identity = 0; // a hash of the spawn's identity
    };

    inline int64_t WanderPauseMs(Wander const& w) { return 4000 + static_cast<int64_t>(Mix(w.identity ^ 0xA5A5) % 6001); }   // 4 to 10 seconds
    inline int64_t WanderSlotMs(Wander const& w)
    {
        if (w.radius <= 0 || w.speed <= 0)
            return 0;
        return static_cast<int64_t>(std::ceil(2.0 * w.radius / w.speed * 1000.0)) + WanderPauseMs(w);
    }
    // the destination of slot k: a point within the wander distance of the spawn point
    inline Point WanderDestination(Wander const& w, int64_t k)
    {
        uint64_t h = Mix(w.identity * 0x9E3779B97F4A7C15ull + static_cast<uint64_t>(k));
        double angle = Unit01(h) * 2.0 * 3.14159265358979323846;
        double dist = w.radius * (0.25 + 0.75 * Unit01(Mix(h)));
        return {float(w.spawn.x + std::cos(angle) * dist), float(w.spawn.y + std::sin(angle) * dist), w.spawn.z};
    }
    // The place at network time 'timeMs': during slot k it walks from the destination of slot k - 1 to that of slot k, then stands until the slot ends.
    // (The first slot the creature ever has is the one its offset puts the clock in; before that nothing exists, as for a path.)
    inline Where WanderAt(Wander const& w, int64_t timeMs, int64_t offsetMs)
    {
        Where out;
        int64_t slotMs = WanderSlotMs(w);
        if (slotMs <= 0)
            return out;
        int64_t t = timeMs + offsetMs;
        int64_t k = t >= 0 ? t / slotMs : -((-t + slotMs - 1) / slotMs);
        int64_t into = t - k * slotMs;
        Point from = WanderDestination(w, k - 1), to = WanderDestination(w, k);
        double d = std::sqrt(double(to.x - from.x) * (to.x - from.x) + double(to.y - from.y) * (to.y - from.y));
        double walk = d / w.speed * 1000.0;
        out.valid = true;
        out.speed = w.speed;
        if (double(into) < walk)
        {
            double f = walk > 0 ? double(into) / walk : 1.0;
            out.at = {float(from.x + (to.x - from.x) * f), float(from.y + (to.y - from.y) * f), w.spawn.z};
            out.moving = true;
            out.to = to;
        }
        else
            out.at = to;
        out.node = static_cast<uint32_t>(k & 0x7FFFFFFF);
        return out;
    }

    // the offset of a creature on its path or in its slots: a hash of its identity, so that two creatures of one path are not in step
    inline int64_t OffsetOf(uint64_t identity, int64_t periodMs)
    {
        return periodMs > 0 ? static_cast<int64_t>(Mix(identity) % static_cast<uint64_t>(periodMs)) : 0;
    }
}
