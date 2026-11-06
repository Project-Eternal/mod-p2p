/*
 * The connection quality indicator: the figures of the peers in touch folded into one value for the page's bar.
 * Pure functions, no game and no network: the service collects the samples, these compute the value, the page draws it.
 */
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace p2p
{
    namespace quality
    {
        constexpr double WorstMs = 1000;   // a dead or unmeasured connection, and the cap of the scale

        // The expected one-way time of a message over a connection with the median round trip 'rttMs' (-1: no probe answered) and the share
        // of lost probes 'loss': a lost message is noticed after about a round trip and sent again, so
        //   e = (t / 2) * (1 + l) / (1 - l)
        inline double EffectiveLatency(double rttMs, double loss)
        {
            if (rttMs < 0 || loss >= 1)
                return WorstMs;
            loss = std::max(0.0, loss);
            return std::min(WorstMs, rttMs / 2 * (1 + loss) / (1 - loss));
        }

        // The weight of a peer whose character is 'yards' away from the local player
        inline double Weight(double yards) { return 1 / (1 + std::max(0.0, yards) / 10); }

        struct Sample
        {
            double rttMs = -1, loss = 0;
            bool around = false;     // its character is on the local player's map, within sight
            double yards = 0;        // (around only)
        };

        // Off is not a measurement: it is what the page is told when the shared world stops (the page takes its bar away); None is "nobody in touch" while it runs
        enum class State { None, Around, Everyone, Off };

        struct Result
        {
            State state = State::None;
            double latency = 0, loss = 0;   // weighted arithmetic means (ms, share)
            unsigned count = 0;
        };

        // The samples are the peers in touch. Those around count by distance; if none is around, all count alike; if there are none, "none".
        inline Result Combine(std::vector<Sample> const& peers)
        {
            Result r;
            bool anyAround = std::any_of(peers.begin(), peers.end(), [](Sample const& s) { return s.around; });
            double sum = 0, e = 0, l = 0;
            for (Sample const& s : peers)
            {
                if (anyAround && !s.around)
                    continue;
                double w = anyAround ? Weight(s.yards) : 1;
                sum += w;
                e += w * EffectiveLatency(s.rttMs, s.loss);
                l += w * (s.rttMs < 0 ? 1.0 : std::min(1.0, std::max(0.0, s.loss)));
                ++r.count;
            }
            if (!r.count)
                return r;
            r.state = anyAround ? State::Around : State::Everyone;
            r.latency = e / sum;
            r.loss = l / sum;
            return r;
        }

        inline Result Stopped() { Result r; r.state = State::Off; return r; }

        inline char const* StateName(State s) { return s == State::Around ? "around" : s == State::Everyone ? "everyone" : s == State::Off ? "off" : "none"; }

        // The text of the host command Quality: "state latency-ms loss count" (web/p2p.js splits it at the spaces)
        inline std::string Text(Result const& r)
        {
            char buf[96];
            std::snprintf(buf, sizeof buf, "%s %.1f %.4f %u", StateName(r.state), r.latency, r.loss, r.count);
            return buf;
        }
    }
}
