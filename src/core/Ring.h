/*
 * A single-producer single-consumer ring of length-prefixed records in a block of shared memory. The module's thread and the page's
 * JavaScript (web/p2p.js, which implements the same layout with Atomics) are the two ends; neither ever blocks the other.
 *
 * Layout (little endian, 32-bit words): [0] head (total bytes consumed), [1] tail (total bytes produced), [2] capacity (a power of two),
 * [3] doorbell (the producer increments it after a push; the consumer may futex-wait on it), then the data. Positions are free running
 * 32-bit counters; the index is position & (capacity - 1). A record is a u32 length followed by that many bytes; the bytes may be split
 * at the end of the data area and are copied in two pieces.
 */
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace p2p
{
    class Ring
    {
    public:
        static constexpr size_t HeaderBytes = 16;
        static size_t BytesFor(uint32_t capacity) { return HeaderBytes + capacity; }

        // 'memory' is HeaderBytes + capacity bytes; the producer's side initializes it once (Init), both sides then attach
        static void Init(void* memory, uint32_t capacity)
        {
            memset(memory, 0, HeaderBytes);
            reinterpret_cast<uint32_t*>(memory)[2] = capacity;
        }
        explicit Ring(void* memory) : _h(reinterpret_cast<std::atomic<uint32_t>*>(memory)), _data(reinterpret_cast<uint8_t*>(memory) + HeaderBytes)
        {
            _mask = _h[2].load(std::memory_order_relaxed) - 1;
        }

        // false when full (the record is not written)
        bool Push(uint8_t const* data, size_t size)
        {
            uint32_t capacity = _mask + 1, head = _h[0].load(std::memory_order_acquire), tail = _h[1].load(std::memory_order_relaxed);
            uint32_t need = static_cast<uint32_t>(size) + 4;
            if (need > capacity || capacity - (tail - head) < need)
                return false;
            uint32_t length = static_cast<uint32_t>(size);
            Copy(tail, reinterpret_cast<uint8_t const*>(&length), 4);
            Copy(tail + 4, data, size);
            _h[1].store(tail + need, std::memory_order_release);
            _h[3].fetch_add(1, std::memory_order_release);
            return true;
        }
        bool Push(std::vector<uint8_t> const& v) { return Push(v.data(), v.size()); }

        // false when empty
        bool Pop(std::vector<uint8_t>& out)
        {
            uint32_t head = _h[0].load(std::memory_order_relaxed), tail = _h[1].load(std::memory_order_acquire);
            if (head == tail)
                return false;
            uint32_t length = 0;
            Take(head, reinterpret_cast<uint8_t*>(&length), 4);
            out.resize(length);
            Take(head + 4, out.data(), length);
            _h[0].store(head + 4 + length, std::memory_order_release);
            return true;
        }
        std::atomic<uint32_t>* Doorbell() { return &_h[3]; }
        uint32_t Used() const { return _h[1].load(std::memory_order_acquire) - _h[0].load(std::memory_order_acquire); }

    private:
        void Copy(uint32_t at, uint8_t const* src, size_t n)
        {
            uint32_t i = at & _mask, first = std::min<size_t>(n, _mask + 1 - i);
            memcpy(_data + i, src, first);
            if (n > first)
                memcpy(_data, src + first, n - first);
        }
        void Take(uint32_t at, uint8_t* dst, size_t n)
        {
            uint32_t i = at & _mask, first = std::min<size_t>(n, _mask + 1 - i);
            memcpy(dst, _data + i, first);
            if (n > first)
                memcpy(dst + first, _data, n - first);
        }
        std::atomic<uint32_t>* _h;
        uint8_t* _data;
        uint32_t _mask;
    };
}
