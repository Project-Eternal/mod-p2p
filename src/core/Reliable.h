/*
 * The reliable channel of one link, from end to end: what travels in it arrives once and in order, whatever route carried each frame.
 *
 * The sender numbers its messages, keeps every one that has not been acknowledged, and sends it again after a timeout derived from the
 * measured delay (on whatever route is current at that moment: the caller decides when it transmits). The receiver acknowledges with the
 * highest number received in order plus a bitmap of the next 64 that arrived early, hands messages up in order and drops duplicates.
 * A window of messages in flight is the limit; beyond it a message waits in the sender's queue instead of being lost.
 *
 * No clock and no I/O here: the owner passes the time in and transmits what Due() returns. The two ends of a link make one fresh
 * Reliable each (Reset) when the link comes up, because the link's keys, and so its sequence numbers, start over.
 */
#pragma once
#include "Codec.h"
#include <algorithm>
#include <deque>
#include <map>
#include <vector>

namespace p2p
{
    class ReliableStream
    {
    public:
        struct Config
        {
            size_t window = 256;           // messages in flight
            size_t windowBytes = 256 * 1024;
            size_t maxQueue = 8192;        // messages waiting beyond the window
            int64_t minRtoMs = 300, maxRtoMs = 8000;
        };

        ReliableStream() = default;
        explicit ReliableStream(Config const& c) : _config(c) { }
        void Reset()
        {
            _out.clear();
            _queue.clear();
            _nextOut = 1;
            _inFlightBytes = 0;
            _expect = 1;
            _early.clear();
            _ackDue = false;
            _earlyBytes = 0;
        }

        // ---- sender
        // 'message' is whatever the owner will deliver at the other end (type and body); false when the queue is full
        bool Queue(Bytes message)
        {
            if (_queue.size() >= _config.maxQueue)
                return false;
            _queue.push_back(std::move(message));
            return true;
        }
        size_t Pending() const { return _out.size() + _queue.size(); }
        size_t InFlight() const { return _out.size(); }

        struct Frame { uint64_t seq; Bytes const* message; };
        // The messages to transmit now: new ones that fit the window, and the ones whose timeout passed. 'rttMs' is the best estimate of the
        // round trip (negative if none yet).
        template <class F> void Due(int64_t now, int64_t rttMs, F&& transmit)
        {
            while (!_queue.empty() && _out.size() < _config.window && _inFlightBytes < _config.windowBytes)
            {
                Out o;
                o.message = std::move(_queue.front());
                _queue.pop_front();
                _inFlightBytes += o.message.size();
                o.sentAt = now;
                o.tries = 0;
                uint64_t seq = _nextOut++;
                auto it = _out.emplace(seq, std::move(o)).first;
                transmit(Frame{seq, &it->second.message});
            }
            for (auto& [seq, o] : _out)
            {
                if (now - o.sentAt < Rto(rttMs, o.tries))
                    continue;
                o.sentAt = now;
                ++o.tries;
                ++_resent;
                transmit(Frame{seq, &o.message});
            }
        }
        // the peer's acknowledgement: 'cumulative' = every message up to it arrived; bit i of 'early' = message cumulative + 2 + i arrived
        void OnAck(uint64_t cumulative, uint64_t early)
        {
            for (auto it = _out.begin(); it != _out.end() && it->first <= cumulative;)
            {
                _inFlightBytes -= std::min(_inFlightBytes, it->second.message.size());
                it = _out.erase(it);
            }
            for (int i = 0; i < 64; ++i)
                if (early >> i & 1)
                {
                    auto it = _out.find(cumulative + 2 + i);
                    if (it != _out.end())
                    {
                        _inFlightBytes -= std::min(_inFlightBytes, it->second.message.size());
                        _out.erase(it);
                    }
                }
        }
        uint64_t Resent() const { return _resent; }

        // ---- receiver
        // Takes a message numbered 'seq'; appends the messages that are now ready, in order, to 'ready'.
        void OnData(uint64_t seq, Bytes message, std::vector<Bytes>& ready)
        {
            _ackDue = true;   // (a duplicate is acknowledged again: the first acknowledgement may have been lost)
            if (seq < _expect || _early.count(seq))
                return;
            if (seq == _expect)
            {
                ready.push_back(std::move(message));
                ++_expect;
                for (auto it = _early.begin(); it != _early.end() && it->first == _expect;)
                {
                    _earlyBytes -= std::min(_earlyBytes, it->second.size());
                    ready.push_back(std::move(it->second));
                    it = _early.erase(it);
                    ++_expect;
                }
                return;
            }
            if (seq > _expect + 2 * _config.window || _earlyBytes > 2 * _config.windowBytes)
                return;   // (beyond what the sender may have in flight: it will send it again)
            _earlyBytes += message.size();
            _early.emplace(seq, std::move(message));
        }
        bool AckDue() const { return _ackDue; }
        // the acknowledgement to send (clears the flag)
        void MakeAck(uint64_t& cumulative, uint64_t& early)
        {
            _ackDue = false;
            cumulative = _expect - 1;
            early = 0;
            for (auto const& [seq, m] : _early)
            {
                uint64_t bit = seq - _expect - 1;   // (seq > _expect: bit 0 is _expect + 1, that is cumulative + 2)
                if (bit < 64)
                    early |= 1ull << bit;
            }
        }
        uint64_t Expected() const { return _expect; }

