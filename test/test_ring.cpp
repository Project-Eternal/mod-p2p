#include "Check.h"
#include "../src/core/Ring.h"
#include <thread>
using namespace p2p;

int main()
{
    std::vector<uint8_t> mem(Ring::BytesFor(1024));
    Ring::Init(mem.data(), 1024);
    Ring r(mem.data());
    std::vector<uint8_t> out;
    CHECK(!r.Pop(out));
    CHECK(r.Push({1, 2, 3}) && r.Pop(out) && out == std::vector<uint8_t>({1, 2, 3}));
    CHECK(r.Push({}) && r.Pop(out) && out.empty());
    CHECK(!r.Push(std::vector<uint8_t>(1021)));   // 1021 + 4 > 1024
    CHECK(r.Push(std::vector<uint8_t>(1020)) && !r.Push({1}) && r.Pop(out) && out.size() == 1020);
    // threads: records of changing sizes cross the wrap point; order and content are kept
    const int N = 200000;
    std::thread producer([&] {
        Ring p(mem.data());
        for (int i = 0; i < N;)
        {
            std::vector<uint8_t> v(1 + i % 97, static_cast<uint8_t>(i));
            if (p.Push(v)) ++i;
        }
    });
    int got = 0, bad = 0;
    while (got < N)
    {
        if (r.Pop(out))
        {
            if (out.size() != size_t(1 + got % 97) || out[0] != uint8_t(got) || out.back() != uint8_t(got)) ++bad;
            ++got;
        }
    }
    producer.join();
    CHECK(bad == 0);
    DONE();
}
