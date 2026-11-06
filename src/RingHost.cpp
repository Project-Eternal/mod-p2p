#include "RingHost.h"
#include <chrono>
#include <cstdlib>
#include <thread>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>
#endif

namespace p2p
{
    static constexpr uint32_t RingCapacity = 1u << 18;

    RingHost::RingHost()
    {
        for (void*& m : _memory)
        {
            m = aligned_alloc(16, Ring::BytesFor(RingCapacity));
            Ring::Init(m, RingCapacity);
        }
        _toHost = std::make_unique<Ring>(_memory[0]);
        _toModule = std::make_unique<Ring>(_memory[1]);
    }

    RingHost::~RingHost()
    {
        _toHost.reset();
        _toModule.reset();
        for (void* m : _memory)
            free(m);
    }

    bool RingHost::Attach()
    {
#ifdef __EMSCRIPTEN__
        // the page object Module.p2p: attach(toHostPtr, toModulePtr) returns 1 when it has a node to connect to
        _attached = MAIN_THREAD_EM_ASM_INT({
            var p = Module['p2p'];
            if (!p || !p.attach) return 0;
            return p.attach($0, $1, function () { return wasmMemory.buffer; }) ? 1 : 0;
        }, _memory[0], _memory[1]) != 0;
        // the page's setting: network events in the chat (on by default)
        _chatEvents = MAIN_THREAD_EM_ASM_INT({ var p = Module['p2p']; return p && p.chatEvents === 0 ? 0 : 1; }) != 0;
        // the page's private shard identifier, handed over here and nowhere else
        char shard[512] = {0};
        MAIN_THREAD_EM_ASM({ var p = Module['p2p']; var s = p && p.shard ? String(p.shard) : ''; stringToUTF8(s, $0, $1); }, shard, sizeof shard);
        _shard = shard;
#endif
        return _attached;
    }

    void RingHost::Command(HostCommand const& c)
    {
        if (!_attached)
            return;
        Bytes record = Encode(c);
        if (!_toHost->Push(record))
            ++_dropped;
        _kickPending = true;
    }

    bool RingHost::Poll(HostEvent& e)
    {
        if (!_attached || !_toModule->Pop(_scratch))
        {
            if (_kickPending)
                Kick();
            return false;
        }
        return Decode(_scratch.data(), _scratch.size(), e) || Poll(e);   // (a record that does not decode is dropped)
    }

    void RingHost::Kick()
    {
        _kickPending = false;
#ifdef __EMSCRIPTEN__
        MAIN_THREAD_ASYNC_EM_ASM({ var p = Module['p2p']; if (p && p.wake) p.wake(); });
#endif
    }

    void RingHost::Wait(unsigned ms)
    {
        if (!_attached)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            return;
        }
        std::atomic<uint32_t>* bell = _toModule->Doorbell();
        uint32_t now = bell->load(std::memory_order_acquire);
        if (now != _seen || _toModule->Used())
        {
            _seen = now;
            return;
        }
#ifdef __EMSCRIPTEN__
        emscripten_futex_wait(reinterpret_cast<volatile void*>(bell), now, ms);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
        _seen = bell->load(std::memory_order_acquire);
    }
}