    private:
        struct Out { Bytes message; int64_t sentAt = 0; int tries = 0; };
        int64_t Rto(int64_t rttMs, int tries) const
        {
            int64_t base = rttMs < 0 ? 1000 : 2 * rttMs + 100;
            base = std::max(_config.minRtoMs, std::min(_config.maxRtoMs, base));
            return std::min(_config.maxRtoMs, base << std::min(tries, 4));
        }

        Config _config;
        std::map<uint64_t, Out> _out;
        std::deque<Bytes> _queue;
        uint64_t _nextOut = 1;
        size_t _inFlightBytes = 0;
        uint64_t _resent = 0;
        uint64_t _expect = 1;
        std::map<uint64_t, Bytes> _early;
        size_t _earlyBytes = 0;
        bool _ackDue = false;
    };

    /*
     * The keyed channel: what a newer message of the same key makes useless is never waited for. Each message has a key (for
     * example "the movement of character X") and a version that grows with every message sent. The sender keeps only the newest unacknowledged
     * message of each key and sends it again after the timeout; a newer one replaces it. The receiver applies a message only when its version is
     * newer than the last one it applied for that key, and acknowledges (key, version) pairs. Not ordered with anything else, so a lost message
     * holds up nothing. Like ReliableStream, no clock and no I/O; both ends make a fresh one (Reset) when the link comes up.
     */
    class KeyedStream
    {
    public:
        struct Config
        {
            size_t maxKeys = 8192;     // keys waiting for an acknowledgement
            int64_t minRtoMs = 300, maxRtoMs = 2000;
            size_t acksPerFrame = 100;
        };
        KeyedStream() = default;
        explicit KeyedStream(Config const& c) : _config(c) { }
        void Reset()
        {
            _out.clear();
            _version = 0;
            _seen.clear();
            _acks.clear();
        }

        // ---- sender: the message replaces the waiting one of the key; false when too many keys wait
        bool Set(uint64_t key, Bytes message)
        {
            auto it = _out.find(key);
            if (it == _out.end() && _out.size() >= _config.maxKeys)
                return false;
            Out& o = _out[key];
            o.version = ++_version;
            o.message = std::move(message);
            o.sent = false;
            o.tries = 0;
            return true;
        }
        size_t Pending() const { return _out.size(); }
        // transmit(key, version, message) for each new message and each one whose timeout passed
        template <class F> void Due(int64_t now, int64_t rttMs, F&& transmit)
        {
            for (auto& [key, o] : _out)
            {
                if (o.sent && now - o.sentAt < Rto(rttMs, o.tries))
                    continue;
                if (o.sent)
                {
                    ++o.tries;
                    ++_resent;
                }
                o.sent = true;
                o.sentAt = now;
                transmit(key, o.version, o.message);
            }
        }
        void OnAck(uint64_t key, uint64_t version)
        {
            auto it = _out.find(key);
            if (it != _out.end() && it->second.version <= version)
                _out.erase(it);
        }
        uint64_t Resent() const { return _resent; }

        // ---- receiver: true when the message is new for its key (the owner delivers it); every message is acknowledged
        bool OnData(uint64_t key, uint64_t version)
        {
            _acks[key] = std::max(_acks[key], version);
            auto it = _seen.find(key);
            if (it != _seen.end() && it->second >= version)
                return false;
            if (it == _seen.end() && _seen.size() >= 4 * _config.maxKeys)
                _seen.clear();   // (a key unseen for long: forgetting it can only accept an old duplicate once)
            _seen[key] = version;
            return true;
        }
        bool AckDue() const { return !_acks.empty(); }
        // up to acksPerFrame (key, version) pairs to acknowledge, removed from the list
        std::vector<std::pair<uint64_t, uint64_t>> TakeAcks()
        {
            std::vector<std::pair<uint64_t, uint64_t>> out;
            for (auto it = _acks.begin(); it != _acks.end() && out.size() < _config.acksPerFrame;)
            {
                out.emplace_back(it->first, it->second);
                it = _acks.erase(it);
            }
            return out;
        }

    private:
        struct Out { uint64_t version = 0; Bytes message; int64_t sentAt = 0; int tries = 0; bool sent = false; };
        int64_t Rto(int64_t rttMs, int tries) const
        {
            int64_t base = rttMs < 0 ? 1000 : 2 * rttMs + 100;
            base = std::max(_config.minRtoMs, std::min(_config.maxRtoMs, base));
            return std::min(_config.maxRtoMs, base << std::min(tries, 1));
        }
        Config _config;
        std::map<uint64_t, Out> _out;
        uint64_t _version = 0, _resent = 0;
        std::map<uint64_t, uint64_t> _seen, _acks;
    };
}
